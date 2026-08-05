obj-m += katana_usb_audio.o

PWD := $(CURDIR)

# Build against the kernel DKMS is targeting, not the running one. DKMS exports
# KERNELRELEASE for the kernel being built for, which differs from `uname -r`
# during a kernel upgrade (the new kernel is installed while the old one runs).
KVER ?= $(if $(KERNELRELEASE),$(KERNELRELEASE),$(shell uname -r))
KDIR ?= /lib/modules/$(KVER)/build
UDEV_RULES_DIR := /etc/udev/rules.d
MODULE_DIR := /lib/modules/$(KVER)/extra

katana_usb_audio-objs := src/control.o src/pcm.o src/usb.o src/katana_usb_audio.o

all:
	make -C $(KDIR) M=$(PWD) modules

clean: 
	make -C $(KDIR) M=$(PWD) clean

install: all
	@echo "############################################################"
	@echo "# WARNING: this installs katana_usb_audio.ko into"
	@echo "#   $(MODULE_DIR)"
	@echo "# On Red Hat based distros DKMS installs there too (it"
	@echo "# overrides DEST_MODULE_LOCATION from dkms.conf), as"
	@echo "# katana_usb_audio.ko.xz. Two copies of one module in one"
	@echo "# directory leaves it to depmod which one wins, so a stray"
	@echo "# 'make install' can quietly outlive every later DKMS"
	@echo "# rebuild, including those after a kernel update."
	@echo "#"
	@echo "# Use 'make dkms-install' instead unless you know why you"
	@echo "# want this. To undo later: 'make uninstall'."
	@echo "############################################################"
	@echo "Installing Katana USB Audio driver..."
	mkdir -p $(MODULE_DIR)
	cp katana_usb_audio.ko $(MODULE_DIR)/
	depmod -a
	@echo "Installing udev rule for driver priority..."
	cp 99-katana-usb-audio.rules $(UDEV_RULES_DIR)/
	udevadm control --reload-rules
	@echo "Installation complete!"
	@echo ""
	@echo "To load the driver now, run:"
	@echo "  sudo modprobe katana_usb_audio"
	@echo ""
	@echo "To unload conflicting snd-usb-audio driver:"
	@echo "  sudo modprobe -r snd_usb_audio"
	@echo "  sudo modprobe katana_usb_audio"

uninstall:
	@echo "Uninstalling Katana USB Audio driver..."
	modprobe -r katana_usb_audio 2>/dev/null || true
	rm -f $(MODULE_DIR)/katana_usb_audio.ko
	rm -f $(UDEV_RULES_DIR)/99-katana-usb-audio.rules
	depmod -a
	udevadm control --reload-rules
	@echo "Uninstall complete!"

dkms-install:
	@echo "Installing via DKMS..."
	@chmod +x install-dkms.sh
	@./install-dkms.sh

dkms-uninstall:
	@echo "Uninstalling from DKMS..."
	@chmod +x uninstall-dkms.sh
	@./uninstall-dkms.sh

dkms-status:
	@echo "DKMS status for katana-usb-audio:"
	@dkms status katana-usb-audio 2>/dev/null || echo "Package not found in DKMS"

.PHONY: all clean install uninstall dkms-install dkms-uninstall dkms-status