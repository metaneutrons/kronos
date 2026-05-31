# Kernel Module Specifications and Inter-Module API Contract {#sec:modules}

To construct the real-time audio and control environment, the Host kernel loads 14 dynamic kernel modules. These modules establish hardware abstractions, initialize the real-time RTAI scheduler, and expose control APIs to userspace applications.

## Host Kernel Module Load Hierarchy {#sec:load-order-korg}

The kernel modules must be loaded in a strict sequential order to ensure proper symbol resolution and address-space alignment:

```
┌────────────────────────────────────────────────────────┐
│  RTAI Scheduler Core (rtai_hal, rtai_sched)            │
└───────────────────┬────────────────────────────────────┘
                    ▼
┌────────────────────────────────────────────────────────┐
│  RTAI Synchronization & Debug (sem, fifos, ndbg)       │
└───────────────────┬────────────────────────────────────┘
                    ▼
┌────────────────────────────────────────────────────────┐
│  Hardware Wrapper Interface (STGEnabler)               │
└───────────────────┬────────────────────────────────────┘
                    ▼
┌────────────────────────────────────────────────────────┐
│  Coprocessor USB Driver (OmapNKS4Module)               │
└───────────────────┬────────────────────────────────────┘
                    ▼
┌────────────────────────────────────────────────────────┐
│  Display Framebuffer Driver (OmapVideoModule)          │
└───────────────────┬────────────────────────────────────┘
                    ▼
┌────────────────────────────────────────────────────────┐
│  Isochronous USB Audio & MIDI (KorgUsbAudioDriver)    │
└───────────────────┬────────────────────────────────────┘
                    ▼
┌────────────────────────────────────────────────────────┐
│  Bignum Cryptographic Lib & MIDI Access (STGGmp, Midi)│
└───────────────────┬────────────────────────────────────┘
                    ▼
┌────────────────────────────────────────────────────────┐
│  DRM Integrity Verifier & Decrypter (loadmod)          │
└───────────────────┬────────────────────────────────────┘
                    ▼
┌────────────────────────────────────────────────────────┐
│  Real-Time Synthesis Engine Core (OA.ko)               │
└────────────────────────────────────────────────────────┘
```

The exact module configuration parameters and loading flags are specified as follows:

1.  **`rtai_hal.ko`**: Configures the base RTAI hardware abstraction layer. Accepts frequency parameters: `rtai_cpufreq_arg=2496000000` (Host CPU clock) and `rtai_apicfreq_arg=1000000000` (APIC timer frequency).
2.  **`rtai_sched.ko`**: Initializes the real-time scheduler.
3.  **`rtai_sem.ko`**: Registers real-time semaphore synchronization primitives.
4.  **`rtai_fifos.ko`**: Allocates real-time inter-process FIFO communication pathways.
5.  **`rtai_ndbg.ko`**: Registers real-time debug traps and exports `debug_traps`.
6.  **`STGEnabler.ko`**: Exports the standard `stg_usb_*` wrapper APIs.
7.  **`OmapNKS4Module.ko`**: Mounts the USB driver and binds to the coprocessor's panel control interface.
8.  **`OmapVideoModule.ko`**: Registers `/dev/fb1` and implements the display rendering thread.
9.  **`KorgUsbAudioDriver.ko`**: Initializes the high-bandwidth isochronous audio stream engines.
10. **`STGGmp.ko`**: Exports GMP arbitrary-precision bignum math functions (`__gmpz_*`) for RSA operations.
11. **`USBMidiAccessory.ko`**: Exposes MIDI ports for external accessories.
12. **`loadmod.ko`**: Verifies system file integrity, performs the coprocessor challenge-response check, and mounts the encrypted filesystems.
13. **`set_memsize.ko`**: Helper module (required under virtualized architectures) to initialize the physical memory bounds variable (`orig_mem_size`).
14. **`OA.ko`**: The primary real-time synthesis engine and DSP core.

---

## Inter-Module Cryptographic Verification Sequence {#sec:drm-sequence}

During the system startup phase, `loadmod.ko` and `OmapNKS4Module.ko` execute a hardware-level verification sequence over the USB control path to ensure system integrity:

```
Host (loadmod.ko)                              Coprocessor (Atmel)
      │                                                │
      ├─── [1] NV2AC Read (0xB6, ch=0x19, len=7) ─────►│ (Requests Public ID)
      ◄─── [2] Returns 7-Byte Public ID ───────────────┤
      │                                                │
      ├─── [3] NV2AC Write (0xB8, challenge data) ────►│ (Transmits challenge)
      │                                                │
      ├─── [4] NV2AC Read (0xB6, ch=0x50, len=1) ─────►│ (Zone status check)
      ◄─── [5] Returns Verification Status ────────────┤
      │                                                │
      ├─── [6] NV2AC Write (0xB4, command data) ──────►│ (Configures read zone)
      │                                                │
      └─── [7] NV2AC Read (0xB6, ch=0x50, len=8) ─────►│ (Requests signed zone data)
```

The subsequent steps (8 through 10) execute challenge-response operations and read volume registers to derive the 24-byte Blowfish key required to decrypt the `.pairFact3` file (see [@sec:pairfact-final]).

---

## Driver Exported Symbol API Specifications {#sec:exported-symbols}

The following tables specify the exact symbol interface contracts exported by each driver module. These symbols define the API boundaries that the coprocessor must satisfy:

### `OmapNKS4Module.ko` Exported Symbols {#sec:nks4-exports}

| Exported Symbol Name | Consumer Module | Operational Purpose |
| :--- | :--- | :--- |
| **`OmapNKS4SendFillData`** | `OmapVideoModule` | Executes a solid-color hardware rectangle fill on the display |
| **`OmapNKS4SendPixelDataRegion`** | `OmapVideoModule` | Streams a raw pixel bounding box to the display memory |
| **`OmapNKS4UpdateScreenInfo`** | `OmapVideoModule` | Configures the virtual framebuffer memory address and dimensions |
| **`OmapNKS4UpdateColorPal`** | `OmapVideoModule` | Updates a palette index color mapping |
| **`OmapNKS4InitLCDRegs`** | `OmapVideoModule` | Initializes the coprocessor's display controller registers |
| **`OmapNKS4XAxisByteSize`** | `OmapVideoModule` | Calculates row stride length in bytes ($width \times 2$) |
| **`OmapNKS4OutputFifo_WriteCommand`** | `OA.ko` | Writes a 4-byte command to the Output FIFO |
| **`OmapNKS4InputFifo_ReadCommand`** | `OA.ko` | Reads a pending event from the Input FIFO |
| **`OmapNKS4Fifos_TriggerOutputInterrupt`** | `OA.ko` | Signals the background USB writer thread to flush the Output FIFO |
| **`stgNV2AC_sync_cmd`** | `OA.ko` | Sends a write command (`0xE0`) to the NV2AC codec |
| **`stgNV2AC_sync_read_cmd`** | `OA.ko` | Initiates a blocking read (`0xE1`) command to the NV2AC codec |
| **`COmapNKS4Driver_StartScanning`** | `OA.ko` | Enables panel and key matrix scanning on the coprocessor |
| **`COmapNKS4Driver_Is88Key`** | `OA.ko` | Queries the coprocessor to identify the keyboard model |
| **`COmapNKS4Driver_GetHardwareVersion`** | `OA.ko` | Queries the coprocessor hardware version |
| **`COmapNKS4Driver_GetSPDIFClockError`** | `OA.ko` | Queries the S/PDIF clock synchronization status |
| **`COmapNKS4_SetProgressBarPercent`** | `OA.ko` / `Eva` | Sets the boot progress indicator to a specific percentage |
| **`COmapNKS4_AddToProgressBar`** | `OA.ko` / `Eva` | Appends a step value to the progress indicator |
| **`COmapNKS4_IncProgressBar`** | `OA.ko` / `Eva` | Increments the progress indicator |
| **`SetupNKS4Calibration`** | `OA.ko` | Enters capacitive touch panel calibration mode |
| **`CleanupNKS4Calibration`** | `OA.ko` | Exits touch panel calibration mode |

### `OmapVideoModule.ko` Exported Symbols {#sec:video-exports}

| Exported Symbol Name | Consumer Module | Operational Purpose |
| :--- | :---: | :--- |
| **`GetScreenXDimension`** | `OA.ko` / `Eva` | Returns the active display width (fixed at `800`) |
| **`GetScreenYDimension`** | `OA.ko` / `Eva` | Returns the active display height (fixed at `600`) |

### `KorgUsbAudioDriver.ko` Exported Symbols {#sec:audio-exports}

| Exported Symbol Name | Consumer Module | Operational Purpose |
| :--- | :---: | :--- |
| **`KorgUsbAudioInitialize`** | `OA.ko` | Allocates audio DMA ring buffers and registers USB audio drivers |
| **`KorgUsbAudioStart`** | `OA.ko` | Activates high-bandwidth isochronous streaming on the endpoints |
| **`KorgUsbAudioOutput`** | `OA.ko` | Returns a pointer to the active playback DMA buffer |
| **`KorgUsbAudioOutputDone`** | `OA.ko` | Submits a completed playback buffer to the isochronous OUT queue |
| **`KorgUsbAudioInput`** | `OA.ko` | Returns a pointer to the active capture DMA buffer |
| **`KorgUsbAudioInputDone`** | `OA.ko` | Signals that a captured audio block has been processed |
| **`KorgUsbAudioDone`** | `OA.ko` | Deallocates real-time resources and stops the USB audio streams |
| **`KorgUsbAudioInitialized`** | `OA.ko` | Queries the current initialization status of the audio subsystem |
