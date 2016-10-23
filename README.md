# hotplug-state-machine
Hotplug kernel driver that manages all intricacies of connecting/disconnecting an external display interface (e.g. hdmi, displayport) to a system.

From HW perspective, hotplug is a trivial plug/unplug operation. Nothing exquisite about it. However, in SW, things get more complicated as hotplug events are used to trigger various workhorses in kernel and userspace.

The SW driver presented here handles following corner cases of hotplug ecosystem:
- Manage spurious hotplug events:
	Display panels are allowed to send hotplug events as and when they please, depending on their panel IC implementation. We have specially observed panels sending out spurious events just when host starts pixel transmission. This observation is on open market panels and is not limited to any specific vendor or product model. Also, the phenomenon is not limited to any interface, observed on both hdmi and displayport.
	
	It's expected of drivers to abstract such HW specific complexities from rest of kernel and userspace. Otherwise, it causes unnecessary confusion and possible conflict on realizing current state of hotplug, across different entities of the system. Imagine userspace rendering applications and audio framework perceiving opposite hotplug states. Alternate solution is to keep spurious hotplug filter logic in all dependent drivers/framework/userspace which is unnecessary boilerplate code spread across the system.

- Non standard delay between plug and unplug events:
	Let's take hdmi example. Specification says that each unplug event should be 100ms minimum. In real world, there are numerous panels from major players in the market which do not strictly adhere to this requirement. You might say that it would be super human to manually unplug and plug in less that 100ms. Indeed, you are right but what if it's being done by an automated system. Mutiple panels might be connected to a switch in a test farm and an automated test might try to electronically switch from one panel to other. Unplug and plug event being generated here can easily be in less than 100ms.

- Seamless display transition from bootloader to kernel:
	It's very typical for systems to have separate drivers for same controller in bootloader and kernel. This means that kernel would try to reinitialize the controller on boot. If your bootloader renders on screen as well, typically a logo, it's likely that user would see a flicker on screen during bootloder to kernel transition. This is the side effect of kernel trying to program the display controller from scratch.

The hotplug driver presented here is based on state machine. Each state is scheduled on a kernel thread. This modularity assures clean transition between states and clean abort in case of mishap like unplug event or edid read failure. Explanation of individual states is documented in hpd.h file.

The client drivers i.e. the display interface drivers like hdmi, displayport can pump hotplug event to this driver at anytime and expect correct outcome. No extra checks needed on client side. All client entry points into the driver is explained in hpd.h file.

To keep this driver agnostic of any display interface or architecture or platform, all non generic items are pushed to struct hpd_ops. Client driver is expected to implement these operations. All operations are explained in hpd.h file. Not all of them need mandatory implementation by client driver.

For better understanding of how various states are working in tandem refer hpd.jpg
