#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

DISK="local/kronos.img"
KERNEL="local/bzImage-korg"
QEMU="vendor/qemu/build/qemu-system-i386"

[ -f "$DISK" ] || { echo "ERROR: Run 'make disk' first."; exit 1; }
[ -f "$KERNEL" ] || { echo "ERROR: Run 'make disk' first."; exit 1; }
[ -f "$QEMU" ] || { echo "ERROR: Run 'make qemu' first."; exit 1; }

exec "$QEMU" -m 2048 -smp 1 -net none -cpu Westmere \
    -kernel "$KERNEL" \
    -append "root=/dev/sda2 max_loop=16 loglevel=5 nosoftlockup fastboot Single raid=noautodetect elevator=noop memmap=0x80000000@0 memmap=384m console=ttyS0,115200 lapic" \
    -device ahci,id=ahci \
    -drive id=disk0,file="$DISK",format=raw,if=none \
    -device ide-hd,drive=disk0,bus=ahci.0 \
    -usb -device usb-nks4 \
    -device kronos-keybed \
    -serial stdio -display none -no-reboot
