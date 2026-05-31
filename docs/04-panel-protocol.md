# NKS4 Panel Protocol Specification {#sec:panel-protocol}

All control-path communication between the Host and the NKS4 coprocessor is orchestrated using a proprietary, message-oriented framing protocol.

## Protocol Command Payload Frame (`NKS4Command`) {#sec:command-format}

Every protocol frame consists of a rigid 4-byte message structure designated as `NKS4Command`:

```
┌───────────────┬───────────────┬───────────────┬───────────────┐
│    Byte 0     │    Byte 1     │    Byte 2     │    Byte 3     │
├───────────────┼───────────────┼───────────────┼───────────────┤
│    data_lo    │    data_hi    │   sub_type    │     type      │
└───────────────┴───────────────┴───────────────┴───────────────┘
```

*   **`data_lo` (Byte 0)**: Low payload byte or device control value.
*   **`data_hi` (Byte 1)**: High payload byte or secondary configuration parameter.
*   **`sub_type` (Byte 2)**: Command or event identifier code.
*   **`type` (Byte 3)**: Message category classifier.

A special **end-of-transfer marker** `[0x00, 0x00, 0x00, 0x87]` terminates every logical event list returned via the Interrupt IN endpoint. Multiple commands can be packed into a single transfer block ahead of the terminator.

---

## Interrupt IN: Coprocessor-to-Host Event Polling {#sec:interrupt-in}

The Host polls the Interrupt IN endpoint (`EP1`) every 1 millisecond. The coprocessor returns a packet containing zero or more 4-byte `NKS4Command` frames followed by the 4-byte end-of-transfer marker.

### Idle Polling Frame

When the coprocessor reports no pending user events or command acknowledgments, it returns only the terminator frame:
```
[0x00, 0x00, 0x00, 0x87]
```

### Event and Response Transmission

When a panel event (e.g., button press) or a command response is pending, the coprocessor packs the payload ahead of the terminator. A single pending event results in an 8-byte transfer:
```
[data_lo, data_hi, sub_type, type]  ← Event or Response payload (4 bytes)
[0x00,    0x00,    0x00,     0x87]  ← End-of-transfer marker (4 bytes)
```

### Asynchronous Signaling and Timing Constraints {#sec:re-response-timing}

The Host panel driver enforces a strict asynchronous command-response pipeline. When a command requiring an acknowledgment is sent via Bulk OUT, the Host kernel thread registers a waiting reference via `prepare_to_wait()` and suspends itself via `schedule_timeout()`. 

To prevent race conditions, the coprocessor **must not** return the response in the immediate USB microframe containing the command completion. If the response is returned on the Interrupt IN endpoint before the Host thread finishes setting up its semaphore wait-state, the event is processed before a listener is registered, resulting in a dropped packet and a Host boot deadlock. The coprocessor must introduce a minimum delay of 1 millisecond (holding the response until the subsequent Interrupt IN poll) to guarantee synchronization.

---

## Bulk OUT: Host-to-Coprocessor Command Routing {#sec:bulk-out}

The Bulk OUT endpoint (`EP2`) multiplexes control commands and display data. To manage these paths, the Host driver allocates two independent URB memory pools:

| URB Pool Pointer | Associated BSS Symbol | Driver Allocator Function | Target Payload Type |
| :--- | :--- | :--- | :--- |
| **`Video URB Pool`** | `sBulkFreeVideoURBList` | `SubmitOmapNKS4CmdBulkWrite` | 4-byte panel control & codec commands |
| **`Command URB Pool`** | `sBulkFreeCommandURBList` | `SubmitOmapNKS4BulkWrite` | Variable-length pixel data blocks |

---

## Initial System Configuration Handshake {#sec:configure-sequence}

Upon USB interface binding, the Host driver's probe sequence executes an 11-step configuration handshake to initialize the panel and verify the coprocessor:

| Step | Command Function | Sent Payload | Target Sub-type | Expected Response Word | Expected Response Payload |
| :---: | :--- | :--- | :---: | :---: | :--- |
| **1** | `CommunicationCheck` | `[0x00, 0x00, 0xEE, 0x00]` | `0xEE` | `0x0066` | `[0x00, 0x00, 0x66, 0x00]` |
| **2** | `GetVersion` | `[0x00, 0x00, 0xF0, 0x00]` | `0xF0` | `0x0070` | `[fw_ver, hw_ver, 0x70, 0x00]` |
| **3** | `ReadPortConfiguration` | `[0x00, 0x00, 0xF1, 0x01]` | `0xF1` | `0x0171` | `[hw_val, model_id, 0x71, 0x01]` |
| **4** | `SetNumberOfAnalogInputs` | `[0x00, 0x3F, 0x90, 0x01]` | `0x90` | None | Fire-and-forget command |
| **5** | `SetAllAnalogInputFilters` | `[data_lo, data_hi, 0xA0, 0x01]` | `0xA0` | None | Fire-and-forget command |
| **6** | `SetNumberOfLEDs` | `[data_lo, data_hi, 0x00, 0x00]` | `0x00` | None | Fire-and-forget command |
| **7** | `ConfigureRotaryEncoders` | `[0x28, 0x00, 0xB0, 0x01]` | `0xB0` | None | Fire-and-forget command |
| **8** | `SetRotaryEncoderSampleSpeed` | `[0x00, 0x64, 0x80, 0x00]` | `0x80` | None | Fire-and-forget command |
| **9** | `ConfigureScanning` | `[0x00, 0x4E, 0x00, 0x00]` | `0x00` | None | Fire-and-forget command |
| **10** | `SetProgressBarPercent` | `[0x0F, 0x00, 0xC0, 0x00]` | `0xC0` | None | Video command interface |
| **11** | `CompleteProbe` | N/A | N/A | None | Probe success signal |

### Verified Handshake Byte Streams

The exact binary transaction sequence for steps 1 through 3 is specified as follows:

```
// Step 1: Communication Verification
Host   → Device: [0x00, 0x00, 0xEE, 0x00]
Device → Host:   [0x00, 0x00, 0x66, 0x00] [0x00, 0x00, 0x00, 0x87]

// Step 2: Firmware and Hardware Version Query
Host   → Device: [0x00, 0x00, 0xF0, 0x00]
Device → Host:   [0x21, 0x21, 0x70, 0x00] [0x00, 0x00, 0x00, 0x87] // reports FW 2.1, HW 2.1

// Step 3: Coprocessor Port Capability Identification
Host   → Device: [0x00, 0x00, 0xF1, 0x01]
Device → Host:   [0x04, 0x00, 0x71, 0x01] [0x00, 0x00, 0x00, 0x87] // reports HW version 4
```

### Response Verification Assembly Logic {#sec:response-verification}

The Host driver verifies incoming Interrupt IN payloads by comparing the 16-bit word at offset `+2` of the local receive buffer. 

The Host's verification logic is defined as:
```c
// CommunicationCheck Assembly Verification (.text+0x26dd):
cmpw $0x66, 0x2e(%esp)    // Compares local stack buffer offset +46 (offset +2 of payload) to 0x0066

// GetVersion Assembly Verification (.text+0x2745):
cmpw $0x70, 0x1e(%esp)    // Compares local stack buffer offset +30 to 0x0070

// ReadPortConfiguration Assembly Verification (.text+0x27d5):
cmpw $0x171, 0x3e(%esp)   // Compares local stack buffer offset +62 to 0x0171
```

The response comparison word is derived by packing the command response bytes into a 32-bit little-endian integer. Because only three commands expect responses, the coprocessor **must not** return response frames for any other commands. Spurious Interrupt IN responses corrupt the Host's internal state machine, causing subsequent legitimate responses to be delivered to the wrong waiting thread.

---

## Host Event Dispatch Pipeline (`ReceiveEventBuffer`) {#sec:events-dispatch}

When an Interrupt IN packet is received, the Host's `InterruptCallback` routine parses the 4-byte frames. The dispatcher branches based on the value of the `type` byte (`byte[3]`):

| Type (`byte[3]`) | Category | Operational Routing |
| :---: | :--- | :--- |
| **`0x87`** | End-of-Transfer | Terminates transaction parsing |
| **`0x00`** | Button & Touch Events | Dispatched by sub-type (`byte[2]`) to touch or button handlers |
| **`0x01`** | Rotary & System Events | Dispatched to knob handlers or `SendNKS4EventToLinuxReader` |
| **`0x03`** | Analog Inputs | Routed directly to `SendNKS4EventToSTG` (sliders, knobs, pedals) |
| **`0x07`** | Clock Sync Status | Updates Host BSS variable `SPDIFClockError` |
| **`0xE1`** | NV2AC/Atmel Reads | Decoded as multi-byte response, triggers `SignalAtmelReadComplete` |

---

## Host-Coprocessor Signaling Interface {#sec:send-event}

The bridge between the asynchronous low-level USB interrupt callback and the blocking, synchronous system commands is managed by the `SendNKS4EventToLinuxReader` and `WaitForNKS4ReadEvent` functions:

```
  Coprocessor Event/Response
              │
              ▼
   [ InterruptCallback ]
              │
              ▼
[ SendNKS4EventToLinuxReader ] ──(Writes payload)──► [ BSS+0x4b0 ] (Local Result Buffer)
              │                                            │
              ▼                                            ▼
       (rt_sem_signal) ──────────────────────────────► (rt_sem_wait)
                                                           │
                                                           ▼
                                                [ WaitForNKS4ReadEvent ]
                                                           │
                                                           ▼
                                                 Synchronous System Caller
```

If the coprocessor delivers a response frame before `WaitForNKS4ReadEvent` has successfully written its destination pointer to `BSS+0x4b0`, the incoming data is silently discarded, causing the blocking caller to hang. The coprocessor must ensure all response frames are delayed to target the active wait window.

---

## Structural Payload Byte-Swap Rules {#sec:byte-swap}

To accommodate differences in endianness between the x86 Host CPU and the coprocessor bus, a strict byte-swapping policy is enforced on the USB interface. 

Every 32-bit word in a variable-length Bulk OUT pixel stream or a codec command block must be byte-reversed (converted to big-endian order) before transmission:

```c
// Applied to every 4-byte word in the designated data block:
uint32_t swapped_word = __builtin_bswap32(original_word);
```

This byte-swap rule applies to:
*   **Video Region Header Packets** (`0xC0`, `0xC2`, `0xC4`)
*   **Video Pixel Data Payloads** (`0xC6`)
*   **NV2AC Codec Control Commands** (`0xE0`, `0xE1`)

This byte-swap rule **does not** apply to:
*   **4-byte Panel Configuration Commands** sent via the Video URB pool.
*   **4-byte Interrupt IN Event Frames**.
