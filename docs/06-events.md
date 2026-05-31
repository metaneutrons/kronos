# Panel Events and Input Scanning Specification {#sec:events}

All human-machine interface (HMI) interactions, including capacitive touch events, mechanical button presses, rotary encoder rotations, and analog controller movements, are digitized by the coprocessor and sent to the Host via the Interrupt IN endpoint (`EP1`) as structured 4-byte frames.

## Event Dispatch Pipeline {#sec:event-dispatch}

When an Interrupt IN packet is received, the Host's `ReceiveEventBuffer` routine (located at `.text+0x2CB0`) dispatches individual 4-byte event payloads based on the value of the `type` byte (`byte[3]`):

![Event Dispatch Logic Flow](img/event-dispatch.png){#fig:event-dispatch}

```{.mermaid caption="Host Event Dispatch Pipeline Architecture" #fig:event-dispatch-src}
flowchart TD
    RX[Receive NKS4Command via INT IN] --> CHK{byte3 == 0x87?}
    CHK -->|Yes| END[End of transfer]
    CHK -->|No| TYPE{byte3 type?}
    TYPE -->|0x00| T0[Touch & Button Handler]
    TYPE -->|0x01| T1{byte2 & 0xF0?}
    TYPE -->|0x03| T3[Analog Controller Dispatch]
    TYPE -->|0x07| T7[SPDIF Clock Monitor]
    TYPE -->|0x61| T61[SPDIF Sync Init]
    TYPE -->|0x62| T62[SPDIF Sync Stream]
    TYPE -->|0xE1| TE1[NV2AC Read Completion]

    T1 -->|0x50| ENC[Rotary Encoder Decode]
    T1 -->|0x71| RESP[SendNKS4EventToLinuxReader]
    T1 -->|other| BTN[Button/Switch Matrix scan]

    T0 --> SUB{byte2 & 0xF0?}
    SUB -->|0x10| TOUCH[Touch Coordinate Decode]
    SUB -->|0x30| TOUCH
    SUB -->|0x40| TOUCH
    SUB -->|0x66| RESP
    SUB -->|0x70| RESP
    SUB -->|0x80+| SCAN[System State Scan]
```

---

## Event Payload Formats {#sec:event-types-summary}

Each active input category is formatted into a unique 4-byte payload. The general command layout is:
```
[byte0, byte1, byte2, type]
```

### Protocol Event Classification

| Type (`byte[3]`) | Sub-type (`byte[2]`) | Target Input / Event | Host Processing Routine |
| :---: | :---: | :--- | :--- |
| **`0x00`** | `0x1x` | Capacitive Touch Press | `HandleTouchPanel` |
| **`0x00`** | `0x30` | Mechanical Switch Press | `HandleSwitchEvent(code, true)` |
| **`0x00`** | `0x40` | Mechanical Switch Release | `HandleSwitchEvent(code, false)` |
| **`0x00`** | `0x66` | `CommunicationCheck` ACK | `SendNKS4EventToLinuxReader` |
| **`0x00`** | `0x70` | `GetVersion` Response | `SendNKS4EventToLinuxReader` |
| **`0x01`** | `0x71` | `ReadPortConfiguration` Response | `SendNKS4EventToLinuxReader` |
| **`0x01`** | `0x5x` | Rotary Encoder Delta | `HandleRotary(value)` |
| **`0x03`** | `0xNN` | Analog Controller ADC (0-1023) | `HandleAnalogController(id, value)` |
| **`0x07`** | N/A | SPDIF Hardware Clock Status | Updates clock synchronization flags |
| **`0xE1`** | N/A | Multi-byte Atmel/Register Response | `SignalAtmelReadComplete` |

---

## SPDIF Clock Synchronization Frames (`0x61` / `0x62`) {#sec:spdif-events}

The system manages S/PDIF digital audio clock synchronization using two real-time frame types:

```
Type 0x61: [value_hi, value_lo, source_id | flags, 0x61]  (Sync Init)
Type 0x62: [value_hi, value_lo, source_id | flags, 0x62]  (Sync Stream)
```

*   **`source_id`**: Extracted from `byte[2] & 0x3F`.
*   **`value`**: Computed as a 16-bit big-endian value: `(byte[0] << 8) | byte[1]`.

---

## Dual Keybed Scanning Architecture {#sec:keyboard}

The Host orchestrates keyboard scanning using two completely independent hardware paths:

### Path 1: Velocity-Sensitive Scan Link (COM Port UART) {#sec:keybed-uart}

To avoid USB scheduling latency, the primary 88-key velocity matrix is scanned by a dedicated coprocessor microcontroller, which routes real-time note events directly to the Host's `CSTGKeybedInterface` (within `OA.ko`) over a dedicated serial UART link:

```
Keyboard Keybed Matrix
          │
          ▼
   [ Coprocessor ] (Sub-millisecond dual-contact scan & velocity calc)
          │
          ▼ (Physical UART Serial Interconnect)
   [ Host COM Port ] (I/O mapped registers at 0x240 / 0x3F8)
          │
          ▼
[ CSTGKeybedInterface ] (Real-time RTAI interrupt handler in OA.ko)
```

*   **Access Mode**: Direct x86 `in`/`out` port registers, bypasses standard kernel USB stacks.
*   **External MIDI Accessory Routing Separation**: The USB MIDI interface (Interface 2, managed by `USBMidiAccessory.ko`) is strictly dedicated to external accessories connected to the physical MIDI IN/OUT DIN jacks. It is not involved in processing internal keybed events.

### Path 2: USB Scanning Control Interface (`ConfigureScanning`) {#sec:scanning-usb}

The Host's synthesis engine controls coprocessor scanning by writing an 8-bit mask to the coprocessor via the USB Bulk OUT control path. The coprocessor applies this mask to its scanning hardware:

| Bit Position | Hexadecimal Mask | Target Hardware Operation |
| :---: | :---: | :--- |
| **7** | `0x80` | Globally enable mechanical keybed matrix scanning |
| **6** | `0x40` | Enable physical keybed aftertouch (pressure) scanning |
| **4** | `0x10` | Enable analog expression pedal input scanning |
| **3** | `0x08` | Reserved configuration parameter |
| **2** | `0x04` | Reserved configuration parameter |
| **1** | `0x02` | Reserved configuration parameter |
| **0** | `0x01` | Reserved configuration parameter |

---

## Input Event Conversion Specifications {#sec:input-events}

### Capacitive Touch Coordinates Decoding Math {#sec:touch-events}

When the user touches the panel, the coprocessor generates a 4-byte touch coordinate frame:
```
[X_raw, Y_raw, 0x1P, 0x00]
```
Where `P` represents the active logical panel ID (`byte[2] & 0x0F`), ranging from 0 to 15. The Host's `HandleTouchPanel` routine converts the raw coordinates into standard MIDI control values using linear scaling:

$$\Delta X = X_{\text{raw}} - 64, \quad \text{where } X_{\text{raw}} \in [64, 191]$$

$$\Delta Y = Y_{\text{raw}} - 13, \quad \text{where } Y_{\text{raw}} \in [13, 255]$$

$$\text{Column Index} = \lfloor 127 - (\Delta X \times 0.992188) \rfloor, \quad \text{Column} \in [0, 127]$$

$$\text{Row Index} = \lfloor \Delta Y \times 0.034483 \rfloor, \quad \text{Row} \in [0, 8]$$

*   **Column Index**: Mapped to a standard 7-bit MIDI control value (0-127).
*   **Row Index**: Selects one of the 8 physical capacitive touch strips on the panel. The row index is added to `28` to yield the final `eSTGButtonCode` button press event sent to the synthesis engine.

### Mechanical Button Events (Press / Release) {#sec:button-events}

Mechanical button actions are sent using press and release frames:
```
[0x00, button_code, 0x30, 0x00]   // State: Pressed
[0x00, button_code, 0x40, 0x00]   // State: Released
```
*   **`button_code`**: Unique 7-bit identifier (`byte[1] & 0x7F`), ranging from `0` to `78` (`0x4E`). These map directly to physical panel controls (e.g., switches, navigation pads, transport controls).

### Rotary Encoder Delta Decoding {#sec:encoder-events}

Rotary encoder movements are sent as signed 16-bit relative deltas:
```
[delta_lo, delta_hi, 0x50, 0x01]
```
*   **Relative Rotation**: Decoded as a 16-bit signed integer: `(int16_t)((delta_hi << 8) | delta_lo)`. Positive values represent clockwise rotation, while negative values represent counter-clockwise rotation.

### Analog Controller Value Reconstruction Math {#sec:analog-events}

High-resolution analog inputs (e.g., sliders, pitch bend joysticks, expression pedals) are digitized by a 10-bit analog-to-digital converter (ADC). The 10-bit value is split across two bytes to fit the `NKS4Command` frame:
```
[value_lo, value_hi, controller_id, 0x03]
```

The Host reconstructs the original 10-bit ADC value ($V \in [0, 1023]$) using the following bitwise operations:

$$V_{\text{base}} = (V_{\text{lo}} \ll 2) \mid ((V_{\text{hi}} \gg 6) \ \& \ 0x03)$$

$$\text{If } (V_{\text{lo}} \ \& \ 0x01) \neq 0, \quad V = V_{\text{base}} \mid 0x04; \quad \text{Else}, \quad V = V_{\text{base}}$$

To match standard MIDI control conventions, the Host mirrors the reconstructed value around its center point (`512`) and downsamples it to a 7-bit value:

$$V_{\text{mirrored}} = 1023 - V$$

$$V_{\text{final}} = V_{\text{mirrored}} \gg 3, \quad V_{\text{final}} \in [0, 127]$$

#### Analog Controller ID Assignments (`eSTGAnalogDeviceCode`)

The `controller_id` byte (`byte[2] & 0x3F`) maps directly to specific physical hardware controls:

| Controller ID | Physical Hardware Control | Operational Notes |
| :---: | :--- | :--- |
| **`0 - 7`** | Realtime Control Knobs 1 - 8 | Continuous rotary pots |
| **`8 - 15`** | Mixer Drawbars / Sliders 1 - 8 | Linear volume sliders |
| **`16`** | Joystick X-Axis | Pitch bend control (centered at 512) |
| **`17`** | Joystick Y-Axis | Modulation control |
| **`18`** | Ribbon Controller | Linear touch-sensitive ribbon |
| **`19 - 23`** | Pedals 1 - 5 | Continuous expression and damper inputs |
| **`24`** | Mode Selector | Dispatches control behavior based on active system state |

### Coprocessor Event Packing Implementation

To transmit a digitized 10-bit ADC value to the Host, the coprocessor's firmware must pack the bits into the standard `NKS4Command` structure:

```c
void transmit_analog_event(uint8_t controller_id, uint16_t adc_10bit) {
    // Clamps the input to 10-bit resolution (0 to 1023)
    uint16_t val = adc_10bit & 0x3FF;
    
    // Distribute bits across payload bytes
    uint8_t byte0 = (val >> 2) & 0xFE;  // Upper 7 bits mapped to bits 7:1
    if (val & 0x04) {
        byte0 |= 0x01;                  // Map bit 2 to bit 0
    }
    uint8_t byte1 = (val << 6) & 0xC0;  // Lower 2 bits mapped to bits 7:6
    
    // Assemble and send the 4-byte NKS4Command
    uint8_t event_frame[4] = { byte0, byte1, controller_id, 0x03 };
    usb_interrupt_in_transmit(event_frame, 4);
}
```
