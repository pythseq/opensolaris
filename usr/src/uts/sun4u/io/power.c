/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

/*
 *	Power Button Driver
 *
 *	This driver handles interrupt generated by the power button on
 *	platforms with "power" device node which has "button" property.
 *	Currently, these platforms are:
 *
 *		Ultra-5_10, Ultra-80, Sun-Blade-100, Sun-Blade-150,
 *		Sun-Blade-1500, Sun-Blade-2500,
 *		Sun-Fire-V210, Sun-Fire-V240, Netra-240
 *
 *	Only one instance is allowed to attach.  In order to know when
 *	an application that has opened the device is going away, a new
 *	minor clone is created for each open(9E) request.  There are
 *	allocations for creating minor clones between 1 and 255.  The ioctl
 *	interface is defined by pbio(7I) and approved as part of
 *	PSARC/1999/393 case.
 */

#include <sys/types.h>
#include <sys/conf.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/ddi_impldefs.h>
#include <sys/cmn_err.h>
#include <sys/errno.h>
#include <sys/modctl.h>
#include <sys/machsystm.h>
#include <sys/open.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/pbio.h>

/*
 * Maximum number of clone minors that is allowed.  This value
 * is defined relatively low to save memory.
 */
#define	POWER_MAX_CLONE	256

/*
 * Minor number is instance << 8 + clone minor from range 1-255; clone 0
 * is reserved for "original" minor.
 */
#define	POWER_MINOR_TO_CLONE(minor) ((minor) & (POWER_MAX_CLONE - 1))

/*
 * Power Button Abort Delay
 */
#define	ABORT_INCREMENT_DELAY	10

/*
 * Driver global variables
 */
static void *power_state;
static int power_inst = -1;

static hrtime_t	power_button_debounce = NANOSEC/MILLISEC*10;
static hrtime_t power_button_abort_interval = 1.5 * NANOSEC;
static int	power_button_abort_presses = 3;
static int	power_button_abort_enable = 1;
static int	power_button_enable = 1;

static int	power_button_pressed = 0;
static int	power_button_cancel = 0;
static int	power_button_timeouts = 0;
static int	timeout_cancel = 0;
static int	additional_presses = 0;

/*
 * Function prototypes
 */
static int power_attach(dev_info_t *, ddi_attach_cmd_t);
static int power_detach(dev_info_t *, ddi_detach_cmd_t);
static int power_getinfo(dev_info_t *, ddi_info_cmd_t, void *, void **);
static int power_open(dev_t *, int, int, cred_t *);
static int power_close(dev_t, int, int, cred_t *);
static int power_ioctl(dev_t, int, intptr_t, int, cred_t *, int *);
static int power_chpoll(dev_t, short, int, short *, struct pollhead **);
static uint_t power_high_intr(caddr_t);
static uint_t power_soft_intr(caddr_t);
static uint_t power_issue_shutdown(caddr_t);
static void power_timeout(caddr_t);
static void power_log_message(void);

/*
 * Structure used in the driver
 */
static struct power_soft_state {
	dev_info_t	*dip;		/* device info pointer */
	kmutex_t	power_mutex;	/* mutex lock */
	kmutex_t	power_intr_mutex; /* interrupt mutex lock */
	ddi_iblock_cookie_t soft_iblock_cookie; /* holds interrupt cookie */
	ddi_iblock_cookie_t high_iblock_cookie; /* holds interrupt cookie */
	ddi_softintr_t	softintr_id;	/* soft interrupt id */
	uchar_t		clones[POWER_MAX_CLONE]; /* array of minor clones */
	int		monitor_on;	/* clone monitoring the button event */
					/* clone 0 indicates no one is */
					/* monitoring the button event */
	pollhead_t	pollhd;		/* poll head struct */
	int		events;		/* bit map of occured events */
	int		shutdown_pending; /* system shutdown in progress */
	ddi_acc_handle_t power_rhandle; /* power button register handle */
	uint8_t		*power_btn_reg;	/* power button register address */
	uint8_t		power_btn_bit;	/* power button register bit */
	boolean_t	power_regs_mapped; /* flag to tell if regs mapped */
	boolean_t	power_btn_ioctl; /* flag to specify ioctl request */
};

static int power_setup_regs(struct power_soft_state *softsp);
static void power_free_regs(struct power_soft_state *softsp);

/*
 * Configuration data structures
 */
static struct cb_ops power_cb_ops = {
	power_open,		/* open */
	power_close,		/* close */
	nodev,			/* strategy */
	nodev,			/* print */
	nodev,			/* dump */
	nodev,			/* read */
	nodev,			/* write */
	power_ioctl,		/* ioctl */
	nodev,			/* devmap */
	nodev,			/* mmap */
	nodev,			/* segmap */
	power_chpoll,		/* poll */
	ddi_prop_op,		/* cb_prop_op */
	NULL,			/* streamtab */
	D_MP | D_NEW,		/* Driver compatibility flag */
	CB_REV,			/* rev */
	nodev,			/* cb_aread */
	nodev			/* cb_awrite */
};

static struct dev_ops power_ops = {
	DEVO_REV,		/* devo_rev, */
	0,			/* refcnt */
	power_getinfo,		/* getinfo */
	nulldev,		/* identify */
	nulldev,		/* probe */
	power_attach,		/* attach */
	power_detach,		/* detach */
	nodev,			/* reset */
	&power_cb_ops,		/* cb_ops */
	(struct bus_ops *)NULL,	/* bus_ops */
	NULL			/* power */
};

static struct modldrv modldrv = {
	&mod_driverops,		/* Type of module.  This one is a driver */
	"power button driver v%I%",	/* name of module */
	&power_ops,		/* driver ops */
};

static struct modlinkage modlinkage = {
	MODREV_1,
	(void *)&modldrv,
	NULL
};

/*
 * These are the module initialization routines.
 */

int
_init(void)
{
	int error;

	if ((error = ddi_soft_state_init(&power_state,
	    sizeof (struct power_soft_state), 0)) != 0)
		return (error);

	if ((error = mod_install(&modlinkage)) != 0)
		ddi_soft_state_fini(&power_state);

	return (error);
}

int
_fini(void)
{
	int error;

	if ((error = mod_remove(&modlinkage)) == 0)
		ddi_soft_state_fini(&power_state);

	return (error);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}

/*ARGSUSED*/
static int
power_getinfo(dev_info_t *dip, ddi_info_cmd_t infocmd, void *arg,
    void **result)
{
	struct power_soft_state *softsp;

	if (power_inst == -1)
		return (DDI_FAILURE);

	switch (infocmd) {
	case DDI_INFO_DEVT2DEVINFO:
		if ((softsp = ddi_get_soft_state(power_state, power_inst))
		    == NULL)
			return (DDI_FAILURE);
		*result = (void *)softsp->dip;
		return (DDI_SUCCESS);

	case DDI_INFO_DEVT2INSTANCE:
		*result = (void *)power_inst;
		return (DDI_SUCCESS);

	default:
		return (DDI_FAILURE);
	}
}

static int
power_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	struct power_soft_state *softsp;

	switch (cmd) {
	case DDI_ATTACH:
		break;
	case DDI_RESUME:
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}

	/*
	 * If the power node doesn't have "button" property, quietly
	 * fail to attach.
	 */
	if (ddi_prop_exists(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
	    "button") == 0)
		return (DDI_FAILURE);

	if (power_inst != -1)
		return (DDI_FAILURE);

	power_inst = ddi_get_instance(dip);

	if (ddi_soft_state_zalloc(power_state, power_inst) != DDI_SUCCESS)
		return (DDI_FAILURE);

	if (ddi_create_minor_node(dip, "power_button", S_IFCHR,
	    (power_inst << 8) + 0, "ddi_power_button", 0) != DDI_SUCCESS)
		return (DDI_FAILURE);

	softsp = ddi_get_soft_state(power_state, power_inst);
	softsp->dip = dip;

	if (power_setup_regs(softsp) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "power_attach: failed to setup registers");
		goto error;
	}

	if (ddi_get_iblock_cookie(dip, 0,
	    &softsp->high_iblock_cookie) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "power_attach: ddi_get_soft_iblock_cookie "
		    "failed.");
		goto error;
	}
	mutex_init(&softsp->power_intr_mutex, NULL, MUTEX_DRIVER,
	    softsp->high_iblock_cookie);

	if (ddi_add_intr(dip, 0, &softsp->high_iblock_cookie, NULL,
	    power_high_intr, (caddr_t)softsp) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "power_attach: failed to add high-level "
		    " interrupt handler.");
		mutex_destroy(&softsp->power_intr_mutex);
		goto error;
	}

	if (ddi_get_soft_iblock_cookie(dip, DDI_SOFTINT_LOW,
	    &softsp->soft_iblock_cookie) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "power_attach: ddi_get_soft_iblock_cookie "
		    "failed.");
		mutex_destroy(&softsp->power_intr_mutex);
		ddi_remove_intr(dip, 0, NULL);
		goto error;
	}

	mutex_init(&softsp->power_mutex, NULL, MUTEX_DRIVER,
	    (void *)softsp->soft_iblock_cookie);

	if (ddi_add_softintr(dip, DDI_SOFTINT_LOW, &softsp->softintr_id,
	    NULL, NULL, power_soft_intr, (caddr_t)softsp) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "power_attach: failed to add soft "
		    "interrupt handler.");
		mutex_destroy(&softsp->power_mutex);
		mutex_destroy(&softsp->power_intr_mutex);
		ddi_remove_intr(dip, 0, NULL);
		goto error;
	}

	ddi_report_dev(dip);

	return (DDI_SUCCESS);

error:
	power_free_regs(softsp);
	ddi_remove_minor_node(dip, "power_button");
	ddi_soft_state_free(power_state, power_inst);
	return (DDI_FAILURE);
}

/*ARGSUSED*/
/*
 * This driver doesn't detach.
 */
static int
power_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	/*
	 * Since the "power" node has "reg" property, as part of
	 * the suspend operation, detach(9E) entry point is called.
	 * There is no state to save, since this register is used
	 * by OBP to power off the system and the state of the
	 * power off is preserved by hardware.
	 */
	return ((cmd == DDI_SUSPEND) ? DDI_SUCCESS :
	    DDI_FAILURE);
}

/*
 * Handler for the high-level interrupt.
 */
static uint_t
power_high_intr(caddr_t arg)
{
	struct power_soft_state *softsp = (struct power_soft_state *)arg;
	ddi_acc_handle_t hdl = softsp->power_rhandle;
	uint8_t		reg;

	hrtime_t tstamp;
	static hrtime_t o_tstamp = 0;
	static hrtime_t power_button_tstamp = 0;
	static int power_button_cnt;

	if (softsp->power_regs_mapped) {
		mutex_enter(&softsp->power_intr_mutex);
		reg = ddi_get8(hdl, softsp->power_btn_reg);
		if (reg & softsp->power_btn_bit) {
			reg &= softsp->power_btn_bit;
			ddi_put8(hdl, softsp->power_btn_reg, reg);
			(void) ddi_get8(hdl, softsp->power_btn_reg);
		} else {
			if (!softsp->power_btn_ioctl) {
				mutex_exit(&softsp->power_intr_mutex);
				return (DDI_INTR_CLAIMED);
			}
			softsp->power_btn_ioctl = B_FALSE;
		}
		mutex_exit(&softsp->power_intr_mutex);
	}

	tstamp = gethrtime();

	/* need to deal with power button debounce */
	if (o_tstamp && (tstamp - o_tstamp) < power_button_debounce) {
		o_tstamp = tstamp;
		return (DDI_INTR_CLAIMED);
	}
	o_tstamp = tstamp;

	power_button_cnt++;

	mutex_enter(&softsp->power_intr_mutex);
	power_button_pressed++;
	mutex_exit(&softsp->power_intr_mutex);

	/*
	 * If power button abort is enabled and power button was pressed
	 * power_button_abort_presses times within power_button_abort_interval
	 * then call abort_sequence_enter();
	 */
	if (power_button_abort_enable) {
		if (power_button_abort_presses == 1 ||
		    tstamp < (power_button_tstamp +
		    power_button_abort_interval)) {
			if (power_button_cnt == power_button_abort_presses) {
				mutex_enter(&softsp->power_intr_mutex);
				power_button_cancel += power_button_timeouts;
				power_button_pressed = 0;
				mutex_exit(&softsp->power_intr_mutex);
				power_button_cnt = 0;
				abort_sequence_enter("Power Button Abort");
				return (DDI_INTR_CLAIMED);
			}
		} else {
			power_button_cnt = 1;
			power_button_tstamp = tstamp;
		}
	}

	if (!power_button_enable)
		return (DDI_INTR_CLAIMED);

	/* post softint to issue timeout for power button action */
	if (softsp->softintr_id != NULL)
		ddi_trigger_softintr(softsp->softintr_id);

	return (DDI_INTR_CLAIMED);
}

/*
 * Handle the softints....
 *
 * If only one softint is posted for several button presses, record
 * the number of additional presses just incase this was actually not quite
 * an Abort sequence so that we can log this event later.
 *
 * Issue a timeout with a duration being a fraction larger than
 * the specified Abort interval inorder to perform a power down if required.
 */
static uint_t
power_soft_intr(caddr_t arg)
{
	struct power_soft_state *softsp = (struct power_soft_state *)arg;

	if (!power_button_abort_enable)
		return (power_issue_shutdown(arg));

	mutex_enter(&softsp->power_intr_mutex);
	if (!power_button_pressed) {
		mutex_exit(&softsp->power_intr_mutex);
		return (DDI_INTR_CLAIMED);
	}

	/*
	 * Schedule a timeout to do the necessary
	 * work for shutdown, only one timeout for
	 * n presses if power button was pressed
	 * more than once before softint fired
	 */
	if (power_button_pressed > 1)
		additional_presses += power_button_pressed - 1;

	timeout_cancel = 0;
	power_button_pressed = 0;
	power_button_timeouts++;
	mutex_exit(&softsp->power_intr_mutex);
	(void) timeout((void(*)(void *))power_timeout,
	    softsp, NSEC_TO_TICK(power_button_abort_interval) +
	    ABORT_INCREMENT_DELAY);

	return (DDI_INTR_CLAIMED);
}

/*
 * Upon receiving a timeout the following is determined:
 *
 * If an  Abort sequence was issued, then we cancel all outstanding timeouts
 * and additional presses prior to the Abort sequence.
 *
 * If we had multiple timeouts issued and the abort sequence was not met,
 * then we had more than one button press to power down the machine. We
 * were probably trying to issue an abort. So log a message indicating this
 * and cancel all outstanding timeouts.
 *
 * If we had just one timeout and the abort sequence was not met then
 * we really did want to power down the machine, so call power_issue_shutdown()
 * to do the work and schedule a power down
 */
static void
power_timeout(caddr_t arg)
{
	struct power_soft_state *softsp = (struct power_soft_state *)arg;
	static int first = 0;

	/*
	 * Abort was generated cancel all outstanding power
	 * button timeouts
	 */
	mutex_enter(&softsp->power_intr_mutex);
	if (power_button_cancel) {
		power_button_cancel--;
		power_button_timeouts--;
		if (!first) {
			first++;
			additional_presses = 0;
		}
		mutex_exit(&softsp->power_intr_mutex);
		return;
	}
	first = 0;

	/*
	 * We get here if the timeout(s) have fired and they were
	 * not issued prior to an abort.
	 *
	 * If we had more than one press in the interval we were
	 * probably trying to issue an abort, but didnt press the
	 * required number within the interval. Hence cancel all
	 * timeouts and do not continue towards shutdown.
	 */
	if (!timeout_cancel) {
		timeout_cancel = power_button_timeouts +
		    additional_presses;

		power_button_timeouts--;
		if (!power_button_timeouts)
			additional_presses = 0;

		if (timeout_cancel > 1) {
			mutex_exit(&softsp->power_intr_mutex);
			cmn_err(CE_NOTE, "Power Button pressed "
			    "%d times, cancelling all requests",
			    timeout_cancel);
			return;
		}
		mutex_exit(&softsp->power_intr_mutex);

		/* Go and do the work to request shutdown */
		(void) power_issue_shutdown((caddr_t)softsp);
		return;
	}

	power_button_timeouts--;
	if (!power_button_timeouts)
		additional_presses = 0;
	mutex_exit(&softsp->power_intr_mutex);
}

static uint_t
power_issue_shutdown(caddr_t arg)
{
	struct power_soft_state *softsp = (struct power_soft_state *)arg;

	mutex_enter(&softsp->power_mutex);
	softsp->events |= PB_BUTTON_PRESS;
	if (softsp->monitor_on != 0) {
		mutex_exit(&softsp->power_mutex);
		pollwakeup(&softsp->pollhd, POLLRDNORM);
		pollwakeup(&softsp->pollhd, POLLIN);
		return (DDI_INTR_CLAIMED);
	}

	if (!softsp->shutdown_pending) {
		cmn_err(CE_WARN, "Power off requested from power button or "
		    "SC, powering down the system!");
		softsp->shutdown_pending = 1;
		do_shutdown();

		/*
		 * Wait a while for "do_shutdown()" to shut down the system
		 * before logging an error message.
		 */
		(void) timeout((void(*)(void *))power_log_message, NULL,
		    100 * hz);
	}
	mutex_exit(&softsp->power_mutex);

	return (DDI_INTR_CLAIMED);
}

/*
 * Open the device.
 */
/*ARGSUSED*/
static int
power_open(dev_t *devp, int openflags, int otyp, cred_t *credp)
{
	struct power_soft_state *softsp;
	int clone;

	if (otyp != OTYP_CHR)
		return (EINVAL);

	if ((softsp = ddi_get_soft_state(power_state, power_inst)) ==
	    NULL)
		return (ENXIO);

	mutex_enter(&softsp->power_mutex);
	for (clone = 1; clone < POWER_MAX_CLONE; clone++)
		if (!softsp->clones[clone])
			break;

	if (clone == POWER_MAX_CLONE) {
		cmn_err(CE_WARN, "power_open: No more allocation left "
		    "to create a clone minor.");
		mutex_exit(&softsp->power_mutex);
		return (ENXIO);
	}

	*devp = makedevice(getmajor(*devp), (power_inst << 8) + clone);
	softsp->clones[clone] = 1;
	mutex_exit(&softsp->power_mutex);

	return (0);
}

/*
 * Close the device.
 */
/*ARGSUSED*/
static  int
power_close(dev_t dev, int openflags, int otyp, cred_t *credp)
{
	struct power_soft_state *softsp;
	int clone;

	if (otyp != OTYP_CHR)
		return (EINVAL);

	if ((softsp = ddi_get_soft_state(power_state, power_inst)) ==
	    NULL)
		return (ENXIO);

	clone = POWER_MINOR_TO_CLONE(getminor(dev));
	mutex_enter(&softsp->power_mutex);
	if (softsp->monitor_on == clone)
		softsp->monitor_on = 0;
	softsp->clones[clone] = 0;
	mutex_exit(&softsp->power_mutex);

	return (0);
}

/*ARGSUSED*/
static  int
power_ioctl(dev_t dev, int cmd, intptr_t arg, int mode, cred_t *cred_p,
    int *rval_p)
{
	struct power_soft_state *softsp;
	int clone;

	if ((softsp = ddi_get_soft_state(power_state, power_inst)) ==
	    NULL)
		return (ENXIO);

	clone = POWER_MINOR_TO_CLONE(getminor(dev));
	switch (cmd) {
	case PB_BEGIN_MONITOR:
		mutex_enter(&softsp->power_mutex);
		if (softsp->monitor_on) {
			mutex_exit(&softsp->power_mutex);
			return (EBUSY);
		}
		softsp->monitor_on = clone;
		mutex_exit(&softsp->power_mutex);
		return (0);

	case PB_END_MONITOR:
		mutex_enter(&softsp->power_mutex);

		/*
		 * If PB_END_MONITOR is called without first
		 * calling PB_BEGIN_MONITOR, an error will be
		 * returned.
		 */
		if (!softsp->monitor_on) {
			mutex_exit(&softsp->power_mutex);
			return (ENXIO);
		}

		/*
		 * This clone is not monitoring the button.
		 */
		if (softsp->monitor_on != clone) {
			mutex_exit(&softsp->power_mutex);
			return (EINVAL);
		}
		softsp->monitor_on = 0;
		mutex_exit(&softsp->power_mutex);
		return (0);

	case PB_GET_EVENTS:
		mutex_enter(&softsp->power_mutex);
		if (ddi_copyout((void *)&softsp->events, (void *)arg,
		    sizeof (int), mode) != 0) {
			mutex_exit(&softsp->power_mutex);
			return (EFAULT);
		}

		/*
		 * This ioctl returned the events detected since last
		 * call.  Note that any application can get the events
		 * and clear the event register.
		 */
		softsp->events = 0;
		mutex_exit(&softsp->power_mutex);
		return (0);

	/*
	 * This ioctl is used by the test suite.
	 */
	case PB_CREATE_BUTTON_EVENT:
		if (softsp->power_regs_mapped) {
			mutex_enter(&softsp->power_intr_mutex);
			softsp->power_btn_ioctl = B_TRUE;
			mutex_exit(&softsp->power_intr_mutex);
		}
		(void) power_high_intr((caddr_t)softsp);
		return (0);

	default:
		return (ENOTTY);
	}
}

/*ARGSUSED*/
static int
power_chpoll(dev_t dev, short events, int anyyet,
    short *reventsp, struct pollhead **phpp)
{
	struct power_soft_state *softsp;

	if ((softsp = ddi_get_soft_state(power_state, power_inst)) == NULL)
		return (ENXIO);

	mutex_enter(&softsp->power_mutex);
	*reventsp = 0;
	if (softsp->events)
		*reventsp = POLLRDNORM|POLLIN;
	else {
		if (!anyyet)
			*phpp = &softsp->pollhd;
	}
	mutex_exit(&softsp->power_mutex);

	return (0);
}

static void
power_log_message(void)
{
	struct power_soft_state *softsp;

	if ((softsp = ddi_get_soft_state(power_state, power_inst)) == NULL) {
		cmn_err(CE_WARN, "Failed to get internal state!");
		return;
	}

	mutex_enter(&softsp->power_mutex);
	softsp->shutdown_pending = 0;
	cmn_err(CE_WARN, "Failed to shut down the system!");
	mutex_exit(&softsp->power_mutex);
}

/*
 * power button register definitions for acpi register on m1535d
 */
#define	M1535D_PWR_BTN_REG_01		0x1
#define	M1535D_PWR_BTN_EVENT_FLAG	0x1

static int
power_setup_m1535_regs(dev_info_t *dip, struct power_soft_state *softsp)
{
	ddi_device_acc_attr_t	attr;
	uint8_t *reg_base;

	attr.devacc_attr_version = DDI_DEVICE_ATTR_V0;
	attr.devacc_attr_endian_flags = DDI_STRUCTURE_LE_ACC;
	attr.devacc_attr_dataorder = DDI_STRICTORDER_ACC;
	if (ddi_regs_map_setup(dip, 0, (caddr_t *)&reg_base, 0, 0, &attr,
	    &softsp->power_rhandle) != DDI_SUCCESS) {
		return (DDI_FAILURE);
	}
	softsp->power_btn_reg = &reg_base[M1535D_PWR_BTN_REG_01];
	softsp->power_btn_bit = M1535D_PWR_BTN_EVENT_FLAG;
	softsp->power_regs_mapped = B_TRUE;
	return (DDI_SUCCESS);
}

/*
 * Setup register map for the power button
 * NOTE:- we only map registers for platforms
 * binding with the ali1535d+-power compatible
 * property.
 */
static int
power_setup_regs(struct power_soft_state *softsp)
{
	char	*binding_name;

	softsp->power_regs_mapped = B_FALSE;
	softsp->power_btn_ioctl = B_FALSE;
	binding_name = ddi_binding_name(softsp->dip);
	if (strcmp(binding_name, "ali1535d+-power") == 0)
		return (power_setup_m1535_regs(softsp->dip, softsp));

	return (DDI_SUCCESS);
}

static void
power_free_regs(struct power_soft_state *softsp)
{
	if (softsp->power_regs_mapped)
		ddi_regs_map_free(&softsp->power_rhandle);
}
