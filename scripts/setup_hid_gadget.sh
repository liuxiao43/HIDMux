#!/usr/bin/env bash
set -euo pipefail

GADGET_NAME="${GADGET_NAME:-hidmux}"
CONFIGFS_ROOT="${CONFIGFS_ROOT:-/sys/kernel/config/usb_gadget}"
CONFIG_DIR="$CONFIGFS_ROOT/$GADGET_NAME"

USB_VENDOR_ID="${USB_VENDOR_ID:-}"
USB_PRODUCT_ID="${USB_PRODUCT_ID:-}"
USB_DEVICE_BCD="${USB_DEVICE_BCD:-0x0100}"
USB_SPEC_BCD="${USB_SPEC_BCD:-0x0200}"

USB_SERIAL="${USB_SERIAL:-00000001}"
USB_MANUFACTURER="${USB_MANUFACTURER:-HIDMux Project}"
USB_PRODUCT="${USB_PRODUCT:-HIDMux Virtual Input Bridge}"

CONFIG_NAME="${CONFIG_NAME:-Config 1}"
MAX_POWER="${MAX_POWER:-100}"
BM_ATTRIBUTES="${BM_ATTRIBUTES:-0x80}"

UDC_NAME="${UDC_NAME:-}"
HID_GROUP="${HID_GROUP:-}"
HID_MODE="${HID_MODE:-660}"

require_root() {
    if [[ "$EUID" -ne 0 ]]; then
        echo "Error: this script must be run as root."
        echo "Example:"
        echo "  sudo env USB_VENDOR_ID=0x1234 USB_PRODUCT_ID=0x0001 $0"
        exit 1
    fi
}

require_usb_ids() {
    if [[ -z "$USB_VENDOR_ID" || -z "$USB_PRODUCT_ID" ]]; then
        echo "Error: USB_VENDOR_ID and USB_PRODUCT_ID must be set."
        echo
        echo "Do not publish this project with another company's VID/PID."
        echo "Set IDs explicitly when running the script, for example:"
        echo
        echo "  sudo env USB_VENDOR_ID=0x1234 USB_PRODUCT_ID=0x0001 $0"
        echo
        exit 1
    fi
}

require_tools() {
    if ! command -v xxd >/dev/null 2>&1; then
        echo "Error: xxd is required but was not found."
        echo "Install it with your distro's vim-common/xxd package."
        exit 1
    fi
}

prepare_configfs() {
    modprobe libcomposite 2>/dev/null || true

    if ! mountpoint -q /sys/kernel/config; then
        mount -t configfs none /sys/kernel/config
    fi

    if [[ ! -d "$CONFIGFS_ROOT" ]]; then
        echo "Error: configfs USB gadget directory not available: $CONFIGFS_ROOT"
        exit 1
    fi
}

cleanup_gadget() {
    if [[ ! -d "$CONFIG_DIR" ]]; then
        return
    fi

    echo "Cleaning up existing gadget: $GADGET_NAME"

    if [[ -e "$CONFIG_DIR/UDC" ]]; then
        echo "" > "$CONFIG_DIR/UDC" 2>/dev/null || true
    fi

    rm -f "$CONFIG_DIR/configs/c.1/hid.usb0" 2>/dev/null || true
    rm -f "$CONFIG_DIR/configs/c.1/hid.usb1" 2>/dev/null || true

    rmdir "$CONFIG_DIR/configs/c.1/strings/0x409" 2>/dev/null || true
    rmdir "$CONFIG_DIR/configs/c.1" 2>/dev/null || true

    rmdir "$CONFIG_DIR/functions/hid.usb0" 2>/dev/null || true
    rmdir "$CONFIG_DIR/functions/hid.usb1" 2>/dev/null || true

    rmdir "$CONFIG_DIR/strings/0x409" 2>/dev/null || true
    rmdir "$CONFIG_DIR" 2>/dev/null || true
}

write_attr() {
    local path="$1"
    local value="$2"
    printf "%s\n" "$value" > "$path"
}

write_report_desc() {
    local hex="$1"
    local path="$2"

    echo "${hex// /}" | xxd -r -p > "$path"
}

create_gadget_root() {
    mkdir -p "$CONFIG_DIR"
    cd "$CONFIG_DIR"

    write_attr idVendor "$USB_VENDOR_ID"
    write_attr idProduct "$USB_PRODUCT_ID"
    write_attr bcdDevice "$USB_DEVICE_BCD"
    write_attr bcdUSB "$USB_SPEC_BCD"

    mkdir -p strings/0x409
    write_attr strings/0x409/serialnumber "$USB_SERIAL"
    write_attr strings/0x409/manufacturer "$USB_MANUFACTURER"
    write_attr strings/0x409/product "$USB_PRODUCT"
}

create_mouse_function() {
    mkdir -p functions/hid.usb0

    # Custom report-descriptor-based HID mouse.
    # Not marked as a boot mouse because this report uses Report ID 0x01 and
    # 16-bit relative X/Y axes rather than the standard boot mouse layout.
    write_attr functions/hid.usb0/protocol 0
    write_attr functions/hid.usb0/subclass 0
    write_attr functions/hid.usb0/report_length 7

    local mouse_hex
    mouse_hex="05 01 09 02 a1 01 85 01 09 01 a1 00 05 09 19 01 29 05 15 00 25 01 95 05 75 01 81 02 95 01 75 03 81 01 05 01 09 30 09 31 16 01 80 26 ff 7f 75 10 95 02 81 06 09 38 15 81 25 7f 75 08 95 01 81 06 c0 c0"

    write_report_desc "$mouse_hex" functions/hid.usb0/report_desc
}

create_keyboard_function() {
    mkdir -p functions/hid.usb1

    # Standard 8-byte boot keyboard report:
    # byte 0 = modifiers, byte 1 = reserved, bytes 2-7 = up to six keycodes.
    write_attr functions/hid.usb1/protocol 1
    write_attr functions/hid.usb1/subclass 1
    write_attr functions/hid.usb1/report_length 8

    local keyboard_hex
    keyboard_hex="05 01 09 06 a1 01 05 07 19 e0 29 e7 15 00 25 01 75 01 95 08 81 02 95 01 75 08 81 01 95 06 75 08 15 00 25 65 05 07 19 00 29 65 81 00 c0"

    write_report_desc "$keyboard_hex" functions/hid.usb1/report_desc
}

create_configuration() {
    mkdir -p configs/c.1/strings/0x409

    write_attr configs/c.1/bMaxPower "$MAX_POWER"
    write_attr configs/c.1/bmAttributes "$BM_ATTRIBUTES"
    write_attr configs/c.1/strings/0x409/configuration "$CONFIG_NAME"

    ln -s functions/hid.usb0 configs/c.1/
    ln -s functions/hid.usb1 configs/c.1/
}

bind_udc() {
    if [[ -z "$UDC_NAME" ]]; then
        UDC_NAME="$(ls /sys/class/udc 2>/dev/null | head -n 1 || true)"
    fi

    if [[ -z "$UDC_NAME" ]]; then
        echo "Error: no USB Device Controller found in /sys/class/udc."
        echo "This board/kernel may not support USB gadget device mode."
        exit 1
    fi

    write_attr UDC "$UDC_NAME"
}

set_hid_permissions() {
    for dev in /dev/hidg0 /dev/hidg1; do
        if [[ ! -e "$dev" ]]; then
            echo "Warning: expected device not found: $dev"
            continue
        fi

        if [[ -n "$HID_GROUP" ]]; then
            chgrp "$HID_GROUP" "$dev"
            chmod "$HID_MODE" "$dev"
        else
            chmod 600 "$dev"
        fi
    done
}

print_summary() {
    echo
    echo "Gadget setup complete."
    echo "Name:         $GADGET_NAME"
    echo "Manufacturer:$USB_MANUFACTURER"
    echo "Product:     $USB_PRODUCT"
    echo "VID:PID:     $USB_VENDOR_ID:$USB_PRODUCT_ID"
    echo "UDC:         $UDC_NAME"
    echo

    ls -l /dev/hidg0 /dev/hidg1 2>/dev/null || true

    echo
    if [[ -z "$HID_GROUP" ]]; then
        echo "HID gadget devices are root-only."
        echo "Run the bridge with sudo, or rerun this script with HID_GROUP set."
        echo "Example:"
        echo "  sudo env USB_VENDOR_ID=$USB_VENDOR_ID USB_PRODUCT_ID=$USB_PRODUCT_ID HID_GROUP=plugdev $0"
    else
        echo "HID gadget devices are accessible to group: $HID_GROUP"
    fi
}

main() {
    require_root
    require_usb_ids
    require_tools
    prepare_configfs
    cleanup_gadget
    create_gadget_root
    create_mouse_function
    create_keyboard_function
    create_configuration
    bind_udc
    set_hid_permissions
    print_summary
}

main "$@"
