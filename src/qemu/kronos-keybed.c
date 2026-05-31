/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Korg Kronos Keybed Serial Interface Emulation — QEMU Prototype
 *
 * Copyright (C) 2026 metaneutrons
 *
 * This module emulates the physical serial path between the Kronos host CPU
 * and the keyboard PSoC scanner. On real hardware, this is a 16550 UART
 * whose base address is discovered via the motherboard's Super I/O chip.
 *
 * The Kronos kernel module OA.ko (CSTGKeybedInterface::Startup) probes
 * COM ports 1–6 by:
 *   1. Unlocking the Super I/O chip (write 0x87 twice to port 0x4E)
 *   2. Reading the chip ID (register 0x20, must be 0x01–0xFE)
 *   3. Selecting a Logical Device Number (register 0x07)
 *   4. Reading the UART base address (registers 0x60/0x61)
 *   5. Reading the IRQ number (register 0x70)
 *   6. Sending handshake byte 0xA5 on the discovered UART
 *   7. Waiting for response 0xA0–0xAF + 2 version bytes via IRQ
 *
 * This device emulates both the Super I/O configuration interface and the
 * 16550 UART, providing an immediate handshake response. On real ESP32
 * hardware, the ESP32's physical UART replaces this emulation entirely.
 *
 * Usage: qemu-system-i386 ... -device kronos-keybed
 */

#include "qemu/osdep.h"
#include "hw/isa/isa.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qom/object.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Super I/O Configuration
 *
 * The Super I/O chip (IT8712/Winbond-compatible) is accessed via an
 * index/data register pair at I/O ports 0x4E/0x4F. The Korg kernel
 * uses the standard unlock sequence (write 0x87 twice) and reads
 * UART configuration from Logical Device registers.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define SUPERIO_PORT_INDEX      0x4E    /* Configuration index register */
#define SUPERIO_PORT_DATA       0x4F    /* Configuration data register */
#define SUPERIO_UNLOCK_KEY      0x87    /* Written twice to enter config mode */
#define SUPERIO_LOCK_KEY        0xAA    /* Written once to exit config mode */

/* Super I/O register addresses */
#define SIO_REG_CHIP_ID         0x20    /* Chip identification (must be 0x01–0xFE) */
#define SIO_REG_CHIP_REV        0x21    /* Chip revision */
#define SIO_REG_LDN             0x07    /* Logical Device Number select */
#define SIO_REG_ACTIVATE        0x30    /* Device activate (1 = enabled) */
#define SIO_REG_BASE_HI         0x60    /* UART base address high byte */
#define SIO_REG_BASE_LO         0x61    /* UART base address low byte */
#define SIO_REG_IRQ             0x70    /* Interrupt request number */

/* Super I/O response values */
#define SIO_CHIP_ID             0x87    /* IT8712-compatible chip ID */
#define SIO_CHIP_REV            0x12    /* Arbitrary revision number */

/* ═══════════════════════════════════════════════════════════════════════════
 * UART Configuration
 *
 * The UART base address must not conflict with QEMU's default COM ports
 * (0x3F8, 0x2F8, 0x3E8, 0x2E8). We use 0x240 which is in valid ISA
 * I/O space and satisfies the Korg driver's validation:
 *   - (base - 0x100) <= 0xEF8
 *   - (base & 7) == 0
 *
 * IRQ5 is used to avoid conflict with COM1's IRQ4. The RTAI interrupt
 * handler in OA.ko receives the keybed response via this IRQ.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define UART_BASE_ADDR          0x240   /* 16550 UART base I/O address */
#define UART_IRQ_NUM            5       /* Hardware interrupt line */
#define UART_NUM_REGISTERS      8       /* Standard 16550 register count */

/* 16550 register offsets (relative to base) */
#define UART_REG_RBR_THR        0       /* Receive Buffer / Transmit Holding */
#define UART_REG_IER_DLM        1       /* Interrupt Enable / Divisor High */
#define UART_REG_IIR_FCR        2       /* Interrupt ID / FIFO Control */
#define UART_REG_LCR            3       /* Line Control Register */
#define UART_REG_MCR            4       /* Modem Control Register */
#define UART_REG_LSR            5       /* Line Status Register */
#define UART_REG_MSR            6       /* Modem Status Register */
#define UART_REG_SCR            7       /* Scratch Register */

/* LSR bit definitions */
#define LSR_DATA_READY          0x01    /* Receive data available */
#define LSR_THRE                0x20    /* Transmit Holding Register Empty */
#define LSR_TEMT                0x40    /* Transmitter Empty */

/* IIR values */
#define IIR_NO_INTERRUPT        0x01    /* No interrupt pending */
#define IIR_RX_DATA_AVAIL       0x04    /* Received data available */

/* LCR bit definitions */
#define LCR_DLAB                0x80    /* Divisor Latch Access Bit */

/* IER bit definitions */
#define IER_RX_AVAIL            0x01    /* Enable Received Data Available IRQ */

/* ═══════════════════════════════════════════════════════════════════════════
 * Keybed Protocol Constants
 *
 * The handshake is minimal: host sends one byte, device responds with three.
 * After startup, the same UART carries standard MIDI messages (Note On/Off,
 * Aftertouch, CC) from the keyboard scanner.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define KEYBED_HANDSHAKE_REQ    0xA5    /* Host → Device: "are you there?" */
#define KEYBED_HANDSHAKE_ACK    0xA0    /* Device → Host: "yes" (0xA0–0xAF) */
#define KEYBED_VERSION_HI       0x02    /* Keyboard firmware version 2.1 */
#define KEYBED_VERSION_LO       0x01
#define KEYBED_RESPONSE_LEN     3       /* ACK + version high + version low */

/* ═══════════════════════════════════════════════════════════════════════════
 * Device State
 * ═══════════════════════════════════════════════════════════════════════════ */

#define TYPE_KRONOS_KEYBED "kronos-keybed"
OBJECT_DECLARE_SIMPLE_TYPE(KronosKeybedState, KRONOS_KEYBED)

struct KronosKeybedState {
    ISADevice parent_obj;

    /* --- Super I/O state machine --- */
    uint8_t     sio_index;          /* Currently selected register */
    uint8_t     sio_ldn;            /* Currently selected Logical Device */
    bool        sio_locked;         /* Config mode locked (true = locked) */
    uint8_t     sio_unlock_count;   /* Consecutive 0x87 writes seen */

    /* --- 16550 UART state --- */
    uint8_t     uart_lsr;           /* Line Status Register */
    uint8_t     uart_lcr;           /* Line Control Register */
    uint8_t     uart_ier;           /* Interrupt Enable Register */
    uint8_t     uart_mcr;           /* Modem Control Register */
    uint8_t     uart_rx_buf[KEYBED_RESPONSE_LEN];  /* Receive FIFO */
    uint8_t     uart_rx_count;      /* Bytes available in rx_buf */
    uint8_t     uart_rx_pos;        /* Current read position */
    qemu_irq    irq;                /* Hardware interrupt line */

    /* --- Port I/O registrations --- */
    PortioList  sio_portio;
    PortioList  uart_portio;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Super I/O Index Port (0x4E) — Write Handler
 *
 * Implements the standard Super I/O unlock/lock state machine:
 *   - Two consecutive writes of 0x87 → unlock (enter config mode)
 *   - Single write of 0xAA → lock (exit config mode)
 *   - Any other write while unlocked → select register index
 * ═══════════════════════════════════════════════════════════════════════════ */
static void sio_index_write(void *opaque, uint32_t addr, uint32_t val)
{
    KronosKeybedState *s = opaque;

    if (val == SUPERIO_UNLOCK_KEY) {
        s->sio_unlock_count++;
        if (s->sio_unlock_count >= 2) {
            s->sio_locked = false;
        }
        return;
    }

    if (val == SUPERIO_LOCK_KEY) {
        s->sio_locked = true;
        s->sio_unlock_count = 0;
        return;
    }

    s->sio_unlock_count = 0;

    if (!s->sio_locked) {
        s->sio_index = val;
    }
}

static uint32_t sio_index_read(void *opaque, uint32_t addr)
{
    return 0xFF;  /* Index port reads return 0xFF (write-only in practice) */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Super I/O Data Port (0x4F) — Read Handler
 *
 * Returns register values based on the currently selected index.
 * The Korg driver reads: chip ID, revision, LDN, base address, IRQ.
 * ═══════════════════════════════════════════════════════════════════════════ */
static uint32_t sio_data_read(void *opaque, uint32_t addr)
{
    KronosKeybedState *s = opaque;

    if (s->sio_locked) {
        return 0xFF;  /* All reads return 0xFF when locked */
    }

    switch (s->sio_index) {
    case SIO_REG_CHIP_ID:   return SIO_CHIP_ID;
    case SIO_REG_CHIP_REV:  return SIO_CHIP_REV;
    case SIO_REG_LDN:       return s->sio_ldn;
    case SIO_REG_ACTIVATE:  return 0x01;  /* Device always active */
    case SIO_REG_BASE_HI:   return (UART_BASE_ADDR >> 8) & 0xFF;
    case SIO_REG_BASE_LO:   return UART_BASE_ADDR & 0xFF;
    case SIO_REG_IRQ:       return UART_IRQ_NUM;
    default:                return 0x00;
    }
}

static void sio_data_write(void *opaque, uint32_t addr, uint32_t val)
{
    KronosKeybedState *s = opaque;

    if (s->sio_locked) return;

    if (s->sio_index == SIO_REG_LDN) {
        s->sio_ldn = val;  /* Select logical device for subsequent reads */
    }
}

static const MemoryRegionPortio sio_portio_list[] = {
    { 0, 1, 1, .read = sio_index_read, .write = sio_index_write },
    { 1, 1, 1, .read = sio_data_read,  .write = sio_data_write },
    PORTIO_END_OF_LIST(),
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 16550 UART — Read Handler
 *
 * Minimal 16550 implementation sufficient for the keybed handshake.
 * Only RBR, IER, IIR, LCR, MCR, and LSR are functionally implemented.
 * The DLAB (Divisor Latch Access Bit) in LCR switches register 0/1
 * between data mode and baud rate divisor mode.
 * ═══════════════════════════════════════════════════════════════════════════ */
static uint32_t uart_read(void *opaque, uint32_t addr)
{
    KronosKeybedState *s = opaque;
    addr -= UART_BASE_ADDR;

    switch (addr) {
    case UART_REG_RBR_THR:
        if (s->uart_lcr & LCR_DLAB) {
            return 0x01;  /* DLL: divisor latch low (baud rate config) */
        }
        /* Read next byte from receive buffer */
        if (s->uart_rx_pos < s->uart_rx_count) {
            uint8_t byte = s->uart_rx_buf[s->uart_rx_pos++];
            if (s->uart_rx_pos >= s->uart_rx_count) {
                s->uart_lsr &= ~LSR_DATA_READY;
                qemu_irq_lower(s->irq);
            }
            return byte;
        }
        return 0x00;

    case UART_REG_IER_DLM:
        if (s->uart_lcr & LCR_DLAB) {
            return 0x00;  /* DLM: divisor latch high */
        }
        return s->uart_ier;

    case UART_REG_IIR_FCR:
        if (s->uart_lsr & LSR_DATA_READY) {
            return IIR_RX_DATA_AVAIL;
        }
        return IIR_NO_INTERRUPT;

    case UART_REG_LCR:  return s->uart_lcr;
    case UART_REG_MCR:  return s->uart_mcr;
    case UART_REG_LSR:  return s->uart_lsr | LSR_THRE | LSR_TEMT;
    case UART_REG_MSR:  return 0x00;
    case UART_REG_SCR:  return 0x00;
    default:            return 0xFF;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 16550 UART — Write Handler
 *
 * When the host writes the handshake byte (0xA5) to the Transmit Holding
 * Register, we immediately queue the 3-byte response and raise IRQ to
 * notify the RTAI interrupt handler that data is available.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void uart_write(void *opaque, uint32_t addr, uint32_t val)
{
    KronosKeybedState *s = opaque;
    addr -= UART_BASE_ADDR;

    switch (addr) {
    case UART_REG_RBR_THR:
        if (s->uart_lcr & LCR_DLAB) {
            break;  /* DLL write — baud rate config, ignored */
        }
        /*
         * Keybed handshake: when host transmits 0xA5, respond with
         * [0xA0, VERSION_HI, VERSION_LO]. The response is placed in
         * the receive buffer and an IRQ is raised to wake the RTAI
         * handler (CSTGComPort::RTAIInterruptHandler).
         */
        if (val == KEYBED_HANDSHAKE_REQ) {
            s->uart_rx_buf[0] = KEYBED_HANDSHAKE_ACK;
            s->uart_rx_buf[1] = KEYBED_VERSION_HI;
            s->uart_rx_buf[2] = KEYBED_VERSION_LO;
            s->uart_rx_count  = KEYBED_RESPONSE_LEN;
            s->uart_rx_pos    = 0;
            s->uart_lsr      |= LSR_DATA_READY;

            if (s->uart_ier & IER_RX_AVAIL) {
                qemu_irq_raise(s->irq);
            }
        }
        break;

    case UART_REG_IER_DLM:
        if (s->uart_lcr & LCR_DLAB) {
            break;  /* DLM write — baud rate config, ignored */
        }
        s->uart_ier = val;
        break;

    case UART_REG_IIR_FCR:
        break;  /* FCR write — FIFO control, ignored */

    case UART_REG_LCR:
        s->uart_lcr = val;
        break;

    case UART_REG_MCR:
        s->uart_mcr = val;
        break;

    default:
        break;
    }
}

static const MemoryRegionPortio uart_portio_list[] = {
    { 0, UART_NUM_REGISTERS, 1, .read = uart_read, .write = uart_write },
    PORTIO_END_OF_LIST(),
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Device Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

static void kronos_keybed_realize(DeviceState *dev, Error **errp)
{
    ISADevice *isa = ISA_DEVICE(dev);
    KronosKeybedState *s = KRONOS_KEYBED(dev);

    /* Super I/O starts locked (requires 0x87 × 2 to unlock) */
    s->sio_locked = true;

    /* UART starts with TX ready (THRE + TEMT set) */
    s->uart_lsr = LSR_THRE | LSR_TEMT;

    /* Allocate IRQ line matching the value returned by SIO register 0x70 */
    s->irq = isa_get_irq(isa, UART_IRQ_NUM);

    /* Register Super I/O ports (0x4E index, 0x4F data) */
    isa_register_portio_list(isa, &s->sio_portio,
                             SUPERIO_PORT_INDEX, sio_portio_list, s,
                             "kronos-keybed-sio");

    /* Register UART ports (8 registers starting at UART_BASE_ADDR) */
    isa_register_portio_list(isa, &s->uart_portio,
                             UART_BASE_ADDR, uart_portio_list, s,
                             "kronos-keybed-uart");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * QOM Type Registration
 * ═══════════════════════════════════════════════════════════════════════════ */

static void kronos_keybed_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = kronos_keybed_realize;
    dc->desc    = "Korg Kronos keybed serial interface (Super I/O + 16550 UART)";
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo kronos_keybed_type_info = {
    .name          = TYPE_KRONOS_KEYBED,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(KronosKeybedState),
    .class_init    = kronos_keybed_class_init,
};

static void kronos_keybed_register_types(void)
{
    type_register_static(&kronos_keybed_type_info);
}

type_init(kronos_keybed_register_types)
