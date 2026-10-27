#!/bin/bash

# Katana USB Audio Driver DKMS Installation Script
# This script installs the driver using DKMS for automatic rebuilding on kernel updates

set -e

PACKAGE_NAME="katana-usb-audio"
PACKAGE_VERSION="1.0"
MODULE_DIR="/usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}"
UDEV_RULES_DIR="/etc/udev/rules.d"

echo "Installing Katana USB Audio Driver via DKMS..."

# Check if running as root
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root (use sudo)" 
   exit 1
fi

# Check if DKMS is installed
if ! command -v dkms &> /dev/null; then
    echo "Error: DKMS is not installed. Please install dkms package first."
    echo "On Ubuntu/Debian: sudo apt install dkms"
    echo "On Fedora/RHEL: sudo dnf install dkms"
    echo "On Arch: sudo pacman -S dkms"
    exit 1
fi

# Remove existing installation if present
if dkms status | grep -q "$PACKAGE_NAME"; then
    echo "Removing existing DKMS installation..."
    dkms remove "$PACKAGE_NAME/$PACKAGE_VERSION" --all 2>/dev/null || true
fi

# Remove old source directory if it exists
if [ -d "$MODULE_DIR" ]; then
    echo "Removing old source directory..."
    rm -rf "$MODULE_DIR"
fi

# Create source directory
echo "Creating source directory: $MODULE_DIR"
mkdir -p "$MODULE_DIR"

# Copy source files. Only sources: a wholesale copy of src/ would also carry
# over .o/.cmd files from a local `make`, and DKMS copies them on into its build
# directory with their timestamps intact. If a .c was edited since that local
# build, the stale .o can look up to date, make skips it, and DKMS installs the
# old code while reporting success.
echo "Copying source files..."
mkdir -p "$MODULE_DIR/src"
cp src/*.c src/*.h "$MODULE_DIR/src/"
cp Makefile "$MODULE_DIR/"
cp dkms.conf "$MODULE_DIR/"
cp 99-katana-usb-audio.rules "$MODULE_DIR/"

# Add to DKMS
echo "Adding module to DKMS..."
dkms add "$PACKAGE_NAME/$PACKAGE_VERSION"

# Build the module
echo "Building module with DKMS..."
dkms build "$PACKAGE_NAME/$PACKAGE_VERSION"

# Install the module
echo "Installing module with DKMS..."
dkms install "$PACKAGE_NAME/$PACKAGE_VERSION"

# Install udev rules
echo "Installing udev rules..."
cp 99-katana-usb-audio.rules "$UDEV_RULES_DIR/"
udevadm control --reload-rules

# Load the new driver
echo "Loading katana_usb_audio driver..."
modprobe katana_usb_audio

# Hand the Katana's interfaces over from snd-usb-audio, if it holds them.
#
# This is deliberately targeted rather than "modprobe -r snd_usb_audio": that
# would take down every unrelated USB audio device on the system, and it fails
# outright whenever one of them is in use, leaving the Katana still bound to
# snd-usb-audio while the script reported success.
#
# Must run after the modprobe above, or /sys/bus/usb/drivers/katana_usb_audio/
# does not exist yet and every bind write fails.
echo "Binding Katana interfaces to katana_usb_audio..."
bound=0
for dev in /sys/bus/usb/devices/*; do
    [ -f "$dev/idVendor" ] || continue
    [ "$(cat "$dev/idVendor")" = "041e" ] || continue
    [ "$(cat "$dev/idProduct")" = "3247" ] || continue

    for iface in "$dev":*; do
        [ -e "$iface" ] || continue
        name=$(basename "$iface")
        # Interfaces other than 0 and 1 are rejected by this driver's probe,
        # so the bind write failing for them is expected.
        echo "$name" > /sys/bus/usb/drivers/snd-usb-audio/unbind 2>/dev/null || true
        echo "$name" > /sys/bus/usb/drivers/katana_usb_audio/bind 2>/dev/null || true
    done

    bound=1
done

if [ "$bound" -eq 0 ]; then
    echo "Note: no Katana found on the USB bus - it will be claimed on plug-in."
fi

echo ""
# Verify rather than assume. A failed bind is silent (the writes above are
# guarded for set -e), and looks exactly like a successful one from here.
if [ -n "$(ls -A /sys/bus/usb/drivers/katana_usb_audio/ 2>/dev/null | grep ':' || true)" ]; then
    echo "✅ DKMS installation complete - Katana bound to katana_usb_audio"
elif [ "$bound" -eq 0 ]; then
    echo "✅ DKMS installation complete - plug in the Katana to use it"
else
    echo "⚠️  DKMS installation complete, but the Katana did not bind."
    echo "   Replug the device or reboot, then check:"
    echo "     ls /sys/bus/usb/drivers/katana_usb_audio/"
    echo "     dmesg | grep -i katana"
fi
echo ""
echo "The driver will now automatically rebuild when you install new kernels."
echo ""
echo "To check installation status:"
echo "  dkms status"
echo ""
echo "To manually load/unload the driver:"
echo "  sudo modprobe katana_usb_audio     # Load"
echo "  sudo modprobe -r katana_usb_audio  # Unload"
echo ""
echo "To uninstall, run: sudo ./uninstall-dkms.sh" 