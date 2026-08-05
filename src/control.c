#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/usb.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include "control.h"

// USB Audio Class 1.0 request codes
#define UAC_SET_CUR	0x01
#define UAC_GET_CUR	0x81
#define UAC_GET_MIN	0x82
#define UAC_GET_MAX	0x83
#define UAC_GET_RES	0x84

// bmRequestType for Feature Unit requests (class request, interface recipient)
#define UAC_RT_GET	(USB_DIR_IN  | USB_TYPE_CLASS | USB_RECIP_INTERFACE)  // 0xA1
#define UAC_RT_SET	(USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE)  // 0x21

// wValue: control selector in the high byte, channel number in the low byte
#define UAC_MUTE_CH0	0x0100	// Mute Control, channel 0 (master)
#define UAC_VOLUME_CH1	0x0201	// Volume Control, channel 1 (left)
#define UAC_VOLUME_CH2	0x0202	// Volume Control, channel 2 (right)

// wIndex: Feature Unit ID in the high byte, interface number in the low byte
#define UAC_FU1_IFACE0	0x0100	// Interface 0, Feature Unit 1 (speaker output)

#define KATANA_CTL_TIMEOUT	1000

/*
 * usb_control_msg() maps the caller's data buffer for DMA, so that buffer must
 * live in the kernel's direct map. Neither a stack variable (CONFIG_VMAP_STACK)
 * nor usb_alloc_coherent() memory (vmap-remapped when an IOMMU is active)
 * qualifies, and the HCD rejects both with -EAGAIN before the request ever
 * reaches the device. usb_control_msg_recv()/_send() bounce through a kmalloc'd
 * buffer internally, so an ordinary local variable is safe to pass here.
 *
 * Note both helpers return 0 on success, not a byte count (a short transfer is
 * reported as -EREMOTEIO).
 */
static int katana_ctl_get(struct usb_device *usb_dev, __u8 request,
			  __u16 value, void *buf, __u16 len)
{
	return usb_control_msg_recv(usb_dev, 0, request, UAC_RT_GET,
				    value, UAC_FU1_IFACE0, buf, len,
				    KATANA_CTL_TIMEOUT, GFP_KERNEL);
}

static int katana_ctl_set(struct usb_device *usb_dev, __u8 request,
			  __u16 value, const void *buf, __u16 len)
{
	return usb_control_msg_send(usb_dev, 0, request, UAC_RT_SET,
				    value, UAC_FU1_IFACE0, buf, len,
				    KATANA_CTL_TIMEOUT, GFP_KERNEL);
}

// Global volume range variables (set once during initialization)
static int16_t katana_vol_min = -20480;  // Default fallback
static int16_t katana_vol_max = 0;       // Default fallback
static int16_t katana_vol_res = 1;       // Default fallback
static int katana_vol_range_initialized = 0;

// Removed auto-unmute logic - let ALSA handle mute/unmute properly

// Forward declarations
static int katana_set_hardware_mute(struct usb_device *usb_dev, int mute);

// Get volume range from device using USB Audio Class standard requests
static int katana_get_volume_range(struct usb_device *usb_dev, int16_t *min_vol, int16_t *max_vol, int16_t *res_vol)
{
	int err;
	unsigned char volume_data[2];

	// Get MIN value
	err = katana_ctl_get(usb_dev, UAC_GET_MIN, UAC_VOLUME_CH1, volume_data, 2);
	if (err == 0) {
		*min_vol = volume_data[0] | (volume_data[1] << 8);
	} else {
		pr_warn("Katana Control: Failed to get volume MIN: %d\n", err);
		*min_vol = -20480; // fallback
	}

	// Get MAX value
	err = katana_ctl_get(usb_dev, UAC_GET_MAX, UAC_VOLUME_CH1, volume_data, 2);
	if (err == 0) {
		*max_vol = volume_data[0] | (volume_data[1] << 8);
	} else {
		pr_warn("Katana Control: Failed to get volume MAX: %d\n", err);
		*max_vol = 0; // fallback
	}

	// Get RES value
	err = katana_ctl_get(usb_dev, UAC_GET_RES, UAC_VOLUME_CH1, volume_data, 2);
	if (err == 0 && (volume_data[0] | (volume_data[1] << 8)) != 0) {
		*res_vol = volume_data[0] | (volume_data[1] << 8);
	} else {
		if (err)
			pr_warn("Katana Control: Failed to get volume RES: %d\n", err);
		*res_vol = 1; // fallback
	}

	// Update global variables
	katana_vol_min = *min_vol;
	katana_vol_max = *max_vol;
	katana_vol_res = *res_vol;
	katana_vol_range_initialized = 1;
	
	pr_info("Katana Control: Volume range initialized - MIN: %d, MAX: %d, RES: %d\n",
		*min_vol, *max_vol, *res_vol);

	return 0;
}

// Set raw hardware volume value
static int katana_set_hardware_volume_raw(struct usb_device *usb_dev, int16_t volume_value)
{
	int err;
	unsigned char volume_data[2];

	// Initialize volume range if not done already
	if (!katana_vol_range_initialized) {
		int16_t min_vol, max_vol, res_vol;
		katana_get_volume_range(usb_dev, &min_vol, &max_vol, &res_vol);
	}

	// Pack volume value into 2-byte little-endian format
	volume_data[0] = volume_value & 0xff;
	volume_data[1] = (volume_value >> 8) & 0xff;

	// Send SET_CUR request for volume control on channel 1 (left)
	err = katana_ctl_set(usb_dev, UAC_SET_CUR, UAC_VOLUME_CH1, volume_data, 2);
	if (err < 0) {
		pr_err("Katana Control: Failed to set hardware volume %d: %d\n", volume_value, err);
		return err;
	}

	// Also set right channel (channel 2)
	err = katana_ctl_set(usb_dev, UAC_SET_CUR, UAC_VOLUME_CH2, volume_data, 2);
	if (err < 0) {
		pr_err("Katana Control: Failed to set hardware volume right channel %d: %d\n", volume_value, err);
		return err;
	}

	return 0;
}

// Removed unused percentage-based volume control function

// Get raw hardware volume value (not percentage)
static int16_t katana_get_hardware_volume_raw(struct usb_device *usb_dev)
{
	int err;
	unsigned char volume_data[2];
	int16_t volume_value;

	// Initialize volume range if not done already
	if (!katana_vol_range_initialized) {
		int16_t min_vol, max_vol, res_vol;
		katana_get_volume_range(usb_dev, &min_vol, &max_vol, &res_vol);
	}

	// Send GET_CUR request for volume control on channel 1 (left)
	err = katana_ctl_get(usb_dev, UAC_GET_CUR, UAC_VOLUME_CH1, volume_data, 2);
	if (err < 0) {
		pr_err("Katana Control: Failed to get hardware volume: %d\n", err);
		return katana_vol_min; // Return minimum on error
	}

	// Return raw 16-bit signed volume value
	volume_value = volume_data[0] | (volume_data[1] << 8);

	pr_debug("Katana Control: Got raw hardware volume 0x%04x (%d)\n",
		(uint16_t)volume_value, volume_value);
	return volume_value;
}

// Get hardware volume using USB Audio Class control requests (returns percentage)
// Removed unused percentage-based volume get function

// Set hardware mute using USB Audio Class control requests
static int katana_set_hardware_mute(struct usb_device *usb_dev, int mute)
{
	int err;
	unsigned char mute_data;

	// Convert mute value: device uses inverted logic (0 = muted, 1 = unmuted)
	mute_data = mute ? 0 : 1;

	// Send SET_CUR request for mute control on channel 0 (master)
	err = katana_ctl_set(usb_dev, UAC_SET_CUR, UAC_MUTE_CH0, &mute_data, 1);
	if (err < 0) {
		pr_err("Katana Control: Failed to set hardware mute %d: %d\n", mute, err);
		return err;
	}

	return 0;
}

// Get hardware mute using USB Audio Class control requests
static int katana_get_hardware_mute(struct usb_device *usb_dev)
{
	int err;
	unsigned char mute_data;
	int mute;

	// Send GET_CUR request for mute control on channel 0 (master)
	err = katana_ctl_get(usb_dev, UAC_GET_CUR, UAC_MUTE_CH0, &mute_data, 1);
	if (err < 0) {
		pr_err("Katana Control: Failed to get hardware mute: %d\n", err);
		return -1; // Return error as negative value
	}

	// Return mute state: device uses inverted logic (0 = muted, 1 = unmuted)
	// Convert to ALSA standard: 1 = muted, 0 = unmuted
	mute = mute_data ? 0 : 1;
	return mute;
}

// Helper function to get USB device from control
static struct usb_device *get_usb_device_from_control(struct snd_kcontrol *kctl)
{
	struct snd_card *card = kctl->private_data;
	if (!card || !card->private_data) {
		pr_err("Katana Control: No USB device available\n");
		return NULL;
	}
	return (struct usb_device *)card->private_data;
}

int katana_volume_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct usb_device *usb_dev = get_usb_device_from_control(kctl);
	if (!usb_dev) {
		ucontrol->value.integer.value[0] = 0; // Default value
		return 0;
	}
	
	// Get raw volume from device
	int16_t raw_volume = katana_get_hardware_volume_raw(usb_dev);
	if (raw_volume < katana_vol_min) {
		ucontrol->value.integer.value[0] = 0; // Default on error
		return 0;
	}
	
	// Convert raw volume to ALSA steps
	int alsa_steps = (raw_volume - katana_vol_min) / katana_vol_res;
	
	ucontrol->value.integer.value[0] = alsa_steps;
	pr_debug("Katana Control: Volume get - %d steps (raw: %d)\n", alsa_steps, raw_volume);
	return 0;
}

int katana_volume_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct usb_device *usb_dev = get_usb_device_from_control(kctl);
	if (!usb_dev) {
		return 0;
	}
	
	// Initialize volume range if not done already
	if (!katana_vol_range_initialized) {
		int16_t min_vol, max_vol, res_vol;
		katana_get_volume_range(usb_dev, &min_vol, &max_vol, &res_vol);
	}
	
	int alsa_steps = ucontrol->value.integer.value[0];
	
	// Convert ALSA steps to raw volume value
	int16_t raw_volume = katana_vol_min + (alsa_steps * katana_vol_res);
	
	// Clamp to valid range
	if (raw_volume < katana_vol_min) raw_volume = katana_vol_min;
	if (raw_volume > katana_vol_max) raw_volume = katana_vol_max;
	
	int err = katana_set_hardware_volume_raw(usb_dev, raw_volume);
	
	return (err == 0) ? 1 : 0; // Return 1 if changed successfully
}

int katana_volume_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *uinfo)
{
	/*
		Info control callback:
		Write detailed information of this control functionality into snd_ctl_elem_info
		structure.

		More:
		https://www.kernel.org/doc/html/latest/sound/kernel-api/writing-an-alsa-driver.html#info-callback
	*/
	
	// Initialize volume range if not done already (get USB device from control)
	if (!katana_vol_range_initialized) {
		struct usb_device *usb_dev = get_usb_device_from_control(kctl);
		if (usb_dev) {
			int16_t min_vol, max_vol, res_vol;
			katana_get_volume_range(usb_dev, &min_vol, &max_vol, &res_vol);
		}
	}
	
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	
	// Calculate number of steps based on device resolution
	int steps = (katana_vol_max - katana_vol_min) / katana_vol_res;
	
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = steps;
	
	return 0;
}

// Volume control callbacks
int katana_mute_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct usb_device *usb_dev = get_usb_device_from_control(kctl);
	if (!usb_dev) {
		ucontrol->value.integer.value[0] = 1; // Default value
		return 0;
	}
	
	int mute = katana_get_hardware_mute(usb_dev);
	if (mute < 0) {
		mute = 1; // Default on error
	}
	
	ucontrol->value.integer.value[0] = mute;
	pr_debug("Katana Control: Mute get - %d\n", mute);
	return 0;
}

int katana_mute_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct usb_device *usb_dev = get_usb_device_from_control(kctl);
	if (!usb_dev) {
		return 0;
	}
	
	int new_mute = ucontrol->value.integer.value[0];
	
	int err = katana_set_hardware_mute(usb_dev, new_mute);
	
	return (err == 0) ? 1 : 0; // Return 1 if changed successfully
}

int katana_mute_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;

	return 0;
}

// Control structure templates
struct snd_kcontrol_new katana_vol_ctl = {
	.iface	       = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name	       = "PCM Playback Volume", // SOURCE - DIRECTION - FUNCTION
	.index	       = 0,
	.access	       = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.get	       = katana_volume_get,
	.put	       = katana_volume_put,
	.info	       = katana_volume_info,
};

struct snd_kcontrol_new katana_mute_ctl = {
	.iface	       = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name	       = "PCM Playback Switch", // Mute control
	.index	       = 0,
	.access	       = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.get	       = katana_mute_get,
	.put	       = katana_mute_put,
	.info	       = katana_mute_info,
};