#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/usb.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/atomic.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/pcm.h>

#include <linux/mutex.h>
#include <linux/slab.h>

#include "control.h"
#include "usb.h"
#include "pcm.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Print3M");
MODULE_DESCRIPTION("Katana USB AudioControl driver");

// Export operation tracking functions for use by PCM module
int katana_enter_operation(void);
void katana_exit_operation(void);
EXPORT_SYMBOL(katana_enter_operation);
EXPORT_SYMBOL(katana_exit_operation);

/*
 * Per-device state.
 *
 * The driver binds two interfaces of the same device (AudioControl and
 * AudioStream) and builds a single ALSA card from both. probe(), disconnect(),
 * suspend() and resume() are therefore each called twice per device, and the
 * card must outlive the first disconnect. This struct is attached to both
 * interfaces with usb_set_intfdata() and torn down only when the last one goes
 * away; katana_lock serialises the two probes against each other.
 */
struct katana_dev {
	struct usb_device *udev;
	struct snd_card *card;
	struct snd_pcm *pcm;
	int control_ready;
	int stream_ready;
	int registered;
	int bound_ifaces;
	bool suspended;
};

static DEFINE_MUTEX(katana_lock);

static const int katana_ifaces[] = {
	AUDIO_CONTROL_IFACE_ID,
	AUDIO_STREAM_IFACE_ID,
};

// Disconnect synchronization
static atomic_t disconnect_in_progress = ATOMIC_INIT(0);
static atomic_t active_operations = ATOMIC_INIT(0);
static DECLARE_COMPLETION(disconnect_completion);

// Define supported devices (Katana only)
static struct usb_device_id usb_table[] = {
	{ USB_DEVICE(KATANA_VENDOR_ID, KATANA_PRODUCT_ID) },
	{} // Terminator
};

// These devices will be probed with this kernel module.
MODULE_DEVICE_TABLE(usb, usb_table);

// Implementation of exported functions for disconnect synchronization
int katana_enter_operation(void)
{
	if (atomic_read(&disconnect_in_progress)) {
		pr_debug("Katana USB: Operation blocked, disconnect in progress\n");
		return -ENODEV;
	}
	atomic_inc(&active_operations);
	
	// Double-check after incrementing (race condition protection)
	if (atomic_read(&disconnect_in_progress)) {
		atomic_dec(&active_operations);
		pr_debug("Katana USB: Operation blocked, disconnect in progress\n");
		return -ENODEV;
	}
	
	return 0;
}

void katana_exit_operation(void)
{
	if (atomic_dec_and_test(&active_operations)) {
		// If we were the last operation and disconnect is waiting, signal completion
		if (atomic_read(&disconnect_in_progress)) {
			complete(&disconnect_completion);
		}
	}
}

/*
 * Look for state already created by the sibling interface of the same device.
 * Only interfaces bound to this driver are considered - another driver may own
 * an interface we do not claim, and its intfdata is not ours to read.
 * Caller must hold katana_lock.
 */
static struct katana_dev *katana_find_dev(struct usb_interface *iface,
					  struct usb_device *dev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(katana_ifaces); i++) {
		struct usb_interface *other = usb_ifnum_to_if(dev, katana_ifaces[i]);

		if (!other || other == iface)
			continue;
		if (other->dev.driver != iface->dev.driver)
			continue;

		if (usb_get_intfdata(other))
			return usb_get_intfdata(other);
	}

	return NULL;
}

// Tear down state created by katana_dev_create(). Caller must hold katana_lock.
static void katana_dev_destroy(struct katana_dev *kdev)
{
	if (kdev->card)
		snd_card_free(kdev->card);
	kfree(kdev);
}

static int katana_usb_probe(struct usb_interface *iface, const struct usb_device_id *id)
{
	/*
		Determine whether the provided interface is that one we want to work with.
		This function is called for each interface of the matching device.
		We claim the AudioControl and AudioStream interfaces and build one ALSA
		card from the pair.
	*/
	// Map the device's interface to the device itself and get its data
	struct usb_device *dev = interface_to_usbdev(iface);
	struct katana_dev *kdev;
	bool created = false;
	int err;

	// Exit if this is not the desired interface
	int ifnum = iface->cur_altsetting->desc.bInterfaceNumber;
	dev_info(&iface->dev, "Processing interface %d (looking for %d and %d)\n",
		 ifnum, AUDIO_CONTROL_IFACE_ID, AUDIO_STREAM_IFACE_ID);

	if (ifnum != AUDIO_CONTROL_IFACE_ID && ifnum != AUDIO_STREAM_IFACE_ID) {
		dev_info(&iface->dev, "Wrong interface: %d\n", ifnum);
		return -ENODEV;
	}

	dev_info(&iface->dev, "Attached to USB device %04X:%04X\n", dev->descriptor.idVendor,
		 dev->descriptor.idProduct);

	mutex_lock(&katana_lock);

	// Reuse the state built by the sibling interface, or create it
	kdev = katana_find_dev(iface, dev);
	if (!kdev) {
		kdev = kzalloc(sizeof(*kdev), GFP_KERNEL);
		if (!kdev) {
			err = -ENOMEM;
			goto __error;
		}
		kdev->udev = dev;
		created = true;

		/*
		 * Index -1 asks ALSA to pick the first free slot itself. Do not scan
		 * with snd_card_ref(): it takes a reference on every card it returns,
		 * and dropping those on the floor makes snd_card_free() of an
		 * unrelated card wait forever on its refcount.
		 */
		err = snd_card_new(&dev->dev, -1, "katana-usb-audio", THIS_MODULE, 0,
				   &kdev->card);
		if (err != 0) {
			dev_err(&iface->dev, "ALSA card creation failed: %d\n", err);
			kfree(kdev);
			goto __error;
		}

		// Basic info about the new sound card
		strcpy(kdev->card->driver, "katana_ac");
		strcpy(kdev->card->shortname, "SoundBlaster X Katana");
		strcpy(kdev->card->longname, "Creative SoundBlaster X Katana USB Audio Device");

		// Store the USB device in the card's private data for PCM operations
		kdev->card->private_data = dev;

		dev_info(&iface->dev, "New ALSA card created: %s\n", kdev->card->longname);
	}

	// Setup Audio Control component
	if (ifnum == AUDIO_CONTROL_IFACE_ID && !kdev->control_ready) {
		// Init volume control
		struct snd_kcontrol *kctl_vol = snd_ctl_new1(&katana_vol_ctl, kdev->card);
		if (kctl_vol == NULL) {
			dev_err(&iface->dev, "Volume control creation failed\n");
			err = -ENOMEM;
			goto __error_dev;
		}

		// Attach volume control
		err = snd_ctl_add(kdev->card, kctl_vol);
		if (err != 0) {
			dev_err(&iface->dev, "Adding volume control failed: %d\n", err);
			snd_ctl_free_one(kctl_vol);
			goto __error_dev;
		}

		// Init mute control
		struct snd_kcontrol *kctl_mute = snd_ctl_new1(&katana_mute_ctl, kdev->card);
		if (kctl_mute == NULL) {
			dev_err(&iface->dev, "Mute control creation failed\n");
			err = -ENOMEM;
			goto __error_dev;
		}

		// Attach mute control
		err = snd_ctl_add(kdev->card, kctl_mute);
		if (err != 0) {
			dev_err(&iface->dev, "Adding mute control failed: %d\n", err);
			snd_ctl_free_one(kctl_mute);
			goto __error_dev;
		}

		kdev->control_ready = 1;
		dev_info(&iface->dev, "Audio controls added successfully\n");
	}

	// Setup Audio Stream component
	if (ifnum == AUDIO_STREAM_IFACE_ID && !kdev->stream_ready) {
		// Create PCM device. Keep the pointer: suspend needs it to stop
		// any running substream before the host controller goes down.
		err = katana_pcm_new(kdev->card, &kdev->pcm);
		if (err != 0) {
			dev_err(&iface->dev, "PCM device creation failed: %d\n", err);
			goto __error_dev;
		}

		kdev->stream_ready = 1;
		dev_info(&iface->dev, "PCM device created successfully\n");
	}

	// Claim the interface before announcing success
	usb_set_intfdata(iface, kdev);
	kdev->bound_ifaces++;

	// Register the card only after both interfaces are ready
	if (kdev->control_ready && kdev->stream_ready && !kdev->registered) {
		err = snd_card_register(kdev->card);
		if (err != 0) {
			dev_err(&iface->dev, "ALSA card registration failed: %d\n", err);
			usb_set_intfdata(iface, NULL);
			kdev->bound_ifaces--;
			goto __error_dev;
		}
		kdev->registered = 1;
		dev_info(&iface->dev, "ALSA card registered successfully with all components\n");
	} else if (!kdev->registered) {
		dev_info(&iface->dev, "Interface %d processed, waiting for other interface...\n", ifnum);
	}

	mutex_unlock(&katana_lock);

	dev_info(&iface->dev, "Everything works. Ifnum: %d\n", ifnum);

	return 0; // SUCCESS - there is a match

__error_dev:
	// Only tear the card down if the sibling interface is not using it
	if (created && kdev->bound_ifaces == 0)
		katana_dev_destroy(kdev);
__error:
	mutex_unlock(&katana_lock);
	// Standard error if the driver doesn't want to work with this interface
	return err ? err : -ENODEV;
}

static void katana_usb_disconnect(struct usb_interface *iface)
{
	/*
		This function is called when the driver is not able already to control the device.
		Its main purpose is to clean everything up after the driver usage.
		Called once per bound interface - only the last one tears the card down.
	*/
	struct usb_device *dev = interface_to_usbdev(iface);
	struct katana_dev *kdev = usb_get_intfdata(iface);

	if (!kdev)
		return;

	usb_set_intfdata(iface, NULL);

	mutex_lock(&katana_lock);

	if (--kdev->bound_ifaces > 0) {
		mutex_unlock(&katana_lock);
		dev_info(&dev->dev, "Interface released, %d still bound\n",
			 kdev->bound_ifaces);
		return;
	}

	// Step 1: Set disconnect flag to block new operations
	atomic_set(&disconnect_in_progress, 1);

	// Step 2: Invalidate USB device in all PCM substreams and stop their URBs
	// This prevents use-after-free bugs when the card is freed
	katana_pcm_invalidate_usb_dev(kdev->card);

	// Step 3: Wait for all active operations to complete
	// Check if there are any active operations
	if (atomic_read(&active_operations) > 0) {
		// Re-initialize completion for this disconnect
		reinit_completion(&disconnect_completion);

		// Wait for completion with timeout (10 seconds max)
		unsigned long timeout = wait_for_completion_timeout(&disconnect_completion,
							   msecs_to_jiffies(10000));
		if (timeout == 0) {
			pr_warn("Katana USB: Timeout waiting for operations to complete, forcing disconnect\n");
		}
	}

	// Step 4: Now it's safe to free the card
	katana_dev_destroy(kdev);

	// Step 5: Reset disconnect state for potential future reconnection
	atomic_set(&disconnect_in_progress, 0);
	atomic_set(&active_operations, 0);

	mutex_unlock(&katana_lock);

	dev_info(&dev->dev, "The driver has been disconnected\n");
}

/*
 * Power management.
 *
 * All three callbacks run once per bound interface, so each guards on
 * kdev->suspended and does the real work only on the first (resp. last) call.
 * Providing them is what stops the USB core from performing a logical
 * disconnect and re-probe when the device loses VBUS across S3.
 */
static int katana_usb_suspend(struct usb_interface *iface, pm_message_t message)
{
	struct katana_dev *kdev = usb_get_intfdata(iface);

	if (!kdev)
		return 0;

	mutex_lock(&katana_lock);
	if (!kdev->suspended) {
		kdev->suspended = true;

		// Stop the stream and wait for every URB to retire before the host
		// controller powers down
		katana_pcm_suspend(kdev->pcm);

		if (kdev->card)
			snd_power_change_state(kdev->card, SNDRV_CTL_POWER_D3hot);

		dev_info(&iface->dev, "Suspended\n");
	}
	mutex_unlock(&katana_lock);

	return 0;
}

static int katana_usb_resume(struct usb_interface *iface)
{
	struct katana_dev *kdev = usb_get_intfdata(iface);

	if (!kdev)
		return 0;

	mutex_lock(&katana_lock);
	if (kdev->suspended) {
		kdev->suspended = false;

		katana_pcm_resume(kdev->pcm);

		if (kdev->card)
			snd_power_change_state(kdev->card, SNDRV_CTL_POWER_D0);

		dev_info(&iface->dev, "Resumed\n");
	}
	mutex_unlock(&katana_lock);

	return 0;
}

/*
 * Called instead of resume when the device lost power and was reset. There is
 * no probe-time device configuration to replay: the altsetting and sample rate
 * are applied by katana_pcm_prepare(), which runs again when userspace
 * re-prepares the stream after the -ESTRPIPE that suspend caused, and the
 * volume and mute controls read their values back from the hardware on every
 * get. So the work is identical to a normal resume.
 */
static int katana_usb_reset_resume(struct usb_interface *iface)
{
	dev_info(&iface->dev, "Device was reset, resuming\n");
	return katana_usb_resume(iface);
}

// Main USB driver structure
static struct usb_driver usb_ac_driver = {
	.name	      = "katana_usb_audio",    // Should be unique and the same as the module name
	.probe	      = katana_usb_probe,      // See if the driver is willing to work with the iface
	.disconnect   = katana_usb_disconnect, // Called when the interface is no longer accessible
	.suspend      = katana_usb_suspend,    // Stop streaming before the bus goes down
	.resume	      = katana_usb_resume,     // Bus came back with the device still configured
	.reset_resume = katana_usb_reset_resume, // Device lost power and was reset
	.id_table     = usb_table,	       // Required or the driver's probe will never get called
	// .supports_autosuspend is intentionally left unset - runtime PM is not implemented
};

/*
	This is a shortcut to avoid boilerplate __init and __exit functions to
	only register (usb_register) and deregister (usb_deregister) the USB driver.
	This helper handles it automatically.
*/
module_usb_driver(usb_ac_driver);