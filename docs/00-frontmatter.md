---
title: "Korg Kronos — Hardware Emulation Reference"
subtitle: "Complete NKS4 USB Protocol Specification & ESP32-P4 Implementation Guide"
author: "KronosHacking Project"
date: 2026-05-31
lang: en
toc: true
toc-depth: 3
number-sections: true
reference-links: true
colorlinks: true
linkcolor: blue
geometry: margin=2.5cm
fontsize: 11pt
monofont: "JetBrains Mono"
header-includes:
  - \usepackage{longtable}
  - \usepackage{booktabs}
---

# Introduction {#sec:introduction}

## Architectural Paradigm and Scope {#sec:project-overview}

This document serves as an authoritative system reference manual for the emulation of the NKS4 panel control board within the Korg Kronos synthesizer workstation. The NKS4 is a specialized peripheral subsystem responsible for physical interface orchestration, capacitive touch scanning, rotary encoder tracking, analog-to-digital control conversions, high-fidelity audio digital-to-analog and analog-to-digital conversion (DAC/ADC), and keyboard matrix decoding.

The objective of this emulation framework is to replace the legacy, proprietary OMAP-based NKS4 hardware architecture with a modern, high-performance ESP32-P4 microcontroller. This co-design facilitates the execution of Korg's proprietary real-time synthesis engine and graphical user interface (GUI) application stack on standard commodity x86 architectures without altering Korg's proprietary core binary modules.

The technical specifications, register definitions, and protocol standards detailed herein have been rigorously mapped and verified by characterizing the interface boundaries of the proprietary kernel modules (`OmapNKS4Module.ko`, `OmapVideoModule.ko`, `KorgUsbAudioDriver.ko`, and `OA.ko`).

## Document Conventions {#sec:conventions}

*   **Byte Ordering**: All multi-byte data payloads conform to the **little-endian** byte-ordering standard unless explicitly designated otherwise.
*   **Transmission Order**: USB transaction payloads are indexed chronologically from byte 0.
*   **Register Addressing**: All hardware and software register addresses are designated in hexadecimal format (`0xNN`).
*   **Code Referencing**: Disassembled binary references are marked according to their offset relative to the base of the corresponding ELF binary (`module.ko:.text+0xNNNN`).
*   **Systems Nomenclature**:
    *   **Host**: Refers to the main x86 execution environment running the Linux kernel and the synthesis engine.
    *   **Device**: Refers to the NKS4 peripheral controller or its functional ESP32-P4 equivalent.

## Glossary {#sec:glossary}

| Term | Definition |
| :--- | :--- |
| **NKS4** | Proprietary panel control subsystem comprising a TI OMAP CPU, Atmel CryptoMemory, and NV2AC audio codec. |
| **Eva** | The primary userspace GUI application executing on the Host, implemented utilizing a Portable Embedded GUI (PEG) framework. |
| **OA.ko** | The core real-time synthesis engine and system manager kernel module. |
| **RTAI** | Real-Time Application Interface; a hard real-time Linux kernel extension. |
| **NV2AC** | Specialized proprietary audio codec and control device integrated on the NKS4 subsystem. |
| **BSS** | Block Started by Symbol; the uninitialized data segment of a relocated kernel module. |
| **URB** | USB Request Block; the standard Linux kernel descriptor for asynchronous USB transfers. |
| **I-pipe** | The Interrupt Pipeline patch utilized by RTAI to prioritize real-time interrupts over standard Linux kernel events. |
