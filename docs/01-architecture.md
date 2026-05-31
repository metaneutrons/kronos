# System Architecture {#sec:architecture}

## Hardware Architecture and Inter-Processor Boundaries {#sec:original-hardware}

The Korg Kronos workstation is structured as a heterogeneous dual-processor computing environment, partitioning tasks between general-purpose control and hard real-time signal generation:

*   **Host CPU (Intel Atom Z530)**: Operating at 1.6 GHz on an x86 32-bit architecture, the Host is responsible for running the operating system, orchestrating the GUI framework, and executing the core digital signal processing (DSP) synthesis engine.
*   **Coprocessor Subsystem (NKS4)**: Based on an ARM-core TI OMAP processor, this subsystem manages high-resolution display rendering, capacitive touch sensing, panel switches, rotary encoders, and high-fidelity audio codec data formatting.

![Hardware Architecture Block Diagram](img/architecture-original.png){#fig:arch-original}

```{.mermaid caption="Host-Coprocessor System Interconnect Schema" #fig:arch-original-src}
graph TB
    subgraph "Intel Atom (Host CPU)"
        KERNEL[Linux 2.6.32 + RTAI]
        OA[OA.ko - Synth Engine]
        EVA[Eva - GUI Application]
        NKS4MOD[OmapNKS4Module.ko]
        VIDMOD[OmapVideoModule.ko]
        AUDIODRV[KorgUsbAudioDriver.ko]
    end

    subgraph "NKS4 Board (OMAP + Peripherals)"
        OMAP[TI OMAP CPU]
        ATMEL[Atmel CryptoMemory]
        NV2AC[NV2AC Audio Codec]
        LCD[800×600 LCD]
        TOUCH[Touch Panel]
        PSOC[PSoC - Key Scanner]
        KEYS[88-Key Keyboard]
        ENCODERS[Encoders + Buttons]
        DAC[DAC → Analog Out]
        ADC[ADC ← Analog In]
    end

    KERNEL --> OA
    KERNEL --> EVA
    OA --> NKS4MOD
    OA --> AUDIODRV
    EVA --> VIDMOD
    VIDMOD --> NKS4MOD

    NKS4MOD <-->|"USB 2.0 Interface"| OMAP
    AUDIODRV <-->|"USB Isochronous Audio"| OMAP
    OA <-->|"16550 UART Interconnect"| PSOC

    OMAP --> LCD
    OMAP --> ATMEL
    OMAP --> NV2AC
    OMAP --> TOUCH
    OMAP --> ENCODERS
    PSOC --> KEYS
    NV2AC --> DAC
    ADC --> NV2AC
```

### Keybed Interconnect Specification

A critical design characteristic of the Kronos hardware architecture is the separation of the keyboard interface from the primary USB control path. The key matrix scanning is managed by a dedicated PSoC microcontroller on the NKS4 board. Communication between the Host CPU's synthesis engine (`CSTGKeybedInterface` within `OA.ko`) and the PSoC keybed scanner occurs via a dedicated, physical 16550 UART serial link mapping directly to x86 I/O port addresses. This secondary, non-USB data path bypasses the USB polling latency and CPU scheduling jitter, ensuring sub-millisecond real-time keyboard event responsiveness.

## Host Operating System and Runtime Environment {#sec:operating-system}

### Kernel and Core Operating System Specifications {#sec:os-kernel}

The Host environment executes a specialized, stripped-down Linux distribution tuned for low-latency embedded operations:

| Subsystem Component | Specification | Operational Role |
| :--- | :--- | :--- |
| **Kernel** | Linux 2.6.32 (32-bit x86 architecture) | Base operating system and hardware abstraction |
| **Real-time Extension** | RTAI 3.x (Real-Time Application Interface) | Orchestrates hard real-time kernel-space DSP tasks |
| **Root Filesystem** | ext3 on CompactFlash (Partition 2) | Persistent system storage |
| **Init System** | BusyBox init (`/etc/rcS` execution script) | Directs initial hardware and driver loading sequence |
| **Shell Environment** | BusyBox ash | System command execution shell |
| **C Standard Library** | uClibc | Compact embedded standard library footprint |

### Storage Subsystem Partition Layout {#sec:os-filesystem}

```
/
├── korg/
│   ├── Mod/
│   │   ├── OA.ko                  (Core synthesis engine)
│   │   ├── OmapVideoModule.ko     (Coprocessor framebuffer driver)
│   │   └── KorgUsbAudioDriver.ko  (Real-time USB audio/MIDI driver)
│   ├── Eva                        (GUI shell application)
│   └── ...                        (Resource files: presets, samples, configs)
├── sbin/
│   └── OmapNKS4Module.ko         (Coprocessor USB control driver)
├── lib/modules/2.6.32/
│   └── rtai_*.ko                  (RTAI real-time infrastructure modules)
└── etc/
    └── rcS                        (System initialization script)
```

### System Boot and Drivers Loading Sequence {#sec:os-init}

Upon Host power-on, the `/etc/rcS` init script executes a deterministic driver initialization chain:

1.  **System Mounting**: Essential filesystems are mounted, and the loopback network interfaces are established.
2.  **Real-Time Bootstrapping**: RTAI real-time modules (`rtai_hal.ko`, `rtai_sched.ko`, `rtai_sem.ko`, `rtai_fifos.ko`) are loaded into the kernel space, configuring the secondary real-time hardware scheduler.
3.  **USB Control Path Probing**: `OmapNKS4Module.ko` is loaded, registering the USB driver and initiating the NKS4 control protocol handshake.
4.  **Framebuffer Registration**: `OmapVideoModule.ko` is initialized, registering the virtual framebuffer interface at `/dev/fb1`.
5.  **Application Launch**: The GUI shell `Eva` is started in userspace, which internally mounts and loads the synthesis engine `OA.ko` and the high-performance isochronous audio stream manager `KorgUsbAudioDriver.ko`.

## Multi-Layered Defense-in-Depth and Anti-Analysis Mechanics {#sec:os-security}

The Kronos incorporates several overlapping protective boundaries to defend system integrity, prevent software reverse-engineering, and enforce license constraints:

| Security Layer | Technical Implementation | Security Objective |
| :--- | :--- | :--- |
| **Symbol Stripping** | Total symbol removal in `Eva`; only essential API symbols exported in `OA.ko` | Obstructs static and dynamic reverse-engineering of user-space logic |
| **Identifier Obfuscation** | Obfuscated name mapping for internal functions (e.g., `bzzzzzzzzzzzt12`) | Increases complexity of static call-graph reconstruction |
| **Cryptographic Licensing** | Atmel AT88SC0204CA hardware auth validation via `SetupAtmelForAuthorizations` | Restricts system operation to licensed hardware |
| **Kernel-Space Execution** | The primary DSP engine (`OA.ko`) operates in Ring 0 (kernel-space RTAI) | Bypasses traditional user-space analysis vectors (e.g., `ptrace`, `strace`, `LD_PRELOAD`) |
| **Console Locking** | Unpopulated UART pads; production builds disable SSH and local TTY login shells | Prevents interactive system compromise |
| **Proprietary Interconnect** | Vendor-specific USB subclass matching (`bInterfaceSubClass=0xFF`) | Restricts standard host operating system driver auto-matching |
| **Relocation Integrity Monitoring** | `loadmod.ko` hashes and verifies the memory layout of relocated binary modules | Detects runtime software modifications |

Despite these extensive software-level integrity verification checks, the USB wire control protocol relies on unencrypted transmission. Therefore, the command packet sequences flowing across the physical USB bus are directly readable, establishing a reliable interface control specification.
