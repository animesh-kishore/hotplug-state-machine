/*
 * HPD: hotplug detect state machine
 *
 * Author: Animesh Kishore <animesh.kishore@gmail.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 3, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include "hpd.h"

#define MAX_EDID_READ_ATTEMPTS 5

#define HPD_STABILIZE_MS 40
#define HPD_DROP_TIMEOUT_MS 1500
#define CHECK_PLUG_STATE_DELAY_MS 10
#define CHECK_EDID_DELAY_MS 60

static const char * const state_names[] = {
	"Reset",
	"Check Plug",
	"Check EDID",
	"Disabled",
	"Enabled",
	"Wait for HPD reassert",
	"Recheck EDID",
	"Takeover from bootloader",
};

static void set_hpd_state(struct hpd_data *data,
			int target_state, int resched_time);

static void hpd_disable(struct hpd_data *data)
{
	if (data->ops->disable)
		data->ops->disable(data->drv_data);
}

static void hpd_reset_state(struct hpd_data *data)
{
	/*
	 * Shut everything down, then schedule a
	 * check of the plug state in the near future.
	 */
	hpd_disable(data);
	set_hpd_state(data, STATE_PLUG,
			CHECK_PLUG_STATE_DELAY_MS);
}

static void hpd_plug_state(struct hpd_data *data)
{
	if (data->ops->get_hpd_state(data->drv_data)) {
		/*
		 * Looks like there is something plugged in.
		 * Get ready to read the sink's EDID information.
		 */
		data->edid_reads = 0;

		set_hpd_state(data, STATE_CHECK_EDID,
				CHECK_EDID_DELAY_MS);
	} else {
		/*
		 * Nothing plugged in, so we are finished. Go to the
		 * DONE_DISABLED state and stay there until the next HPD event.
		 */
		hpd_disable(data);
		set_hpd_state(data, STATE_DONE_DISABLED, -1);
	}
}

static void edid_check_state(struct hpd_data *data)
{
	if (!data->ops->get_hpd_state(data->drv_data)) {
		/* hpd dropped - stop EDID read */
		pr_info("hpd: dropped, abort EDID read\n");
		goto end_disabled;
	}

	if (!data->ops->edid_read(data->drv_data)) {
		/*
		 * Failed to read EDID. If we still have retry attempts left,
		 * schedule another attempt. Otherwise give up and just go to
		 * the disabled state.
		 */
		data->edid_reads++;
		if (data->edid_reads >= MAX_EDID_READ_ATTEMPTS) {
			pr_info("hpd: EDID read failed %d times. Giving up.\n",
				data->edid_reads);
			goto end_disabled;
		} else {
			set_hpd_state(data, STATE_CHECK_EDID,
					CHECK_EDID_DELAY_MS);
		}

		return;
	}

	if (data->ops->edid_ready)
		data->ops->edid_ready(data->drv_data);


	set_hpd_state(data, STATE_DONE_ENABLED, -1);

	return;
end_disabled:
	hpd_disable(data);
	set_hpd_state(data, STATE_DONE_DISABLED, -1);
}

static void wait_for_hpd_reassert_state(struct hpd_data *data)
{
	/*
	 * Looks like HPD dropped and really did stay low.
	 * Go ahead and reset the system.
	 */
	set_hpd_state(data, STATE_HPD_RESET, 0);
}

static void edid_recheck_state(struct hpd_data *data)
{
	int tgt_state, timeout, status;

	tgt_state = STATE_HPD_RESET;
	timeout = 0;
	
	status = data->ops->edid_recheck(data->drv_data);

	if (status == -1) {
		/*
		 * Failed to read EDID. If we still have retry attempts left,
		 * schedule another attempt. Otherwise give up and reset;
		 */
		data->edid_reads++;
		if (data->edid_reads >= MAX_EDID_READ_ATTEMPTS) {
			pr_info("hpd: EDID retry %d times. Giving up.\n",
				data->edid_reads);
		} else {
			tgt_state = STATE_RECHECK_EDID;
			timeout = CHECK_EDID_DELAY_MS;
		}
	} else if (status == 0) {
		/*
		 * Successful read and EDID is unchanged, just go back to
		 * the DONE_ENABLED state and do nothing.
		 */
		pr_info("hpd: No EDID change, taking no action.\n");
		tgt_state = STATE_DONE_ENABLED;
		timeout = -1;
	}

	set_hpd_state(data, tgt_state, timeout);
}

typedef void (*dispatch_func_t)(struct hpd_data *data);
static const dispatch_func_t state_machine_dispatch[] = {
	hpd_reset_state,			/* STATE_HPD_RESET */
	hpd_plug_state,				/* STATE_PLUG */
	edid_check_state,			/* STATE_CHECK_EDID */
	NULL,						/* STATE_DONE_DISABLED */
	NULL,						/* STATE_DONE_ENABLED */
	wait_for_hpd_reassert_state,/* STATE_WAIT_FOR_HPD_REASSERT */
	edid_recheck_state,			/* STATE_RECHECK_EDID */
	NULL,						/* STATE_INIT_FROM_BOOTLOADER */
};

static void handle_hpd_evt(struct hpd_data *data, int cur_hpd)
{
	int tgt_state;
	int timeout = 0;

	if ((STATE_DONE_ENABLED == data->state) && !cur_hpd) {
		/*
		 * HPD dropped while we were in DONE_ENABLED. Hold
		 * steady and wait to see if it comes back.
		 */
		tgt_state = STATE_WAIT_FOR_HPD_REASSERT;
		timeout = HPD_DROP_TIMEOUT_MS;
	} else if (STATE_WAIT_FOR_HPD_REASSERT == data->state &&
		cur_hpd) {
		/*
		 * Looks like HPD dropped and eventually came back. Re-read the
		 * EDID and reset the system only if the EDID has changed.
		 */
		data->edid_reads = 0;
		tgt_state = STATE_RECHECK_EDID;
		timeout = CHECK_EDID_DELAY_MS;
	} else if (STATE_DONE_ENABLED == data->state && cur_hpd) {
		/*
		 * Looks like HPD dropped but came back quickly,
		 * ignore it.
		 */
		pr_info("hpd: ignoring bouncing hpd\n");
		return;
	} else if (STATE_INIT_FROM_BOOTLOADER == data->state && cur_hpd) {
		/*
		 * We follow the same protocol as STATE_HPD_RESET
		 * but avoid actually entering that state so
		 * we do not actively disable HPD. Worker will check HPD
		 * level again when it's woke up after 40ms.
		 */
		tgt_state = STATE_PLUG;
		timeout = HPD_STABILIZE_MS;
	} else {
		/*
		 * Looks like there was HPD activity while we were neither
		 * waiting for it to go away during steady state output, nor
		 * looking for it to come back after such an event. Wait until
		 * HPD has been steady for at least 40 mSec, then restart the
		 * state machine.
		 */
		tgt_state = STATE_HPD_RESET;
		timeout = HPD_STABILIZE_MS;
	}

	set_hpd_state(data, tgt_state, timeout);
}

static void hpd_worker(struct work_struct *work)
{
	int pending_hpd_evt, cur_hpd;
	struct hpd_data *data = container_of(
					to_delayed_work(work),
					struct hpd_data, dwork);

	/*
	 * Observe and clear pending flag
	 * and latch the current HPD state.
	 */
	mutex_lock(&data->lock);
	pending_hpd_evt = data->pending_hpd_evt;
	data->pending_hpd_evt = 0;
	mutex_unlock(&data->lock);
	cur_hpd = data->ops->get_hpd_state(data->drv_data);

	pr_info("hpd: state %d (%s), hpd %d, pending_hpd_evt %d\n",
		data->state, state_names[data->state],
		cur_hpd, pending_hpd_evt);

	if (pending_hpd_evt) {
		/*
		 * If we were woken up because of HPD activity, just schedule
		 * the next appropriate task and get out.
		 */
		handle_hpd_evt(data, cur_hpd);
	} else if (data->state < ARRAY_SIZE(state_machine_dispatch)) {
		dispatch_func_t func = state_machine_dispatch[data->state];

		if (NULL == func)
			pr_warn("hpd: NULL state handler in state %d\n",
				data->state);
		else
			func(data);
	} else {
		pr_warn("hpd: unexpected state scheduled %d",
			data->state);
	}
}

static void sched_hpd_work(struct hpd_data *data, int resched_time)
{
	cancel_delayed_work(&data->dwork);

	if (resched_time >= 0 && !data->shutdown)
		schedule_delayed_work(&data->dwork,
				msecs_to_jiffies(resched_time));
}

static void set_hpd_state(struct hpd_data *data,
			int target_state, int resched_time)
{
	mutex_lock(&data->lock);

	pr_info("hpd: switching from state %d (%s) to state %d (%s)\n",
		data->state, state_names[data->state],
		target_state, state_names[target_state]);
	data->state = target_state;

	/*
	 * If the pending_hpd_evt flag is already set, don't bother to
	 * reschedule the state machine worker.  We should be able to assert
	 * that there is a worker callback already scheduled, and that it is
	 * scheduled to run immediately. This is particularly important when
	 * making the transition to the steady state ENABLED or DISABLED states.
	 * If an HPD event occurs while the worker is in flight, after the
	 * worker checks the state of the pending HPD flag, and then the state
	 * machine transitions to ENABLE or DISABLED, the system would end up
	 * canceling the callback to handle the HPD event were it not for this
	 * check.
	 */
	if (!data->pending_hpd_evt)
		sched_hpd_work(data, resched_time);

	mutex_unlock(&data->lock);
}

void hpd_shutdown(struct hpd_data *data)
{
	data->shutdown = 1;
	cancel_delayed_work_sync(&data->dwork);
	
	if (data->ops->shutdown)
		data->ops->shutdown(data->drv_data);
}

void hpd_set_pending_evt(struct hpd_data *data)
{
	mutex_lock(&data->lock);

	/* We always schedule work any time there is a pending HPD event */
	data->pending_hpd_evt = 1;
	sched_hpd_work(data, 0);

	mutex_unlock(&data->lock);
}

void hpd_init(struct hpd_data *data, void *drv_data, struct hpd_ops *ops)
{
	BUG_ON(!data || !ops ||
		!ops->get_hpd_state ||
		!ops->edid_read ||
		!ops->edid_ready ||
		!ops->edid_recheck);

	if (ops->init)
		ops->init(drv_data);

	data->drv_data = drv_data;
	data->state = STATE_INIT_FROM_BOOTLOADER;
	data->pending_hpd_evt = 0;
	data->shutdown = 0;
	data->ops = ops;
	data->edid_reads = 0;

	mutex_init(&data->lock);

	INIT_DELAYED_WORK(&data->dwork, hpd_worker);
}
