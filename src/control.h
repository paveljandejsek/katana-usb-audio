#pragma once

#include <sound/control.h>

struct usb_device;

// Control structure declarations
extern struct snd_kcontrol_new katana_vol_ctl;
extern struct snd_kcontrol_new katana_mute_ctl;

// Control function declarations
int katana_volume_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol);
int katana_volume_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol);
int katana_volume_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *uinfo);

int katana_mute_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol);
int katana_mute_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol);
int katana_mute_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *uinfo);

// Returns 0 if the device answers class requests, or the USB error if not
int katana_ctl_health_check(struct usb_device *usb_dev);
