# Appendix {#sec:appendix}

---

## `NKS4Command` Type Reference {#sec:appendix-types}

The following table specifies the complete command and event type mapping for `NKS4Command` frames:

| Type Byte (`byte[3]`) | Operational Category | Targeted Host Dispatcher Routine |
| :---: | :--- | :--- |
| **`0x00`** | Capacitive Touch Events | `ReceiveEventBuffer` Touch Handler |
| **`0x01`** | Rotary, Button, and Response Events | `ReceiveEventBuffer` Button & Acknowledge Handler |
| **`0x02`** | Touch Scanning Controls | Internal Coprocessor Calibration |
| **`0x03`** | Analog Continuous Controllers | `ReceiveEventBuffer` Analog Dispatcher |
| **`0x07`** | S/PDIF Hardware Clock Monitoring | Updates Host clock status variables |
| **`0x61`** | S/PDIF Synchronization Init | `ProcessNextNKSEvent` Init Routine |
| **`0x62`** | S/PDIF Synchronization Stream | `ProcessNextNKSEvent` Stream Decoder |
| **`0x87`** | End-of-Transfer Terminator | Terminates the active event loop |
| **`0xC0`** | Set Active Display Viewport | Coprocessor Video Interface |
| **`0xC2`** | Start Pixel Bounding Box Transfer | Coprocessor Video Interface |
| **`0xC4`** | Solid-Color Hardware Fill | Coprocessor Video Interface |
| **`0xC5`** | Reserved Graphics Configuration | Coprocessor Video Interface |
| **`0xC6`** | Pixel Data Payload Block | Coprocessor Video Interface |
| **`0x83`** | Graphics Transfer End Marker | Triggers coprocessor screen refresh |
| **`0xE0`** | NV2AC Codec Register Write | `SubmitOmapNKS4CmdBulkWrite` (Plaintext Bulk OUT) |
| **`0xE1`** | NV2AC Codec Register Read | `SubmitOmapNKS4CmdBulkWrite` (Synchronous response required) |

---

## Analysis Methodology & Systems Verification Challenges {#sec:appendix-re}

This section details the static and dynamic analysis methodologies, hypervisor real-time virtualization constraints, memory-relocation self-checks, and legacy compiler toolchain behaviors that characterize the verification of the Korg Kronos platform.

### B.1 Static and Dynamic Binary Characterization Techniques {#sec:re-techniques}

Because the source code for the proprietary kernel drivers is unavailable, the system interfaces were characterized using binary analysis techniques:

1.  **Exported-Symbol Interface Characterization**: The dynamic boundaries of the proprietary modules were mapped by tracing the exported and imported ELF symbols. These symbols define the API boundaries of the real-time subsystems.
2.  **Relocation-Path Call-Graph Reconstruction**: disassembler tools were utilized to trace `R_386_PC32` relocations. This allowed the mapping of inter-module call-graphs (such as the communication path between `loadmod.ko` and `OmapNKS4Module.ko`).
3.  **BSS Segment Layout Mapping**: Unnamed global variables and structural list heads were mapped by scanning for `cmpl $ADDR, (ADDR)` comparison patterns and tracing offset access indices relative to relocated base registers.
4.  **Hardware-in-the-Loop Address Probing**: Variables mapped to specific physical memory locations were validated using memory inspection commands in the QEMU monitor (`xp` and `pmem` commands) and by routing debug prints to Host PIO diagnostic ports.

### B.2 Emulation-Based Co-Verification Constraints {#sec:re-pitfalls}

Implementing a stable, virtual software-in-the-loop (SIL) testing framework introduced several hypervisor-level constraints:

*   **APIC Timer Injection in Virtualized Environments**: When executing under KVM hardware acceleration (`-enable-kvm`), the KVM hypervisor's virtual APIC timer injection does not synchronize with RTAI's I-pipe layer. Consequently, hardware timer interrupts fail to trigger the Host scheduler during `schedule_timeout()` calls. This starves Host threads (specifically preventing the `ProcessMsgRoutine` daemon from executing) and causes system deadlocks. Running the Host under **full TCG instruction emulation** resolves the conflict, and adding the `lapic` kernel parameter enables stable real-time interrupt scheduling.
*   **Asynchronous Event Race Conditions**: The Host's synchronous driver requests utilize semaphores and state pointers stored in the BSS segment (`BSS+0x4b0`). If the coprocessor returns a response frame on the Interrupt IN endpoint before the Host thread finishes setting up its semaphore wait-state, the event is processed before a listener is registered, resulting in a dropped packet and a Host boot deadlock. The coprocessor must introduce a minimum delay of 1 millisecond (holding the response until the subsequent Interrupt IN poll) to guarantee synchronization.

### B.3 Relocatable Memory-Layout Integrity Verification {#sec:re-integrity}

`loadmod.ko` implements self-checking integrity checks that are highly sensitive to memory relocation:

*   **Relocated Address Self-Hashing Mismatch**: The driver computes the MD5 hash of its own relocated `.text` and `.data` segments in RAM. Because module load addresses determined by `sys_init_module` vary based on the loaded modules, relocatable references modify the binary image in memory. In custom systems or QEMU environments, this relocation-dependency triggers a self-test failure, corrupting the PRNG state used to decrypt system strings and halting system initialization. Resolving this mismatch requires applying binary patches to bypass the self-test routines (see [@sec:loadmod-patches]).
*   **Inter-Module Link Validation**: `loadmod.ko` registers its integrity state by writing to a location within `register_cdrom()`. This is Korg's proprietary mechanism for cross-module validation; `OA.ko` subsequently reads this address to verify `loadmod` is present. If the modules are loaded out of order, the BSS registration address is unmapped, causing the verification check to fail and sound output to be distorted (see [@sec:cripple-check]).

### B.4 Toolchain Integration and Legacy Compilation Anomalies {#sec:build-fixes-challenges}

Re-compiling custom Host kernels and RTAI helper modules to match Korg's original module vermagic (`2.6.32-korg`) introduces legacy compiler constraints:

*   **Compiler C89 Statement Ordering Quirks**: The legacy GCC 4.9 toolchain, operating under the `-std=gnu89` standard, silently discards statements declared before variable declarations inside out-of-tree RTAI module source files.
*   **Relocation Page Overwrites**: Dynamic binary patches written to addresses covered by ELF relocations are overwritten by the kernel module loader during relocation resolution. Binary patches must target regions that are not subject to dynamic symbol relocation.

---

## System References {#sec:appendix-references}

*   **Host Kernel**: Linux Kernel version 2.6.32.11 (with Korg real-time and MTRR patches).
*   **Real-Time Kernel Extension**: Real-Time Application Interface (RTAI) version 3.8.1 (Korg GPL release).
*   **System Binaries**: Proprietary Host modules (`OmapNKS4Module.ko`, `OmapVideoModule.ko`, `KorgUsbAudioDriver.ko`, `OA.ko`, `loadmod.ko`) from Korg Kronos system restore DVDs.
*   **Coprocessor Architecture Reference**: ESP32-P4 Technical Reference Manual, Espressif Systems.
*   **USB 2.0 Standard**: Universal Serial Bus Specification, Revision 2.0.
*   **MIDI Standard**: MIDI 1.0 Detailed Specification, MIDI Manufacturers Association (MMA).

---

## Recovery Media Integrity Verification {#sec:appendix-dvd-checksums}

All recovery media are available from Korg's public CDN. SHA-256 checksums are provided for integrity verification prior to image construction.

### KRONOS System Version 3.2.1 (January 2026, Kernel Build #31)

| File | Download URL | SHA-256 |
| :--- | :--- | :--- |
| DVD 1 (7.8 GB) | `https://storage.korg.com/kronos_dvd/KRONOS3/KronosDVD1_3_2_1.iso` | `b7550e50dd7b9b319864b283d5876f7ec5b895320f01fd89957806f0d69bb8bf` |
| DVD 2 (7.7 GB) | `https://storage.korg.com/kronos_dvd/KRONOS3/KronosDVD2_3_2_1.iso` | `716582f7f3cd2ebabc8e7c529c5b8806af03977b81b172da4f9a4336a3438a4e` |
| DVD 3 (6.7 GB) | `https://storage.korg.com/kronos_dvd/KRONOS3/KronosDVD3_3_2_1.iso` | `b03f87e92335f4938aaa7105e9a15ab233459b446a2c709d14758fdb0faacf58` |

### KRONOS System Version 3.2.2 (April 2026, Update Only)

| File | Download URL | SHA-256 |
| :--- | :--- | :--- |
| Update ZIP (419 MB) | `https://cdn.korg.com/us/support/download/files/59180c871025155934ae1d5cb7e237bc.zip` | `19d7b6bbb1ce3895377a576d2324d65c44a78aa0da556a9969af23d46afcf6fd` |

### KRONOS System Version 3.1.3 (September 2020, Kernel Build #26)

| File | Download URL | SHA-256 |
| :--- | :--- | :--- |
| DVD 1 (5.6 GB) | `https://storage.korg.com/kronos_dvd/KRONOS2/KronosDVD1_3_1_3.iso` | `48ca3c131cb45badd8972f58f98dd1c30ba7051c0b5423044a96ce476b70b7b5` |
| DVD 2 (5.2 GB) | `https://storage.korg.com/kronos_dvd/KRONOS2/KronosDVD2_3_1_3.iso` | `6b5277ff1be1429f86d1153fa0c38d8793c630f578f29c94c4903f6cb82ed6eb` |
| DVD 3 (2.7 GB) | `https://storage.korg.com/kronos_dvd/KRONOS2/KronosDVD3_3_1_3.iso` | `6cf977978b47f3d37ce93361263756f0d8e9da74c6a62adac10e4c9e33f3ad30` |

### KRONOS System Version 3.0.1 (November 2014, Kernel Build #26)

| File | SHA-256 |
| :--- | :--- |
| `KORG_KRONOS_1.ISO` (5.6 GB) | `cb1496873b9fc85781eae3921de5160805a5f2ffff179c157969f11d2675b4f3` |
| `KORG_KRONOS_2.ISO` (5.1 GB) | `2a1ff645cdb50fa9bc5104745f41a5d8358cedef592311ad494191219eb7bce7` |
| `KORG_KRONOS_3.ISO` (7.1 GB) | `85ec296bad12dbf0254d173517844905e0722ad79707f01aea39eb120b23be52` |

### KRONOS / KRONOS X (Original)

| File | Download URL |
| :--- | :--- |
| DVD 1 (5.2 GB) | `https://storage.korg.com/kronos_dvd/KRONOS_KRONOS-X/KORG_KRONOS_1D.iso` |
| DVD 2 (6.6 GB) | `https://storage.korg.com/kronos_dvd/KRONOS_KRONOS-X/KORG_KRONOS_2D.iso` |

---

## Binary Module Integrity Checksums {#sec:appendix-binary-checksums}

SHA-256 checksums of the unmodified (pre-patch) kernel modules extracted from decrypted filesystem images. These checksums identify the exact binary variant and determine the applicable patch specification.

### `loadmod.ko`

| System Version | File Size | SHA-256 |
| :--- | :---: | :--- |
| 3.0.1 | 46,622 B | `2eeb557d24fe30633c031911f4ce068fe4b2150ba45ade394411c2343198c23f` |
| 3.1.3 | 52,384 B | `bd1ec535b62d159eca74115d7fb9b8ede07627638e20a0af8548df2d74518182` |
| 3.2.1 | 52,384 B | `f77f835d93e4154257ceb9685d69ecad898adcd935b173cc5c4c34469fc854ee` |
| 3.2.2 | 52,384 B | `a228d26bf5a3435a05b001bc87ea294988de72e26cc2b7dc902a4042d2bbf46b` |

### `OA.ko`

| System Version | File Size | SHA-256 |
| :--- | :---: | :--- |
| 3.0.1 | 14,049,572 B | `1b302d9cba180541351accc39494dda69157b30fdc95df83298980af0380cd1c` |
| 3.1.3 | 14,049,572 B | `becb99c471e23ee9939939a86996730cb33dac49e818b0c88f14d801a485a984` |
| 3.2.1 | 14,049,572 B | `6f67a3de5b06ad238e3dc1c43031aa93d952e40e2422e2fed9a37364eb6b6ca9` |
| 3.2.2 | 14,049,572 B | `2b2e5a5cd76abb03243feaa34784d14a92794eb032be0b13988b5080c4338094` |

---

## Binary Patch Specification {#sec:appendix-patches}

> **Automated by:** [`scripts/build-disk.sh`](https://github.com/metaneutrons/kronos/blob/main/scripts/build-disk.sh)

The following patches are applied to `.init.text` sections of the respective modules. All patches are version-independent unless noted. The `kronos-keybed` QEMU device eliminates the need for the OA.ko keybed patch in virtualized environments.

### `loadmod.ko` — Anti-Tamper Bypass (4 Patches)

Required in QEMU and on any x86 host where vmalloc addresses differ from the original Kronos hardware.

| # | `.init.text` Offset | Original Bytes | Patched Bytes | Function Bypassed |
| :---: | :--- | :--- | :--- | :--- |
| 1 | `+0x3b` | `0f 85 a3 00 00 00` | `90 90 90 90 90 90` | Relocated `.text` MD5 self-check |
| 2 | `+0x48` | `0f 85 a2 00 00 00` | `90 90 90 90 90 90` | Relocated `.data` integrity hash |
| 3 | `+0x5b` | `0f 85 xx 00 00 00`¹ | `90 90 90 90 90 90` | Module memory space verification |
| 4 | `+0xbd` | `75 47` | `90 90` | DRM result verification |

¹ Jump displacement varies by version: `0x68` (3.0.1/3.1.3), `0x9b` (3.2.x). The NOP patch is applied regardless.

### `OA.ko` — Atmel Authorization Bypass (1 Patch)

Required when the 24-byte Atmel symmetric key is unavailable (i.e., no physical Atmel chip or key dump).

| `.init.text` Offset | Original Byte | Patched Byte | Function Bypassed |
| :--- | :--- | :--- | :--- |
| `+0x19f` | `74` (`je`) | `eb` (`jmp`) | `SetupAtmelForAuthorizations` return check |

This offset is **identical across all known versions** (3.0.1, 3.1.3, 3.2.1, 3.2.2).

### `OA.ko` — Keybed COM Port Bypass (1 Patch, D525 Only)

Required on physical hardware if the ESP32 keybed UART is not connected or not responding. **Not needed in QEMU** (the `kronos-keybed` device handles the handshake).

| System Version | `.init.text` Offset | Original Byte | Patched Byte | Function Bypassed |
| :--- | :--- | :--- | :--- | :--- |
| 3.0.1 / 3.1.3 | `+0x244` | `75` (`jne`) | `eb` (`jmp`) | `CSTGKeybedInterface_Startup` return check |
| 3.2.1 / 3.2.2 | `+0x2a2` | `75` (`jne`) | `eb` (`jmp`) | `CSTGKeybedInterface_Startup` return check |
