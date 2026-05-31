# Korg Kronos — Hardware Emulation

Emulating the Korg Kronos NKS4 panel board with an ESP32-P4, enabling the
Kronos synthesizer software to run on commodity x86 hardware with **zero
binary patches** to Korg's proprietary kernel modules.

## Project Status

- ✅ Full USB protocol reverse-engineered (panel, audio, MIDI)
- ✅ QEMU emulation working (OA.ko synth engine fully initialized)
- ✅ Keybed UART protocol decoded and emulated
- ✅ Complete reference documentation (13 chapters)
- ✅ Automated build from Korg Recovery DVD (single `make disk`)
- 🔲 ESP32-P4 firmware implementation
- 🔲 Real hardware test on Intel Atom D525

## Quick Start

```bash
make disk    # Downloads Korg DVDs, builds bootable image
make qemu    # Builds QEMU with custom NKS4 + keybed devices
make run     # Boots the Kronos synth engine in QEMU
```

## What This Does

The Kronos is a professional synthesizer workstation. Its software runs on
an Intel Atom x86 CPU with a custom coprocessor board (NKS4) handling the
display, audio I/O, and keyboard scanning.

This project replaces the NKS4 board with an ESP32-P4 microcontroller,
connected via USB (panel/audio) and UART (keyboard). The result is a
fully functional Kronos running on a standard mini-ITX board.

## Documentation

See the [Architecture](01-architecture.md) chapter for an overview, or
[Emulation](02-emulation.md) for QEMU setup details.

## License

- Code: GPL-3.0
- Documentation: CC BY-NC-SA 4.0
