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
	bool resetting;		// between pre_reset and the first post_reset
	bool reset_pending;	// a reset is queued but pre_reset has not run yet
};

static DEFINE_MUTEX(katana_lock);

/*
 * Recovery from a half-initialised device (see katana_ctl_health_check()).
 *
 * The attempt counter is module-wide rather than per device because a reset
 * that fails outright makes the USB core re-enumerate the Katana as a new
 * device, which would start a per-device counter from zero again and could
 * loop forever. The window lets the next suspend cycle try afresh. Both are
 * protected by katana_lock.
 */
#define KATANA_MAX_RESETS	3
#define KATANA_RESET_WINDOW	(60 * HZ)

static int katana_reset_attempts;
static unsigned long katana_reset_window_start;

// Test hook: make the next N health checks fail, to exercise the recovery
// path without waiting for the device to come back broken from suspend.
static int simulate_unhealthy;
module_param(simulate_unhealthy, int, 0644);
MODULE_PARM_DESC(simulate_unhealthy,
		 "Testing only: report the next N device health checks as failed");

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

// Tear down state created by katana_usb_probe(). Caller must hold katana_lock.
static void katana_dev_destroy(struct katana_dev *kdev)
{
	if (kdev->card)
		snd_card_free(kdev->card);
	kfree(kdev);
}

static int katana_health_check(struct katana_dev *kdev)
{
	if (simulate_unhealthy > 0) {
		simulate_unhealthy--;
		return -EPROTO;
	}
	return katana_ctl_health_check(kdev->udev);
}

/*
 * Register the card once both interfaces are bound - but only if the device is
 * actually answering. If it came back half-initialised, registering it anyway
 * makes PipeWire probe it immediately, fail, and remember it as "Pro Audio"
 * only. Instead, hold the card back and queue a USB reset, which brings the
 * device up properly; post_reset() then calls back in here to try again.
 *
 * Returns 0 when the card was registered or deliberately deferred, or the
 * error from snd_card_register(). Caller must hold katana_lock.
 */
static int katana_try_register(struct katana_dev *kdev, struct usb_interface *iface)
{
	int err;

	if (kdev->registered || !kdev->control_ready || !kdev->stream_ready)
		return 0;

	// A reset is already on its way; post_reset() will be back
	if (kdev->reset_pending)
		return 0;

	err = katana_health_check(kdev);
	if (err == 0) {
		katana_reset_attempts = 0;
	} else {
		if (time_after(jiffies, katana_reset_window_start + KATANA_RESET_WINDOW)) {
			katana_reset_window_start = jiffies;
			katana_reset_attempts = 0;
		}

		if (katana_reset_attempts < KATANA_MAX_RESETS) {
			katana_reset_attempts++;
			dev_warn(&iface->dev,
				 "Device not answering control requests (%d), resetting it (attempt %d/%d)\n",
				 err, katana_reset_attempts, KATANA_MAX_RESETS);
			kdev->reset_pending = true;
			// Asynchronous: usb_reset_device() cannot be called from probe or
			// from inside another reset's post_reset callback
			usb_queue_reset_device(iface);
			return 0;
		}

		// Out of attempts. Register anyway so the card is at least visible
		// and the problem is reported, rather than silently missing.
		dev_err(&iface->dev,
			"Device still not answering after %d resets (%d). Power-cycle the soundbar to recover.\n",
			KATANA_MAX_RESETS, err);
	}

	err = snd_card_register(kdev->card);
	if (err != 0) {
		dev_err(&iface->dev, "ALSA card registration failed: %d\n", err);
		return err;
	}
	kdev->registered = 1;
	dev_info(&iface->dev, "ALSA card registered successfully with all components\n");
	return 0;
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

	// Register the card only after both interfaces are ready, and only once the
	// device is answering. On success the interface must stay bound even if
	// registration was deferred, because the queued reset is tied to it.
	if (kdev->control_ready && kdev->stream_ready) {
		err = katana_try_register(kdev, iface);
		if (err != 0) {
			usb_set_intfdata(iface, NULL);
			kdev->bound_ifaces--;
			goto __error_dev;
		}
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
 * Stop all traffic to the device: suspend any running substream (userspace
 * gets -ESTRPIPE and re-prepares later), wait for every URB to retire, and move
 * the card to D3hot, which makes ALSA hold PCM and mixer ioctls until D0 so
 * nothing new is submitted meanwhile. Shared by system suspend and USB reset.
 * Caller must hold katana_lock.
 */
static void katana_quiesce(struct katana_dev *kdev)
{
	katana_pcm_suspend(kdev->pcm);

	if (kdev->card)
		snd_power_change_state(kdev->card, SNDRV_CTL_POWER_D3hot);
}

// Undo katana_quiesce(). Caller must hold katana_lock.
static void katana_unquiesce(struct katana_dev *kdev)
{
	katana_pcm_resume(kdev->pcm);

	if (kdev->card)
		snd_power_change_state(kdev->card, SNDRV_CTL_POWER_D0);
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
		katana_quiesce(kdev);

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

		katana_unquiesce(kdev);

		dev_info(&iface->dev, "Resumed\n");
	}
	mutex_unlock(&katana_lock);

	return 0;
}

/*
 * USB reset handling.
 *
 * Without these two callbacks the USB core unbinds the driver before a reset
 * and re-matches the interfaces afterwards. That re-match is not an "add"
 * event, so the udev rule never fires and snd-usb-audio takes the device -
 * which is exactly what a manual `usbreset` used to do, and what would undo
 * the automatic recovery in katana_try_register().
 *
 * Like the PM callbacks, both run once per bound interface; kdev->resetting
 * makes the work happen once per reset.
 */
static int katana_usb_pre_reset(struct usb_interface *iface)
{
	struct katana_dev *kdev = usb_get_intfdata(iface);

	if (!kdev)
		return 0;

	mutex_lock(&katana_lock);
	kdev->reset_pending = false;
	if (!kdev->resetting) {
		kdev->resetting = true;

		// No URBs may be in flight or submitted until post_reset
		katana_quiesce(kdev);

		dev_info(&iface->dev, "Device reset starting\n");
	}
	mutex_unlock(&katana_lock);

	return 0;
}

static int katana_usb_post_reset(struct usb_interface *iface)
{
	struct katana_dev *kdev = usb_get_intfdata(iface);

	if (!kdev)
		return 0;

	mutex_lock(&katana_lock);
	if (kdev->resetting) {
		kdev->resetting = false;

		katana_unquiesce(kdev);

		dev_info(&iface->dev, "Device reset complete\n");
	}

	// If registration was held back because the device was not answering,
	// check it again now. A no-op once the card is registered. The error is
	// already logged, and post_reset has no way to fail the reset over it.
	katana_try_register(kdev, iface);
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
	.pre_reset    = katana_usb_pre_reset,  // Stay bound across a USB reset...
	.post_reset   = katana_usb_post_reset, // ...instead of being unbound and re-matched
	.id_table     = usb_table,	       // Required or the driver's probe will never get called
	// .supports_autosuspend is intentionally left unset - runtime PM is not implemented
};

/*
	This is a shortcut to avoid boilerplate __init and __exit functions to
	only register (usb_register) and deregister (usb_deregister) the USB driver.
	This helper handles it automatically.
*/
module_usb_driver(usb_ac_driver);