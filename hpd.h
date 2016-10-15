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

#ifndef __DISPLAY_HPD_H__
#define __DISPLAY_HPD_H__

/*
 * Client specific hpd operations. Some of the work done by
 * hpd driver is arch, platform or display interface specific.
 * Additionally, clients might want to add custom functionalities to
 * hpd states. Below operations give an opportunity to client driver
 * to achieve the same. Additionally keeps hpd driver portable,
 * extendible and not tied to any specific display interface.
 */
struct hpd_ops {
	/*
	 * Invoked during hpd state machine init. Client driver can do
	 * it's own custom initialization here. Implementation optional.
	 */
	void (*init)(void *drv_data);

	/*
	 * Returns current hpd status i.e. asserted or de-asserted.
	 * Implementation by client driver is mandatory.
	 */
	bool (*get_hpd_state)(void *drv_data);

	/*
	 * Panel is disconnected here.
	 * Client driver can disable display subsystem and notify others.
	 * Implementaion optional but it would be very naive of client
	 * to not implement this functional call.
	 */
	void (*disable)(void *drv_data);

	/*
	 * Client specific panel edid read. Implementation mandatory.
	 * Return true for edid read success, false for failure.
	 */
	bool (*edid_read)(void *drv_data);

	/*
	 * New panel has been connected to the system and
	 * edid is available. Tell others about it and enable
	 * display sub-system. Implementation mandatory. */
	void (*edid_ready)(void *drv_data);

	/*
	 * Hpd dropped but came back again in < HPD_DROP_TIMEOUT_MS.
	 * Checks for any edid change. Implementation mandatory.
	 * Return -1 for failure, 1 on edid change and 0 on same edid. 
	 */
	int (*edid_recheck)(void *drv_data);
	
	/* Release resources acquired during init. Implementation optional. */
	void (*shutdown)(void *drv_data);
};

struct hpd_data {
	struct delayed_work dwork;
	int shutdown;
	int state;
	int pending_hpd_evt;
	void *drv_data;
	struct hpd_ops *ops;

	int edid_reads;

	struct mutex lock;
};

enum {
	/*
	 * The initial state for the state machine. When entering RESET, we
	 * shut down all output and then proceed to the CHECK_PLUG state after a
	 * short debounce delay.
	 */
	STATE_HPD_RESET = 0,

	/*
	 * After the debounce delay, check the status of the HPD line.  If its
	 * low, then the cable is unplugged and we go directly to DONE_DISABLED.
	 * If it is high, then the cable is plugged and we proceed to CHECK_EDID
	 * in order to read the EDID and figure out the next step.
	 */
	STATE_PLUG,

	/*
	 * CHECK_EDID is the state we stay in attempting to read the EDID
	 * information after we check the plug state and discover that we are
	 * plugged in. If we max out our retries and fail to read the EDID, we
	 * move to DONE_DISABLED. If we successfully read the EDID, we move on
	 * to DONE_ENABLE and signal others in kernel and userspace that a panel
	 * has been plugged to the system.
	 */
	STATE_CHECK_EDID,

	/*
	 * DONE_DISABLED is the state we stay in after being reset and either
	 * discovering that no cable is plugged in or after we think a cable is
	 * plugged in but fail to read EDID.
	 */
	STATE_DONE_DISABLED,

	/*
	 * DONE_ENABLED is the state we stay in after being reset and disovering
	 * a valid EDID at the other end of a plugged cable.
	 */
	STATE_DONE_ENABLED,

	/*
	 * Some sinks will drop HPD as soon as display signals from host start up.
	 * They will hold HPD low for about a second and then re-assert it. If
	 * source simply holds steady and does not disable the lanes, the
	 * sink seems to accept the video mode after having gone out for coffee
	 * for a bit. This seems to be the behavior of various sources which
	 * work with panels like this, so it is the behavior we emulate here.
	 * If HPD drops while we are in DONE_ENABLED, set a timer for 1.5
	 * seconds and transition to WAIT_FOR_HPD_REASSERT. If HPD has not come
	 * back within this time limit, then go ahead and transition to RESET
	 * and shut the system down. If HPD does come back within this time
	 * limit, then check the EDID again. If it has not changed, then we
	 * assume that we are still hooked to the same panel and just go back to
	 * DONE_ENABLED. If the EDID fails to read or has changed, we
	 * transition to RESET and start the state machine all over again.
	 */
	STATE_WAIT_FOR_HPD_REASSERT,

	/*
	 * RECHECK_EDID is the state we stay in while attempting to re-read the
	 * EDID following an HPD drop and re-assert which occurs while we are in
	 * the DONE_ENABLED state. see HPD_STATE_DONE_WAIT_FOR_HPD_REASSERT
	 * for more details.
	 */
	STATE_RECHECK_EDID,

	/*
	 * Initial state at boot that checks if display subsystem is already
	 * initialized by bootloader and not go to HPD_STATE_RESET which would
	 * disable display subsystem and cause blanking on screen while
	 * transitioning from bootloader to kernel. Blanking is perceivable
	 * only if bootloder is rendering anything on screen.
	 */
	STATE_INIT_FROM_BOOTLOADER,

	/*
	 * STATE_COUNT must be the final state in the enum.
	 * 1) Do not add states after STATE_COUNT.
	 * 2) Do not assign explicit values to the states.
	 * 3) Do not reorder states in the list without reordering the dispatch
	 *    table in hpd.c
	 */
	HPD_STATE_COUNT,
};

/*
 * initialize hpd workhorse
 * 
 * @data: hpd data to be initialized
 * @drv_data: client driver private data
 * @ops: client dependent hpd operations
 *
 * Here client is the driver using services of the hpd state machine.
 * Most likely a display interface driver e.g. hdmi, displayport
 */
void hpd_init(struct hpd_data *data, void *drv_data, struct hpd_ops *ops);

/* release all resources acquired during hpd_init */
void hpd_shutdown(struct hpd_data *data);

/* raise a request to process hotplug event i.e. plug or unplug */
void hpd_set_pending_evt(struct hpd_data *data);

#endif
