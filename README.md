# IR flood LED for the HM1092 IR camera on Dell XPS 16 9640 (Linux)

On the Dell XPS 16 9640 (Intel Meteor Lake, IPU6), the Himax HM1092 IR camera
works under Linux, but its **IR LEDs never turn on**. The picture is lit by
ambient light only, so face login (Howdy) does not work in the dark.

The LEDs are driven by a **TI LM3643** LED driver on I2C. Linux has no driver
bound to it, so nothing ever switches it on. This repository contains:

- `driver/`: a small kernel module (`lm3643-ir`, DKMS) that binds to the
  LM3643 and exposes the LEDs as `/sys/class/leds/lm3643:ir:flood`.
- `ir-bridge/`: a helper that turns the camera's raw 10-bit stream into an
  8-bit GREY stream on a v4l2loopback device (usable by OpenCV and Howdy).
  It switches the LED on while it runs.

## Why the usual fixes don't work

The usual IR LED fixes for IPU6 laptops look for an `ir_flood` LED, which
`int3472` creates from a GPIO of type 0x02 (STROBE) in the camera power
device's `_DSM`. On this machine the camera power device
(`\_SB.PC00.DSC0`, `INT3472:0c`) lists only two GPIOs, reset and avdd, so
there is no `ir_flood` LED. The HM1092 has no strobe registers either.

The LED driver is a separate ACPI device in the DSDT:

```
Device (FLM0)            // _STA = L0FD (enabled), _HID = "TXNW3643"
  _CRS: I2cSerialBus 0x63 on the camera I2C bus (same bus as the HM1092)
  _DSM: "LM3643"
```

Under Linux it shows up as `i2c-TXNW3643:00` with no driver bound. Dell's
Windows camera package ships `lm3643.sys` for it. Register map and currents
follow the TI datasheet (SNVS967A).

### Does your laptop have it?

```sh
cat /sys/bus/acpi/devices/TXNW3643:00/status   # 15 = present and enabled
cat /sys/bus/acpi/devices/TXNW3643:00/path     # \_SB_.PC00.FLM0
ls /sys/bus/i2c/devices/ | grep TXNW3643       # i2c-TXNW3643:00
```

Other Dell laptops with an HM1092 IR camera may use the same design. Reports
are welcome.

## Tested on

- Dell XPS 16 9640, BIOS 1.27.0
- Arch Linux, kernel 7.3.6, libcamera 0.7.2
- HM1092 sensor driver: out-of-tree (linux-media HM1092 v6 series, DKMS)

## Install the driver (DKMS)

```sh
sudo cp -r driver /usr/src/lm3643-ir-0.1
sudo dkms install lm3643-ir/0.1
sudo cp driver/99-lm3643-ir.rules /etc/udev/rules.d/
sudo modprobe lm3643-ir
```

The module loads automatically at boot (ACPI alias `TXNW3643`). The udev rule
lets members of the `video` group switch the LED.

Test it. Point a phone camera at the LEDs next to the webcam: IR light shows
up as purple.

```sh
echo 63 > /sys/class/leds/lm3643:ir:flood/brightness   # on
echo 0  > /sys/class/leds/lm3643:ir:flood/brightness   # off
```

`dmesg` should show `lm3643-ir i2c-TXNW3643:00: LM3643 rev 2, IR flood LED max code 63`.

### Brightness and safety

The driver uses torch (continuous) mode with both LED outputs at the same
current: **I = brightness × 1.4 mA + 0.98 mA**. By default it is capped at
63 (89 mA, the chip's power-on default). The chip allows up to 127 (179 mA)
in torch mode:

```sh
sudo modprobe lm3643-ir max_code=127
```

The ratings of the IR LEDs themselves are unknown, so raise the cap at your
own risk. The LED switches off on suspend, shutdown and module unload.

Windows probably uses the LM3643's IR mode (flash current, pulsed by the
STROBE pin). This driver does not use it, because the STROBE wiring is not
known.

## ir-bridge

libcamera exposes the HM1092 only as raw `R10` (648×368 @ 30 fps), which
most applications can't read. `ir-bridge` captures it, runs a simple
auto-exposure via the sensor subdev controls, and writes GREY frames to a
v4l2loopback device.

```sh
sudo pacman -S libcamera v4l2loopback-dkms   # or your distro's packages
cd ir-bridge && make
sudo modprobe v4l2loopback video_nr=42 card_label="IR camera" exclusive_caps=1
./ir-view.sh -s        # live view with ffplay, LED on
./ir-bridge -d /dev/video42 -t 10   # 10 s stream, e.g. for Howdy
```

Options: `-L N` sets the LED brightness (default: the driver's maximum,
`0` = off). `-e` and `-g` set a fixed exposure and gain, and `-v` prints
brightness once per second. Run `./ir-bridge --help` for all options.

The IR and RGB cameras share one media device. Don't run the bridge
permanently, or the RGB camera stays blocked. Start it only for as long as you
need it (e.g. with `-t`).

## License

GPL-2.0-only. See `LICENSE`.
