# USB Composite Device Interface Specification {#sec:usb-device}

The NKS4 coprocessor exposes itself to the Host as a single physical USB 2.0 High-Speed composite device. The hardware platform registers a single Vendor ID (VID) and Product ID (PID) pair and presents three independent logical interfaces to the Host operating system.

## Device Descriptor Specification {#sec:device-descriptor}

To ensure proper device enumeration and driver binding by the Host's kernel, the composite device descriptor must conform to the following schema:

| Descriptor Field | Hexadecimal Value | System Specification |
| :--- | :---: | :--- |
| **`idVendor`** | `0x0944` | Designated Vendor ID for Korg Inc. |
| **`idProduct`** | `0x1005` | Designated Product ID for NKS4 Coprocessor |
| **`bDeviceClass`** | `0x00` | Class specification delegated to the interface descriptors |
| **`bNumConfigurations`** | `0x01` | Single logical configuration supported |
| **`bInterfaceSubClass`** | `0xFF` | Vendor-specific subclass (mandatory for Host driver matching) |

### Driver Matching Constraints

The Host's panel control driver `OmapNKS4Module.ko` implements a kernel `usb_device_id` match configuration using a match flags mask of `0x0383` (`USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_PRODUCT | USB_DEVICE_ID_MATCH_INT_CLASS | USB_DEVICE_ID_MATCH_INT_SUBCLASS`). Consequently, the device's panel interface descriptor must explicitly populate `bInterfaceSubClass` with `0xFF` and `bInterfaceProtocol` with `0xFF` to satisfy the kernel's match filter and trigger the `OmapNKS4Probe` driver probe routine.

## Co-Resident Interface Allocation {#sec:interface-overview}

The composite device distributes its features across three interfaces. Different kernel drivers bind to these interfaces simultaneously:

*   **Interface 0**: Panel Control & Register Interface, claimed by `OmapNKS4Module.ko`.
*   **Interface 1**: Multichannel Isochronous Digital Audio Interface, claimed by `KorgUsbAudioDriver.ko` via `usb_driver_claim_interface()`.
*   **Interface 2**: Asynchronous Bulk USB MIDI Interface, claimed by `KorgUsbAudioDriver.ko`.

![Composite USB Interface Topography](img/usb-interfaces.png){#fig:usb-interfaces}

```{.mermaid caption="USB Composite Device Logical Interface Map" #fig:usb-interfaces-src}
graph LR
    subgraph "USB Device 0x0944:0x1005"
        IF0["Interface 0<br/>Panel (Vendor-specific, SubClass=0xFF)<br/>OmapNKS4Module.ko"]
        IF1["Interface 1<br/>Audio (Isochronous)<br/>KorgUsbAudioDriver.ko"]
        IF2["Interface 2<br/>MIDI (Bulk)<br/>KorgUsbAudioDriver.ko"]
    end

    IF0 --- EP1["EP1 IN<br/>Interrupt<br/>32 bytes, 1ms"]
    IF0 --- EP2["EP2 OUT<br/>Bulk<br/>512 bytes max"]
    IF0 --- EP7["EP3 IN (Panel BULK)<br/>Bulk<br/>512 bytes max"]
    IF1 --- EP3["EP4 OUT<br/>Isochronous<br/>Audio Output"]
    IF1 --- EP4["EP5 IN<br/>Isochronous<br/>Audio Input"]
    IF2 --- EP5["EP6 OUT<br/>Bulk<br/>MIDI Out"]
    IF2 --- EP6["EP7 IN<br/>Bulk<br/>MIDI In"]
```

## Interface Endpoint Topology {#sec:endpoint-details}

The composite layout defines endpoints to handle control packets, real-time events, audio streams, and MIDI frames. 

While the endpoint addresses for Interface 1 (Audio) and Interface 2 (MIDI) are dynamically discovered via standard USB descriptor queries, Interface 0 (Panel) enforces rigid endpoint properties to prevent Host driver URB allocation faults:

| Interface Rank | Endpoint Address | Transfer Direction | Transfer Type | Maximum Packet Size | Polling Interval | Functional Specification |
| :---: | :---: | :---: | :---: | :---: | :---: | :--- |
| **0** | `EP1` | IN | Interrupt | 32 Bytes | 1 ms | System clock ticks and panel controller events |
| **0** | `EP2` | OUT | Bulk | 512 Bytes | N/A | Configuration commands and raw pixel stream |
| **0** | `EP3` | IN | Bulk | 512 Bytes | N/A | Register-read responses (NV2AC / Atmel checks) |
| **1** | `EP4` | OUT | Isochronous | 1024 Bytes | 125 µs | High-bandwidth digital audio output (DAC stream)[^isoc-size] |
| **1** | `EP5` | IN | Isochronous | 1024 Bytes | 125 µs | High-bandwidth digital audio input (ADC stream) |
| **2** | `EP6` | OUT | Bulk | 64 Bytes | N/A | External device MIDI Out packets |
| **2** | `EP7` | IN | Bulk | 64 Bytes | N/A | External device MIDI In packets |

[^isoc-size]: The audio output requires a transfer of 4608 bytes per audio period (256 frames × 6 channels × 3 bytes). Under USB High-Speed parameters, this data block is split across multiple 125 µs microframes, utilizing high-bandwidth Isochronous endpoints supporting up to 1024 bytes per packet with multiple transactions per microframe (up to 3 × 1024 bytes).
