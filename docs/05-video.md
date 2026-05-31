# Display and Graphics Subsystem Specification {#sec:video}

The Korg Kronos graphical interface is managed by `OmapVideoModule.ko`, which exposes a virtual hardware-abstracted framebuffer interface to userspace. The Host's GUI shell `Eva` draws directly to this framebuffer and uses specialized system control calls (ioctls) to trigger real-time display updates.

## Framebuffer Layout Specification {#sec:framebuffer}

The Host registers a standard Linux framebuffer interface at `/dev/fb1` configured to the following specifications:

| Framebuffer Parameter | Specification Value | Technical Detail |
| :--- | :---: | :--- |
| **Active Resolution** | 800 × 600 pixels | Fixed display window |
| **Color Space / Depth** | 16 bpp (RGB565) | Little-endian pixel packing |
| **Total Memory Size** | 960,000 Bytes | $800 \times 600 \times 2$ bytes |
| **Memory Access Mode** | Memory-mapped (`mmap`) | Shared memory mapping with userspace |

---

## Framebuffer Controller Interface (`OMAPFB` Ioctls) {#sec:omapfb-ioctls}

All rendering requests and display configurations are directed to the virtual framebuffer driver `/dev/fb1` using a series of proprietary ioctl commands:

| Ioctl Code | Macro Definition | Size (Bytes) | NKS4 Driver API | Functional Description |
| :---: | :--- | :---: | :--- | :--- |
| `0x40047201` | `_IOW('r', 1, 4)` | 4 | `IncProgressBar` | Increments the boot progress indicator |
| `0x80047202` | `_IOR('r', 2, 4)` | 4 | `GetProgressBarPercent` | Queries the current progress percentage |
| `0x40047203` | `_IOW('r', 3, 4)` | 4 | `AddToProgressBar` | Appends a step value to the progress indicator |
| `0x40047204` | `_IOW('r', 4, 4)` | 4 | `UpdateColorPal` | Modifies an entry in the display color palette |
| `0x40087205` | `_IOW('r', 5, 8)` | 8 | `InitLCDRegs` | Initializes the coprocessor's LCD controller |
| `0x40047206` | `_IOW('r', 6, 4)` | 4 | `XAxisByteSize` | Calculates row stride length in bytes |
| **`0x400c7207`** | `_IOW('r', 7, 12)` | 12 | `SendPixelDataRegion` | Transmits a modified pixel region over USB |
| **`0x40107208`** | `_IOW('r', 8, 16)` | 16 | `SendFillData` | Executes a solid-color hardware rectangle fill |
| `0x40047209` | `_IOW('r', 9, 4)` | 4 | N/A | Sets system display configuration flags |
| `0x4004720B` | `_IOW('r', 11, 4)` | 4 | `SetProgressBarPercent` | Sets the progress indicator to a specific value |
| `0x4004720E` | `_IOW('r', 14, 4)` | 4 | `GetTitleScreenVersion` | Queries the active graphics resource version |

---

## Multi-Phase Pixel Transfer Protocol {#sec:pixel-transfer}

When userspace triggers a display region update via ioctl `0x400c7207`, the driver initiates a multi-phase USB Bulk OUT transaction sequence to synchronize the coprocessor's display memory:

![Pixel Sync Sequence](img/pixel-transfer.png){#fig:pixel-transfer}

```{.mermaid caption="Three-Phase Display Synchronization Protocol" #fig:pixel-transfer-src}
sequenceDiagram
    participant Eva as Eva (Userspace)
    participant Vid as OmapVideoModule
    participant NKS4 as NKS4 Driver
    participant USB as USB Bulk OUT
    participant ESP as ESP32 Display

    Eva->>Vid: ioctl(0x400c7207, {x, y, width})
    Vid->>NKS4: OmapNKS4SendPixelDataRegion(x, y, width)
    NKS4->>NKS4: Queue 0xC2 event in ring buffer

    Note over NKS4: Video Message Processor thread

    NKS4->>USB: 0xC2 header (12 bytes) — region definition
    loop For each row
        NKS4->>USB: 0xC6 data (512 bytes) — pixel bytes
    end
    NKS4->>USB: 0x83 end marker (4 bytes)

    USB->>ESP: Receive + byte-swap + render
```

### Phase 1: Region Definition Header (`0xC2`)

The transaction sequence begins with a 12-byte header sent via `SubmitOmapNKS4VideoWrite` to define the target bounding box. Every 32-bit word in this packet must be byte-swapped per the rule defined in [@sec:byte-swap]:

```
Byte  0: (start_offset >> 16) & 0x07   — Start address offset [bits 18:16]
Byte  1: (start_offset >> 8) & 0xFF    — Start address offset [bits 15:8]
Byte  2: start_offset & 0xFF           — Start address offset [bits 7:0]
Byte  3: 0xC2                          — Command type code
Byte  4: (row_bytes >> 16) & 0x07      — Width in bytes [bits 18:16]
Byte  5: (row_bytes >> 8) & 0xFF       — Width in bytes [bits 15:8]
Byte  6: row_bytes & 0xFF              — Width in bytes [bits 7:0]
Byte  7: (num_rows >> 16) & 0x07       — Height in rows [bits 18:16]
Byte  8: (num_rows >> 8) & 0xFF        — Height in rows [bits 15:8]
Byte  9: num_rows & 0xFF               — Height in rows [bits 7:0]
Byte 10: (stride >> 16) & 0x07         — Framebuffer stride [bits 18:16]
Byte 11: stride & 0xFF                 — Framebuffer stride [bits 7:0]
```

### Phase 2: Pixel Data Stream (`0xC6`)

Following the header, the raw RGB565 pixel payloads are divided and sent in 512-byte USB Bulk OUT packets. Every packet is preceded by a single byte command prefix and must be byte-swapped per 32-bit word:

```
Byte   0: 0xC6                         — Pixel data command prefix
Bytes 1-511: raw RGB565 pixel data     — Up to 255 pixels (510 bytes) per packet
```

*   The driver reads pixel data linearly starting at `start_offset`.
*   After reading `row_bytes` from a row, the driver skips to the next row by adding `stride` to the source address pointer.
*   Packets are sent until the entire row-column bounding box is transferred.

### Phase 3: Synchronization End Marker (`0x83`)

To signal that the transfer is complete and trigger the coprocessor's screen refresh, the Host sends a 4-byte end marker:
```
Bytes 0-2: 0x00
Byte  3:   0x83
```

---

## Solid-Color Hardware Rectangle Fill Optimization (`0xC4`) {#sec:fill-rect}

To optimize UI operations like background clearing and window borders, the driver bypasses pixel streaming, instead sending a 12-byte fill command to trigger a solid-color fill on the coprocessor. This packet must be byte-swapped per 32-bit word:

```
Byte  0: (Y >> 16) & 0x07
Byte  1: (Y >> 8) & 0xFF
Byte  2: Y & 0xFF
Byte  3: 0xC4                          — Command type code
Byte  4: (height >> 16) & 0x07
Byte  5: (height >> 8) & 0xFF
Byte  6: height & 0xFF
Byte  7: color                         — Signed byte palette index (0x00 to 0x3F)
Byte  8: (width >> 16) & 0x07
Byte  9: (width >> 8) & 0xFF
Byte 10: width & 0xFF
Byte 11: 0x00                          — Reserved padding
```

---

## Active Viewport Boundary Selection (`0xC0`) {#sec:set-region}

To limit drawing operations to a specific sub-region, the driver can send an 8-byte command packet specifying the active viewport coordinates. This packet must be byte-swapped:

```
Byte 0: X_start low
Byte 1: Parameter 1
Byte 2: Parameter 2
Byte 3: 0xC0                           — Command type code
Byte 4: Dimension byte 3
Byte 5: Dimension byte 2 (masked with 0x07)
Byte 6: Dimension byte 1
Byte 7: Dimension byte 0
```

---

## Video Driver API Specifications {#sec:video-api}

`OmapVideoModule.ko` exports several kernel-space interfaces to process and route drawing operations from Userspace:

| Driver API Function | C Function Signature | Subsystem Action |
| :--- | :--- | :--- |
| `SendPixelDataRegion` | `void(int x, int y, int width)` | Queues a pixel region update for the bounding box |
| `SendFillData` | `void(char color, int x, int y, int width)` | Queues a solid-color rectangle fill |
| `InitLCDRegs` | `void(char param1, char param2, int config)` | Directs the initial register setup for the LCD panel |
| `UpdateColorPal` | `void(char idx, char r, char g, char b)` | Modifies a palette index mapping on the coprocessor |
| `UpdateScreenInfo` | `void(char* base_addr, int w, int h)` | Maps the active framebuffer memory address |
| `XAxisByteSize` | `int(int width)` | Computes the physical byte size of a row ($width \times 2$) |

---

## Video Command Ring-Buffer Architecture {#sec:video-ring-buffer}

To prevent blocking userspace rendering during slower USB transmissions, `OmapVideoModule.ko` implements an internal asynchronous FIFO ring buffer. 

### Ring-Buffer Entry Schema (32-Byte Structure)

```
┌──────┬──────────────┬──────────────┬──────────────┬──────────────┬──────────────────┐
│Byte 0│ Bytes 1 - 4  │ Bytes 5 - 8  │ Bytes 9 - 12 │Bytes 13 - 16 │  Bytes 17 - 31   │
├──────┼──────────────┼──────────────┼──────────────┼──────────────┼──────────────────┤
│ cmd  │ color/param  │     arg2     │     arg3     │     arg4     │     reserved     │
└──────┴──────────────┴──────────────┴──────────────┴──────────────┴──────────────────┘
```

### Queue Layout and Memory Topology

The ring-buffer queue is managed inside `COmapNKS4VideoAPI::sInstance` (located at offset `+0x920` relative to the driver's relocated base, spanning `0x31C4` bytes):

| Struct Offset | Data Type | Field Identifier | Architectural Role |
| :---: | :--- | :--- | :--- |
| `+0x000` | Struct [384] | `sRingBufferEntries` | Array of 384 active 32-byte command entries |
| `+0x3180` | `uint32_t` | `sRingBufferMask` | Bitmask for index wrapping (fixed at `0x17F` / 383) |
| `+0x3184` | `uint32_t` | `sRingBufferWriteIdx` | Head pointer for incoming ioctl commands |
| `+0x3188` | `uint32_t` | `sRingBufferReadIdx` | Tail pointer for the USB transmission thread |
| `+0x31B4` | `uint32_t` | `sFrameBufferWidth` | Framebuffer width (fixed at 800) |
| `+0x31B8` | `uint32_t` | `sFrameBufferStride` | Physical row stride length in bytes |
| `+0x31BC` | `uint32_t*` | `sFrameBufferBase` | Pointer to the base of the virtual framebuffer memory |
| `+0x31C0` | `uint32_t` | `sUsbPacketSize` | Maximum physical USB packet payload (fixed at 512) |
| `+0x31C4` | `uint32_t` | `sEventsToProcess` | Atomic counter tracking pending transfers |
