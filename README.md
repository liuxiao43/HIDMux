# HIDMux

HIDMux is a Linux USB gadget input bridge that forwards physical HID input to a host computer while optionally merging in remote mouse input received over UDP.

It was built and tested with:

* **Physical mouse:** Logitech G Pro X Superlight 2
* **Gadget device:** Raspberry Pi 4 running Raspberry Pi OS Lite 64-bit
* **Host system:** Windows 11

The Raspberry Pi appears to the host as a USB HID mouse and keyboard. The bridge reads physical HID input from the Pi, writes virtual HID reports to Linux USB gadget devices, and can merge UDP mouse movement/button packets into the virtual mouse output.

> This project is intended for authorized local hardware/input experiments only. Do not use it to impersonate commercial USB devices, bypass access controls, or send input to systems you do not own or have explicit permission to test.

---

## What it does

HIDMux combines three input paths:

```text
Physical mouse  ──> /dev/hidraw0 ──┐
                                    ├──> HIDMux ──> /dev/hidg0 ──> Windows host mouse
UDP mouse input ──> UDP port 4242 ──┘

Physical button/keyboard input ──> /dev/hidraw1 ──> HIDMux ──> /dev/hidg1 ──> Windows host keyboard
```

The project has two main parts:

1. `setup_hid_gadget.sh`

   * Creates a Linux USB HID gadget using ConfigFS.
   * Exposes a virtual mouse as `/dev/hidg0`.
   * Exposes a virtual keyboard as `/dev/hidg1`.

2. `hidmux.cpp`

   * Reads physical HID reports from `/dev/hidraw0` and `/dev/hidraw1`.
   * Writes virtual mouse and keyboard reports to `/dev/hidg0` and `/dev/hidg1`.
   * Listens for UDP mouse packets and merges them into the virtual mouse output.

---

## Features

* Bridges a physical mouse into a USB HID gadget mouse.
* Bridges a simple keyboard/button HID input into a USB HID gadget keyboard.
* Merges local physical mouse input with UDP mouse input.
* Supports relative X/Y movement, scroll wheel movement, and mouse buttons.
* Uses Linux USB gadget mode through ConfigFS.
* Defaults to local-only UDP binding for safer development.
* Does not require the Windows host to install a custom driver.

---

## Tested setup

| Component          | Tested configuration           |
| ------------------ | ------------------------------ |
| Physical mouse     | Logitech G Pro X Superlight 2  |
| Gadget device      | Raspberry Pi                   |
| Gadget OS          | Raspberry Pi OS Lite 64-bit    |
| Host OS            | Windows 11                     |
| USB gadget output  | `/dev/hidg0`, `/dev/hidg1`     |
| Physical HID input | `/dev/hidraw0`, `/dev/hidraw1` |
| UDP input          | Port `4242`                    |

Other hardware and distributions may work, but device paths, USB controller names, and HID report layouts may differ.

---

## Important USB identity note

Do **not** publish or run this project using another company's USB Vendor ID, Product ID, manufacturer string, or product string.

The physical mouse used during testing was a Logitech G Pro X Superlight 2, but the USB gadget created by this project should use your own appropriate USB identifiers and neutral product strings.

The setup script intentionally requires you to provide:

```bash
USB_VENDOR_ID=0x1234
USB_PRODUCT_ID=0x0001
```

Replace those example values with appropriate IDs for your own testing environment.

---

## Requirements

### Hardware

* Raspberry Pi or another Linux device that supports USB gadget/device mode.
* USB cable connected from the Pi's USB device-mode port to the host computer.
* Physical HID device connected to the Pi.
* Host computer to receive the virtual USB mouse/keyboard input.

### Software

On the Raspberry Pi:

* Raspberry Pi OS Lite 64-bit
* Linux kernel with USB gadget support
* `configfs`
* `libcomposite`
* `g++`
* `xxd`
* `make` or a manual build command

Install common build tools:

```bash
sudo apt update
sudo apt install -y build-essential xxd
```

---

## Repository layout

Recommended layout:

```text
hidmux/
├── README.md
├── LICENSE
├── src/
│   └── hidmux.cpp
├── scripts/
│   └── setup_hid_gadget.sh
└── .gitignore
```

---

## Build

From the repository root:

```bash
g++ -std=c++17 -Wall -Wextra -O2 -pthread src/hidmux.cpp -o hidmux
```

Or, if `hidmux.cpp` is in the current directory:

```bash
g++ -std=c++17 -Wall -Wextra -O2 -pthread hidmux.cpp -o hidmux
```

---

## Raspberry Pi USB gadget setup

Run the setup script as root and provide your USB IDs:

```bash
sudo env USB_VENDOR_ID=0x1234 USB_PRODUCT_ID=0x0001 ./scripts/setup_hid_gadget.sh
```

Optional: allow a non-root user in a specific group to access `/dev/hidg0` and `/dev/hidg1`:

```bash
sudo env \
  USB_VENDOR_ID=0x1234 \
  USB_PRODUCT_ID=0x0001 \
  HID_GROUP=plugdev \
  ./scripts/setup_hid_gadget.sh
```

If you use `HID_GROUP=plugdev`, make sure your user is in that group:

```bash
sudo usermod -aG plugdev "$USER"
```

Then log out and back in for the group change to apply.

After setup, confirm that the gadget devices exist:

```bash
ls -l /dev/hidg0 /dev/hidg1
```

---

## Run HIDMux

Default run command:

```bash
./hidmux \
  --mouse-in /dev/hidraw0 \
  --key-in /dev/hidraw1 \
  --mouse-out /dev/hidg0 \
  --key-out /dev/hidg1
```

If `/dev/hidg0` and `/dev/hidg1` are root-only:

```bash
sudo ./hidmux \
  --mouse-in /dev/hidraw0 \
  --key-in /dev/hidraw1 \
  --mouse-out /dev/hidg0 \
  --key-out /dev/hidg1
```

By default, the UDP listener binds to:

```text
127.0.0.1:4242
```

This is intentionally local-only. To accept UDP input from another device on the network, explicitly opt in:

```bash
./hidmux --allow-remote --bind 0.0.0.0
```

Only use remote binding on a trusted network.

---

## Command-line options

```text
--mouse-in PATH       Physical mouse hidraw input device
                      Default: /dev/hidraw0

--key-in PATH         Physical keyboard/button hidraw input device
                      Default: /dev/hidraw1

--mouse-out PATH      Virtual mouse HID gadget output device
                      Default: /dev/hidg0

--key-out PATH        Virtual keyboard HID gadget output device
                      Default: /dev/hidg1

--bind ADDRESS        UDP bind address
                      Default: 127.0.0.1

--udp-port PORT       UDP port for network mouse packets
                      Default: 4242

--allow-remote        Allow binding UDP input to a non-local address

--poll-us MICROSEC    Poll interval in microseconds
                      Default: 1000

--help                Show usage information
```

---

## UDP mouse packet format

HIDMux expects UDP packets with the following 6-byte layout:

```text
Bytes 0-1: dx      int16 little-endian
Bytes 2-3: dy      int16 little-endian
Byte  4:   wheel   int8
Byte  5:   buttons uint8
```

Button bit layout:

```text
Bit 0: Left button
Bit 1: Right button
Bit 2: Middle button
Bit 3: Back button
Bit 4: Forward button
Bits 5-7: Unused
```

The UDP movement values are accumulated until the mouse bridge thread consumes them. The latest UDP button mask is merged with the physical mouse button state.

---

## HID report layout

### Virtual mouse report

The virtual mouse report is 7 bytes:

```text
Byte 0:   Report ID, always 0x01
Byte 1:   Buttons
Bytes 2-3: X delta, int16 little-endian
Bytes 4-5: Y delta, int16 little-endian
Byte 6:   Wheel delta, int8
```

### Virtual keyboard report

The virtual keyboard report is the standard 8-byte boot keyboard format:

```text
Byte 0:   Modifier keys
Byte 1:   Reserved
Bytes 2-7: Up to six keycodes
```

Current key mappings:

```text
Input mask 0x04 -> HID keycode 0x26 -> '9'
Input mask 0x08 -> HID keycode 0x27 -> '0'
```

---

## Finding the correct hidraw devices

The default paths are:

```text
/dev/hidraw0
/dev/hidraw1
```

These may differ on your system. To inspect HID devices:

```bash
ls -l /dev/hidraw*
```

You can also check kernel messages after plugging in devices:

```bash
dmesg | tail -n 50
```

If the mouse or keyboard input appears under different paths, pass them explicitly:

```bash
./hidmux --mouse-in /dev/hidraw2 --key-in /dev/hidraw3
```

---

## Troubleshooting

### `/dev/hidg0` or `/dev/hidg1` does not exist

The USB gadget was not created or was not bound to a USB Device Controller.

Try:

```bash
sudo ./scripts/setup_hid_gadget.sh
```

Also check:

```bash
ls /sys/class/udc
```

If that directory is empty, the device may not support USB gadget mode, or the USB cable may be connected to a host-only port.

### Permission denied when opening `/dev/hidg0` or `/dev/hidg1`

Either run HIDMux with `sudo`, or rerun the setup script with `HID_GROUP`:

```bash
sudo env USB_VENDOR_ID=0x1234 USB_PRODUCT_ID=0x0001 HID_GROUP=plugdev ./scripts/setup_hid_gadget.sh
```

### The host does not detect the virtual mouse or keyboard

Check that the gadget is bound:

```bash
cat /sys/kernel/config/usb_gadget/hidmux/UDC
```

Check kernel logs:

```bash
dmesg | tail -n 100
```

Unplug and reconnect the USB cable after recreating the gadget.

### Mouse movement works but buttons release unexpectedly

Make sure the bridge is using non-blocking reads for the physical mouse input and preserving the last known physical button state between reports. This prevents network-only movement from accidentally clearing held physical buttons.

### UDP input does not work from another machine

By default, HIDMux binds to `127.0.0.1`, which only accepts local UDP packets.

To receive packets from another device on the network:

```bash
./hidmux --allow-remote --bind 0.0.0.0
```

Then make sure your firewall and network allow UDP traffic to the configured port.

Only do this on a trusted network.

---

## Security considerations

HIDMux can send mouse and keyboard input to a host computer. Treat it like any other tool that can control a machine through USB input.

Recommended safety defaults:

* Keep UDP bound to `127.0.0.1` unless remote input is required.
* Do not expose the UDP listener on untrusted networks.
* Do not run with world-writable HID gadget devices.
* Do not impersonate commercial USB devices.
* Only connect the gadget to systems you own or have permission to test.

---

## Known limitations

* The physical HID report layouts are currently tailored to the tested device setup.
* `/dev/hidraw0` and `/dev/hidraw1` are not guaranteed to be stable across reboots or different hardware.
* UDP input does not include authentication or encryption.
* The keyboard bridge currently maps only two input mask bits to the `9` and `0` keys.
* The mouse report descriptor is custom and may not behave identically across all host systems.

---

## Future improvements

Possible improvements:

* Add a config file for device paths, UDP settings, and key mappings.
* Add udev rules for stable physical HID input names.
* Add optional authentication for UDP input.
* Add a companion Windows sender utility.
* Add systemd service files for startup on boot.
* Add structured logging.
* Add graceful shutdown and gadget cleanup commands.

---

## License

* MIT License for permissive open-source use.

---

## Disclaimer

This project is provided for educational and authorized hardware development purposes. The maintainers are not responsible for misuse, damage, data loss, policy violations, or unauthorized use.
