# 4dpi_32-ng
4DPi-32  display driver for raspbian trixie.

# 4DPi-32 Non-Compressed Driver for Linux 6.x (Raspberry Pi OS Trixie)

This repository provides a 100% open-source, modernized clean driver for the 4D Systems 4DPi-32 display, specifically targeted for **Linux Kernel 6.x (Raspberry Pi OS Trixie / armv6l)** on legacy Raspberry Pi models.

It removes the deprecated clunky closed-source compressed binary blob (`compress-v6.o`) and directly communicates via the original 4D control-byte raw SPI protocol.

## How to Install

### 1. Clone and Build
```bash
git clone https://github.com/tatsushiok/4dpi_32-ng/
cd rpi-4dpi-nocompress

# Compile the kernel module
make
sudo mkdir -p /lib/modules/$(shell uname -r)/kernel/drivers/video/fbdev/4dpi
sudo cp 4dpi_nocompress.ko /lib/modules/$(shell uname -r)/kernel/drivers/video/fbdev/4dpi/
sudo depmod -a

# Compile and install the Device Tree Overlay
dtc -@ -I dts -O dtb -o 4dpi-32-nocompress.dtbo 4dpi-32-nocompress.dts
sudo cp 4dpi-32-nocompress.dtbo /boot/firmware/overlays/
```

### 2. Configure System
Add the following line to your `/boot/firmware/config.txt` to enable the hardware overlay at boot:

```text

#dtoverlay=spi0-0
dtoverlay=4dpi-32-nocompress,speed=48000000,rotate=0
```

*Note: Make sure to comment out any conflicting `dtoverlay=spi0-0` or manual spidev overlays.*

### 3. Reboot and Verify
Reboot your Raspberry Pi. The display will automatically awaken during boot with the Linux console console text!

To verify that the driver successfully auto-bound to the hardware, check:
```bash
cat /sys/bus/spi/devices/spi0.0/modalias
# Output should be: spi:4dpi-32-nocompress

ls -l /dev/fb*
# You will see /dev/fb1 successfully registered!
```

## How to Display Images
Since the primary GPU (`vc4-kms-v3d`) captures `/dev/fb0` for HDMI, this display is cleanly assigned as `/dev/fb1`. You can test rendering high-quality images using standard `fbi`:

```bash
sudo apt update && sudo apt install -y fbi
sudo fbi -d /dev/fb1 -T 1 -a /path/to/your/image.jpg
```

## Limitations
no touch screen support.
No Backlight Control

## Credits / Derived From
Derived conceptually from `trichner/4d-hats` and the original 4D Systems driver repository. Remodelled completely to support modern virtual page mapping structures (`vmalloc`, `FBINFO_VIRTFB`, and `fb_deferred_io_mmap`) to completely prevent kernel Oops on page faults.

## 📄 License
GPL-2.0
