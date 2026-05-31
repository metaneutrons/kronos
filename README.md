# Korg Kronos — Hardware Emulation

[![Documentation](https://img.shields.io/badge/docs-GitHub%20Pages-blue)](https://metaneutrons.github.io/kronos/)
[![License: GPL v3](https://img.shields.io/badge/Code-GPLv3-blue.svg)](LICENSE)
[![Docs: CC BY-NC-SA](https://img.shields.io/badge/Docs-CC%20BY--NC--SA%204.0-lightgrey.svg)](https://creativecommons.org/licenses/by-nc-sa/4.0/)

## Overview

The **Korg Kronos** is a professional synthesizer workstation built around an Intel Atom x86 CPU running a real-time Linux kernel (RTAI). A custom coprocessor board — the **NKS4** — handles the 800×600 display, capacitive touch panel, rotary encoders, buttons, 48kHz/24-bit 6-channel audio I/O, and keyboard scanning.

This project **replaces the NKS4 board** with an ESP32-P4 microcontroller, enabling the complete Kronos software stack to run on commodity x86 hardware (Intel Atom D525 mini-ITX) with no modifications to Korg's proprietary kernel modules.

```
┌─────────────────────────┐         ┌─────────────────────────┐
│   Intel Atom D525       │   USB   │      ESP32-P4           │
│                         │◄───────►│                         │
│  Linux 2.6.32 + RTAI   │         │  USB: Panel/Audio/MIDI  │
│  OA.ko (Synth Engine)  │   UART  │  UART: Keyboard         │
│  Eva (GUI)             │◄───────►│  LCD + Touch + DAC      │
│                         │         │  Key Scanner            │
└─────────────────────────┘         └─────────────────────────┘
```

### Key Achievements

- **Full USB protocol** reverse-engineered from binary kernel modules (no source code, no hardware captures)
- **OA.ko synth engine** (14MB, real-time DSP) fully initializes in QEMU
- **Keybed UART protocol** decoded — physical 16550 serial, not USB
- **5 bytes patched** in QEMU (4 anti-tamper NOPs + 1 Atmel auth skip)
- **Zero patches** required on real hardware with ESP32 providing Atmel credentials

## Quick Start

```bash
git clone --recurse-submodules https://github.com/metaneutrons/kronos.git
cd kronos
make disk    # Downloads Korg recovery media (~8GB), builds bootable image
make qemu    # Builds QEMU with custom NKS4 + keybed devices
make run     # Boots the Kronos synth engine in QEMU
```

### Prerequisites

| Tool | Purpose |
|------|---------|
| `curl` | Download recovery media from Korg CDN |
| `7z` | Extract ISO contents |
| `python3` + `pycryptodome` | Decrypt AES-256-CBC filesystem images |
| `objdump` | Locate patch offsets in kernel modules |
| `gcc`, `ninja`, `meson` | Build QEMU |
| `sudo` | Mount/format disk image partitions |

## How It Works

The build system downloads Korg's publicly available recovery DVDs, decrypts the filesystem images using known AES keys, applies minimal binary patches, and produces a bootable disk image. QEMU runs the unmodified Korg kernel with two custom emulated devices:

| Device | Function |
|--------|----------|
| `kronos-nks4` | USB composite device (panel commands, DRM handshake, video, audio) |
| `kronos-keybed` | ISA Super I/O + 16550 UART (keyboard handshake) |

### What Gets Patched (QEMU Only)

| Module | Bytes | Reason |
|--------|-------|--------|
| `loadmod.ko` | 4 × 6-byte NOP | Anti-tamper checks fail (QEMU vmalloc addresses differ) |
| `OA.ko` | 1 byte (`je` → `jmp`) | Atmel crypto chip not present |

On physical hardware with an ESP32 providing correct Atmel responses and keyboard UART, **no patches are needed**.

## Project Structure

```
src/qemu/               Custom QEMU device sources
  kronos-nks4.c           USB NKS4 panel/audio/DRM device
  kronos-keybed.c         ISA Super I/O + UART keybed emulator
scripts/                Build and run automation
  build-disk.sh           Downloads media, decrypts, patches, builds image
  run.sh                  Launches QEMU with correct parameters
docs/                   Technical reference (13 chapters)
vendor/qemu/            QEMU v10.2.2 (git submodule)
local/                  Device-specific files, build artifacts (.gitignored)
```

## Documentation

The full technical reference is available at **[metaneutrons.github.io/kronos](https://metaneutrons.github.io/kronos/)** and covers:

- System architecture (original hardware + emulation topology)
- USB composite device protocol (panel, audio, MIDI)
- NKS4 command format and video rendering pipeline
- Input event dispatch (touch, encoders, buttons, analog)
- Keybed UART serial protocol
- DRM/security analysis (Atmel, Blowfish, AES key derivation)
- ESP32-P4 implementation guide
- Complete patch specification with checksums for all known firmware versions

## Supported Firmware Versions

| Version | Kernel | Status |
|---------|--------|--------|
| 3.0.1 (2014) | Build #26 | ✅ Tested |
| 3.1.3 (2020) | Build #26 | ✅ Tested |
| 3.2.1 (2026) | Build #31 | ✅ Tested |
| 3.2.2 (2026) | Build #31 | ✅ Tested (default) |

## Disclaimer

This project is an independent research effort for educational and interoperability purposes. It does not distribute any Korg proprietary software. All Korg binaries are downloaded directly from Korg's public CDN by the end user. Korg® and Kronos® are registered trademarks of Korg Inc.

## License

- **Code**: [GNU General Public License v3.0](LICENSE)
- **Documentation**: [Creative Commons BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/)
