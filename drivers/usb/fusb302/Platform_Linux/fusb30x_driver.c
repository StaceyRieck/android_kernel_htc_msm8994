/* 
 * File:   fusb30x_driver.c
 * Author: Tim Bremm <tim.bremm@fairchildsemi.com>
 * Company: Fairchild Semiconductor
 *
 * Created on September 2, 2015, 10:22 AM
 */

/* Standard Linux includes */
#include <linux/init.h>                                                         // __init, __initdata, etc
#include <linux/module.h>                                                       // Needed to be a module
#include <linux/kernel.h>                                                       // Needed to be a kernel module
#include <linux/i2c.h>                                                          // I2C functionality
#include <linux/slab.h>                                                         // devm_kzalloc
#include <linux/types.h>                                                        // Kernel datatypes
#include <linux/errno.h>                                                        // EINVAL, ERANGE, etc
#include <linux/of_device.h>                                                    // Device tree functionality
#include <linux/regulator/consumer.h>						// get regulator header file
#include <linux/htc_flags.h>

/* Driver-specific includes */
#include "fusb30x_global.h"                                                     // Driver-specific structures/types
#include "platform_helpers.h"                                                   // I2C R/W, GPIO, misc, etc

#ifdef FSC_DEBUG
#include "../core/core.h"                                                       // GetDeviceTypeCStatus
#endif // FSC_DEBUG

#include "fusb30x_driver.h"
/* HTC SSD USB + */
#define VDD_3P3_VOL_MIN	3300000		/* uV */
#define VDD_3P3_VOL_MAX	3300000		/* uV */

static struct regulator *vdd;
/* HTC SSD USB - */

/******************************************************************************
* Driver functions
******************************************************************************/
static int fusb30x_pm_suspend(struct device *dev)
{
	struct fusb30x_chip *chip = fusb30x_GetChip();
	if (!chip) {
		if (get_debug_flag() & 0x200) {
			return 0;
		} else {
			printk("FUSB  [%s] - Error: Chip structure is NULL!\n", __func__);
			return 0;
		}
	}
	dev_dbg(dev, "FUSB PM suspend\n");
	atomic_set(&chip->pm_suspended, 1);
	return 0;
}

static int fusb30x_pm_resume(struct device *dev)
{
	struct fusb30x_chip *chip = fusb30x_GetChip();
	if (!chip) {
		if (get_debug_flag() & 0x200) {
			return 0;
		} else {
			printk("FUSB  [%s] - Error: Chip structure is NULL!\n", __func__);
			return 0;
		}
	}
	dev_dbg(dev, "FUSB PM resume\n");
	atomic_set(&chip->pm_suspended, 0);
	return 0;
}

static int __init fusb30x_init(void)
{
    pr_debug("FUSB  %s - Start driver initialization...\n", __func__);

	return i2c_add_driver(&fusb30x_driver);
}

static void __exit fusb30x_exit(void)
{
	i2c_del_driver(&fusb30x_driver);
    pr_debug("FUSB  %s - Driver deleted...\n", __func__);
}

static int fusb30x_probe (struct i2c_client* client,
                          const struct i2c_device_id* id)
{
    int ret = 0;
    struct fusb30x_chip* chip;
    struct i2c_adapter* adapter;

/*++ 2016/07/13, USB Team, PCN00001 ++*/
    if (get_debug_flag() & 0x200) {
	    printk("FUSB  [%s] -(Workaround for ATS) Do not probe FUSB driver since it is in Debug mode with 5 200\n", __func__);
	    return 0;
    }
/*-- 2016/07/13, USB Team, PCN00001 --*/

    if (!client)
    {
        pr_err("FUSB  %s - Error: Client structure is NULL!\n", __func__);
        return -EINVAL;
    }
    dev_info(&client->dev, "%s\n", __func__);

    /* Make sure probe was called on a compatible device */
	if (!of_match_device(fusb30x_dt_match, &client->dev))
	{
		dev_err(&client->dev, "FUSB  %s - Error: Device tree mismatch!\n", __func__);
		return -EINVAL;
	}
    pr_debug("FUSB  %s - Device tree matched!\n", __func__);

    /* Allocate space for our chip structure (devm_* is managed by the device) */
    chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
    if (!chip)
	{
		dev_err(&client->dev, "FUSB  %s - Error: Unable to allocate memory for fusb_g_chip!\n", __func__);
		return -ENOMEM;
	}
    chip->client = client;                                                      // Assign our client handle to our chip
    fusb30x_SetChip(chip);                                                      // Set our global chip's address to the newly allocated memory
    pr_debug("FUSB  %s - Chip structure is set! Chip: %p ... fusb_g_chip: %p\n", __func__, chip, fusb30x_GetChip());

    /* Initialize the chip lock */
    mutex_init(&chip->lock);

    /* Initialize the chip's data members */
    fusb_InitChipData();
    pr_debug("FUSB  %s - Chip struct data initialized!\n", __func__);

    /* HTC SSD USB + */
    vdd = devm_regulator_get(&client->dev, "vdd");
    if (IS_ERR(vdd)) {
	    dev_err(&client->dev, "unable to get typec vdd 3p3\n");
    }

    ret = regulator_set_voltage(vdd, VDD_3P3_VOL_MIN, VDD_3P3_VOL_MAX);
    if (ret < 0) {
	    pr_err("%s: Unable to set optimum mode of the regulator typec vdd 3p3\n", __func__);
    }

    ret = regulator_enable(vdd);
    if (ret) {
	    dev_err(&client->dev, "%s: Unable to enable the typec vdd 3p3\n", __func__);
	    regulator_set_optimum_mode(vdd, 0);
    }
    printk(KERN_WARNING "FUSB %s - power on FUSB302\n", __func__);

    chip->boost_5v = NULL;
    /* HTC SSD USB - */

    /* Verify that the system has our required I2C/SMBUS functionality (see <linux/i2c.h> for definitions) */
    adapter = to_i2c_adapter(client->dev.parent);
    if (i2c_check_functionality(adapter, FUSB30X_I2C_SMBUS_BLOCK_REQUIRED_FUNC))
    {
        chip->use_i2c_blocks = true;
    }
    else
    {
        // If the platform doesn't support block reads, try with block writes and single reads (works with eg. RPi)
        // NOTE: It is likely that this may result in non-standard behavior, but will often be 'close enough' to work for most things
        dev_warn(&client->dev, "FUSB  %s - Warning: I2C/SMBus block read/write functionality not supported, checking single-read mode...\n", __func__);
        if (!i2c_check_functionality(adapter, FUSB30X_I2C_SMBUS_REQUIRED_FUNC))
        {
            dev_err(&client->dev, "FUSB  %s - Error: Required I2C/SMBus functionality not supported!\n", __func__);
            dev_err(&client->dev, "FUSB  %s - I2C Supported Functionality Mask: 0x%x\n", __func__, i2c_get_functionality(adapter));
            return -EIO;
        }
    }
    pr_debug("FUSB  %s - I2C Functionality check passed! Block reads: %s\n", __func__, chip->use_i2c_blocks ? "YES" : "NO");

    /* Assign our struct as the client's driverdata */
    i2c_set_clientdata(client, chip);
    pr_debug("FUSB  %s - I2C client data set!\n", __func__);

    /* Verify that our device exists and that it's what we expect */
    if (!fusb_IsDeviceValid())
    {
        dev_err(&client->dev, "FUSB  %s - Error: Unable to communicate with device!\n", __func__);
        return -EIO;
    }
    pr_debug("FUSB  %s - Device check passed!\n", __func__);

    /* Initialize the platform's GPIO pins and IRQ */
    ret = fusb_InitializeGPIO();
    if (ret)
    {
        dev_err(&client->dev, "FUSB  %s - Error: Unable to initialize GPIO!\n", __func__);
        return ret;
    }
    pr_debug("FUSB  %s - GPIO initialized!\n", __func__);

    /* Initialize our timer */
    fusb_InitializeTimer();
    pr_debug("FUSB  %s - Timers initialized!\n", __func__);

#ifdef FSC_DEBUG
    /* Initialize debug sysfs file accessors */
    fusb_Sysfs_Init();
    pr_debug("FUSB  %s - Sysfs device file created!\n", __func__);
#endif // FSC_DEBUG

#ifdef FSC_INTERRUPT_TRIGGERED
    /* Enable interrupts after successful core/GPIO initialization */
    ret = fusb_EnableInterrupts();
    if (ret)
    {
        dev_err(&client->dev, "FUSB  %s - Error: Unable to enable interrupts! Error code: %d\n", __func__, ret);
        return -EIO;
    }

    /* Initialize the core and enable the state machine (NOTE: timer and GPIO must be initialized by now)
    *  Interrupt must be enabled before starting 302 initialization */
    fusb_InitializeCore();
    pr_debug("FUSB  %s - Core is initialized!\n", __func__);
#else
    /* Initialize the core and enable the state machine (NOTE: timer and GPIO must be initialized by now) */
    fusb_InitializeCore();
    pr_debug("FUSB  %s - Core is initialized!\n", __func__);

    /* Init our workers, but don't start them yet */
    fusb_InitializeWorkers();
    /* Start worker threads after successful initialization */
    fusb_ScheduleWork();
    pr_debug("FUSB  %s - Workers initialized and scheduled!\n", __func__);
#endif  // ifdef FSC_POLLING elif FSC_INTERRUPT_TRIGGERED

    dev_info(&client->dev, "FUSB  %s - FUSB30X Driver loaded successfully!\n", __func__);
	return ret;
}

static int fusb30x_remove(struct i2c_client* client)
{
    pr_debug("FUSB  %s - Removing fusb30x device!\n", __func__);

#ifndef FSC_INTERRUPT_TRIGGERED // Polling mode by default
    fusb_StopThreads();
#endif  // !FSC_INTERRUPT_TRIGGERED

    fusb_GPIO_Cleanup();
    pr_debug("FUSB  %s - FUSB30x device removed from driver...\n", __func__);
    return 0;
}

/*******************************************************************************
 * Driver macros
 ******************************************************************************/
module_init(fusb30x_init);                                                      // Defines the module's entrance function
module_exit(fusb30x_exit);                                                      // Defines the module's exit function

MODULE_LICENSE("GPL");                                                          // Exposed on call to modinfo
MODULE_DESCRIPTION("Fairchild FUSB30x Driver");                                 // Exposed on call to modinfo
MODULE_AUTHOR("Tim Bremm<tim.bremm@fairchildsemi.com>");                        // Exposed on call to modinfo
