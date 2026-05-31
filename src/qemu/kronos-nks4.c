/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Korg Kronos NKS4 USB Device Emulation — QEMU Prototype
 *
 * Copyright (C) 2026 metaneutrons
 *
 * This module emulates the Korg Kronos NKS4 coprocessor board as a QEMU USB
 * device. The NKS4 is an OMAP-based custom board that provides display, touch,
 * audio I/O, and DRM services to the Kronos synthesizer's Intel Atom host CPU.
 *
 * This implementation serves as the functional prototype for the ESP32-P4
 * firmware. All protocol logic validated here will be ported directly to the
 * ESP32 USB device stack.
 *
 * Emulated subsystems:
 *   - USB composite device (VID 0x0944, PID 0x1005)
 *   - 800×600 RGB565 framebuffer with region-based pixel transfer
 *   - Atmel AT88SC0204CA CryptoMemory DRM (10-step authentication)
 *   - NV2AC codec command routing (write-only, ACK on reads)
 *   - Panel configure sequence (CommunicationCheck, GetVersion, PortConfig)
 *
 * Usage: qemu-system-i386 ... -usb -device usb-nks4
 *
 * Debug output is intentional — this is a development prototype. All DRM
 * transactions, bulk commands, and responses are logged to stderr for
 * protocol analysis and verification.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "hw/usb.h"
#include "hw/usb/desc.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * USB Device Identity
 * ═══════════════════════════════════════════════════════════════════════════ */

#define NKS4_VENDOR_ID          0x0944      /* Korg Inc. */
#define NKS4_PRODUCT_ID         0x1005      /* Kronos NKS4 */

/* ═══════════════════════════════════════════════════════════════════════════
 * Display Configuration
 * ═══════════════════════════════════════════════════════════════════════════ */

#define NKS4_FB_WIDTH           800         /* Horizontal resolution (pixels) */
#define NKS4_FB_HEIGHT          600         /* Vertical resolution (pixels) */
#define NKS4_FB_BPP             16          /* Bits per pixel (RGB565) */
#define NKS4_FB_STRIDE          (NKS4_FB_WIDTH * 2)  /* Bytes per scanline */
#define NKS4_FB_SIZE            (NKS4_FB_WIDTH * NKS4_FB_HEIGHT * 2)

/* ═══════════════════════════════════════════════════════════════════════════
 * USB Endpoint Configuration
 * ═══════════════════════════════════════════════════════════════════════════ */

#define NKS4_INT_IN_SIZE        32          /* Interrupt IN max packet (bytes) */
#define NKS4_BULK_MAX_PACKET    512         /* Bulk OUT max packet (bytes) */
#define NKS4_TIMER_INTERVAL_MS  1           /* INT IN poll interval */

/* ═══════════════════════════════════════════════════════════════════════════
 * NKS4 Command Type Identifiers (byte[3] of 4-byte command frame)
 *
 * Commands arrive on Bulk OUT. The type byte determines dispatch:
 *   - 0x00/0x01: Panel configure commands (require INT IN response)
 *   - 0xE0: NV2AC codec write (fire-and-forget)
 *   - 0xE1: NV2AC codec read / Atmel DRM read (requires INT IN response)
 *   - 0xC2/0xC4/0xC6/0x83: Video transfer commands (byte-swapped on wire)
 *   - 0x87: End-of-transfer marker
 * ═══════════════════════════════════════════════════════════════════════════ */

#define CMD_TYPE_PANEL_KEY      0x00        /* Panel key/button/configure */
#define CMD_TYPE_PANEL_PORT     0x01        /* Panel port/encoder events */
#define CMD_TYPE_END_MARKER     0x87        /* Transfer terminator */
#define CMD_TYPE_NV2AC_WRITE    0xE0        /* Codec write (no response) */
#define CMD_TYPE_NV2AC_READ     0xE1        /* Codec/Atmel read (response required) */
#define CMD_TYPE_VIDEO_REGION   0xC2        /* Define pixel transfer region */
#define CMD_TYPE_VIDEO_FILL     0xC4        /* Solid-color rectangle fill */
#define CMD_TYPE_VIDEO_PIXELS   0xC6        /* Pixel data payload */
#define CMD_TYPE_VIDEO_END      0x83        /* End pixel transfer */

/* ═══════════════════════════════════════════════════════════════════════════
 * Panel Configure Command Registers (byte[2] of command frame)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PANEL_REG_COMM_CHECK    0xEE        /* CommunicationCheck → responds 0x0066 */
#define PANEL_REG_GET_VERSION   0xF0        /* GetVersion → responds 0x0070 */
#define PANEL_REG_PORT_CONFIG   0xF1        /* ReadPortConfiguration → responds 0x0171 */

/* ═══════════════════════════════════════════════════════════════════════════
 * Panel Configure Response Codes (byte[2] of INT IN response)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define RESP_COMM_CHECK         0x66        /* CommunicationCheck ACK */
#define RESP_GET_VERSION        0x70        /* GetVersion ACK */
#define RESP_PORT_CONFIG        0x71        /* ReadPortConfiguration ACK */

/* ═══════════════════════════════════════════════════════════════════════════
 * Atmel CryptoMemory Register Addresses (byte[2] of E1/E0 command)
 *
 * The DRM subsystem in loadmod.ko communicates with the Atmel AT88SC0204CA
 * chip via these register addresses, tunneled through NV2AC codec commands.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define ATMEL_REG_READ          0xB6        /* Read from Atmel channel */
#define ATMEL_REG_KEY_READ      0xB2        /* Read symmetric key bytes */
#define ATMEL_REG_WRITE_B8      0xB8        /* Write to Atmel (no response) */
#define ATMEL_REG_WRITE_B4      0xB4        /* Write to Atmel (no response) */

/* ═══════════════════════════════════════════════════════════════════════════
 * Atmel Channel Identifiers (byte[0] of E1 command)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define ATMEL_CH_PUBLIC_ID      0x19        /* Public ID (7 bytes, device-unique) */
#define ATMEL_CH_ZONE           0x50        /* Zone data (integrity verification) */

/* ═══════════════════════════════════════════════════════════════════════════
 * Firmware Version Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define NKS4_FW_VERSION         0x21        /* Firmware version 2.1 */
#define NKS4_HW_VERSION         0x21        /* Hardware version 2.1 */
#define NKS4_HW_CONFIG          0x04        /* Hardware config (hw_ver=4) */

/* ═══════════════════════════════════════════════════════════════════════════
 * PIO Bypass Port (QEMU-only, not used on ESP32)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define NKS4_PIO_PORT           0x240

/* ═══════════════════════════════════════════════════════════════════════════
 * Device-Specific Atmel CryptoMemory Data
 *
 * On real hardware, these values are burned into the AT88SC0204CA chip and
 * cannot be read without the correct authentication sequence. For emulation,
 * we use known values recovered from a specific Kronos unit.
 *
 * The Public ID is device-unique. The .pairFact3 file downloaded from Korg's
 * server is encrypted specifically for this Public ID. Without the matching
 * 24-byte symmetric key, the .pairFact3 cannot be decrypted.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Public ID: 7 bytes uniquely identifying this Atmel chip instance */
static const uint8_t atmel_public_id[7] = {
    0x74, 0xFF, 0x31, 0xB4, 0xA6, 0xA7, 0xD8
};

/*
 * Zone verification data: 8 bytes used by loadmod.ko to verify that the
 * Atmel chip's .data section has not been tampered with. These are universal
 * constants (same on every Kronos unit).
 */
static const uint8_t atmel_zone_data[8] = {
    0xD7, 0xFC, 0x94, 0xF5, 0x71, 0x11, 0xFA, 0xE1
};

/*
 * Symmetric key material (24 bytes, device-specific):
 *   Bytes  0–15: Blowfish-CFB encryption key
 *   Bytes 16–23: Blowfish-CFB initialization vector
 *
 * loadmod.ko reads these in 3 sequential 8-byte reads (DRM steps 8–10).
 * The key is used to decrypt .pairFact3 → 48 bytes → 3 AES partition keys.
 *
 * WARNING: These are placeholder values ("KronosHacking!!!"). A real
 * deployment requires the actual 24 bytes from the target Atmel chip.
 * With placeholder keys, .pairFact3 decryption will produce garbage,
 * but this is irrelevant when using pre-decrypted filesystem images.
 */
static const uint8_t atmel_symmetric_key[24] = {
    0x4B, 0x72, 0x6F, 0x6E, 0x6F, 0x73, 0x48, 0x61,  /* "KronosHa" */
    0x63, 0x6B, 0x69, 0x6E, 0x67, 0x21, 0x21, 0x21,  /* "cking!!!" */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* IV (zeros) */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Device State
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct KronosNks4State {
    USBDevice dev;

    /* --- Display subsystem --- */
    QemuConsole *console;
    uint8_t     framebuffer[NKS4_FB_SIZE];  /* RGB565, row-major */
    bool        fb_dirty;
    int         dirty_x1, dirty_y1;         /* Dirty region (inclusive) */
    int         dirty_x2, dirty_y2;         /* Dirty region (exclusive) */

    /* --- Video transfer state (set by 0xC2, consumed by 0xC6) --- */
    uint32_t    vid_base_offset;            /* Byte offset into framebuffer */
    uint32_t    vid_row_bytes;              /* Bytes per row in transfer */
    uint32_t    vid_num_rows;               /* Total rows in transfer */
    uint32_t    vid_stride;                 /* Framebuffer stride (bytes) */
    uint32_t    vid_current_row;            /* Current row being written */
    uint32_t    vid_row_position;           /* Byte position within current row */

    /* --- INT IN response queue --- */
    uint8_t     response[64];               /* Pending response payload */
    int         response_len;               /* 0 = no pending response */

    /* --- DRM state --- */
    int         key_read_count;             /* Tracks sequential key reads (0–2) */
    bool        drm_active;                 /* Suppresses 0x71 during DRM */

    /* --- Protocol bookkeeping --- */
    int         bulk_command_count;         /* Total bulk OUT commands received */
    QEMUTimer   *timer;                     /* 1ms periodic timer */
    MemoryRegion pio;                       /* PIO bypass (QEMU-only) */
} KronosNks4State;

#define TYPE_KRONOS_NKS4 "usb-nks4"
#define KRONOS_NKS4(obj) OBJECT_CHECK(KronosNks4State, (obj), TYPE_KRONOS_NKS4)

/* ═══════════════════════════════════════════════════════════════════════════
 * USB Descriptors
 *
 * The NKS4 presents as a single-configuration, single-interface USB 2.0
 * device with 3 endpoints:
 *   EP1 IN  (Interrupt) — Responses and panel events, polled every 1ms
 *   EP2 OUT (Bulk)      — Commands, video data, DRM transactions
 *   EP3 IN  (Bulk)      — Reserved (NV2AC read responses, currently NAK'd)
 *
 * OmapNKS4Module.ko matches on VID:PID + bInterfaceSubClass=0xFF.
 * ═══════════════════════════════════════════════════════════════════════════ */

enum { STR_MANUFACTURER = 1, STR_PRODUCT, STR_SERIAL };

static const USBDescStrings nks4_desc_strings = {
    [STR_MANUFACTURER] = "KORG INC.",
    [STR_PRODUCT]      = "KRONOS NKS4",
    [STR_SERIAL]       = "000000000001",
};

static const USBDescIface nks4_desc_iface = {
    .bInterfaceNumber   = 0,
    .bNumEndpoints      = 3,
    .bInterfaceClass    = USB_CLASS_VENDOR_SPEC,
    .bInterfaceSubClass = 0xFF,     /* Required for driver matching */
    .bInterfaceProtocol = 0x00,
    .eps = (USBDescEndpoint[]) {
        {   /* EP1 IN: Interrupt — responses + panel events */
            .bEndpointAddress = USB_DIR_IN | 0x01,
            .bmAttributes     = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize   = NKS4_INT_IN_SIZE,
            .bInterval        = 1,
        },
        {   /* EP2 OUT: Bulk — commands + video pixel data */
            .bEndpointAddress = USB_DIR_OUT | 0x02,
            .bmAttributes     = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize   = NKS4_BULK_MAX_PACKET,
        },
        {   /* EP3 IN: Bulk — NV2AC read responses (reserved) */
            .bEndpointAddress = USB_DIR_IN | 0x03,
            .bmAttributes     = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize   = NKS4_BULK_MAX_PACKET,
        },
    },
};

static const USBDescDevice nks4_desc_device = {
    .bcdUSB            = 0x0200,
    .bDeviceClass      = USB_CLASS_VENDOR_SPEC,
    .bMaxPacketSize0   = 64,
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]) {{
        .bNumInterfaces        = 1,
        .bConfigurationValue   = 1,
        .bmAttributes          = USB_CFG_ATT_ONE,
        .bMaxPower             = 250,   /* 500mA */
        .nif = 1,
        .ifs = &nks4_desc_iface,
    }},
};

static const USBDesc nks4_usb_desc = {
    .id = {
        .idVendor      = NKS4_VENDOR_ID,
        .idProduct     = NKS4_PRODUCT_ID,
        .bcdDevice     = 0x0100,
        .iManufacturer = STR_MANUFACTURER,
        .iProduct      = STR_PRODUCT,
        .iSerialNumber = STR_SERIAL,
    },
    .full = &nks4_desc_device,
    .str  = nks4_desc_strings,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * INT IN Response Wire Format Builder
 *
 * OmapNKS4Module.ko expects multi-byte responses in a specific wire format
 * with per-32-bit-word byte reversal. The format is:
 *
 *   Word 0: [0x00, 0x00, 0x00, 0xE1]           — Header (type at byte 3)
 *   Word 1: [data[2], data[1], data[0], LEN]   — First data + length
 *   Word 2: [data[6], data[5], data[4], data[3]] — Subsequent data
 *   ...
 *   Final:  [0x00, 0x00, 0x00, 0x87]           — End marker
 *
 * This byte-swap matches the bswap32 applied by ContinueProcessingEvent
 * in OmapNKS4Module.ko before USB transmission.
 * ═══════════════════════════════════════════════════════════════════════════ */
static int build_drm_response(uint8_t *out, const uint8_t *data, int data_len)
{
    int pos = 0;

    /* Header word: type 0xE1 at byte position 3 */
    out[pos++] = 0x00;
    out[pos++] = 0x00;
    out[pos++] = 0x00;
    out[pos++] = CMD_TYPE_NV2AC_READ;

    /* First data word: [data[2], data[1], data[0], LENGTH] */
    out[pos++] = (data_len > 2) ? data[2] : 0x00;
    out[pos++] = (data_len > 1) ? data[1] : 0x00;
    out[pos++] = data[0];
    out[pos++] = (uint8_t)data_len;

    /* Subsequent data words: 4 bytes each, reversed */
    for (int base = 3; base < data_len; base += 4) {
        out[pos++] = (base + 3 < data_len) ? data[base + 3] : 0x00;
        out[pos++] = (base + 2 < data_len) ? data[base + 2] : 0x00;
        out[pos++] = (base + 1 < data_len) ? data[base + 1] : 0x00;
        out[pos++] = data[base];
    }

    /* End marker */
    out[pos++] = 0x00;
    out[pos++] = 0x00;
    out[pos++] = 0x00;
    out[pos++] = CMD_TYPE_END_MARKER;

    return pos;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DRM Command Handler
 *
 * Implements the 10-step Atmel authentication sequence expected by
 * loadmod.ko. This is the core protocol that the ESP32 firmware must
 * replicate exactly.
 *
 * Sequence:
 *   Steps 1,3,5,7: Channel reads via register 0xB6
 *   Steps 2,4,6:   Channel writes via registers 0xB8/0xB4 (no response)
 *   Steps 8,9,10:  Key reads via register 0xB2 (returns 24 bytes total)
 * ═══════════════════════════════════════════════════════════════════════════ */
static void handle_drm_read(KronosNks4State *s, uint8_t *cmd, int len)
{
    uint8_t channel = cmd[0];
    uint8_t reg     = cmd[2];
    uint8_t resp[16] = {0};
    int resp_len = 0;

    fprintf(stderr, "NKS4-DRM: Read reg=0x%02X ch=0x%02X cmd=[",
            reg, channel);
    for (int i = 0; i < len; i++) fprintf(stderr, "%02x", cmd[i]);
    fprintf(stderr, "]\n");

    if (reg == ATMEL_REG_READ) {
        /*
         * Register 0xB6: Atmel channel read.
         * Channel determines what data is returned.
         */
        if (channel == ATMEL_CH_PUBLIC_ID) {
            /* Step 1: Return 7-byte device-unique Public ID */
            memcpy(resp, atmel_public_id, 7);
            resp_len = 7;
            fprintf(stderr, "NKS4-DRM: [1/10] Public ID read\n");

        } else if (channel == ATMEL_CH_ZONE) {
            /*
             * Steps 3/5/7: Zone read. Length determines which step:
             *   1 byte  → zone status check (must be != 0xFF)
             *   8 bytes → full zone verification data
             */
            int read_len = (len >= 8) ? cmd[7] : 1;
            if (read_len == 1) {
                resp[0] = atmel_zone_data[0];   /* 0xD7 (!= 0xFF → pass) */
                resp_len = 1;
                fprintf(stderr, "NKS4-DRM: [3or7/10] Zone check (1 byte)\n");
            } else {
                memcpy(resp, atmel_zone_data, 8);
                resp_len = 8;
                fprintf(stderr, "NKS4-DRM: [5/10] Zone data (8 bytes)\n");
            }

        } else {
            /* Unknown channel — return single zero byte as ACK */
            resp[0] = 0x00;
            resp_len = 1;
            fprintf(stderr, "NKS4-DRM: Unknown channel 0x%02X\n", channel);
        }

    } else if (reg == ATMEL_REG_KEY_READ) {
        /*
         * Register 0xB2: Symmetric key read (steps 8, 9, 10).
         * Returns 8 bytes per read, 3 reads total = 24 bytes.
         * These 24 bytes form the Blowfish key+IV for .pairFact3 decryption.
         */
        int part = s->key_read_count;
        if (part < 3) {
            memcpy(resp, &atmel_symmetric_key[part * 8], 8);
            resp_len = 8;
            s->key_read_count++;
            fprintf(stderr, "NKS4-DRM: [%d/10] Key part %d\n", 8 + part, part + 1);
        } else {
            /* Extra reads beyond the expected 3 — return zeros */
            memset(resp, 0, 8);
            resp_len = 8;
            fprintf(stderr, "NKS4-DRM: Extra key read #%d (returning zeros)\n", part);
            s->key_read_count++;
        }

    } else {
        /* Unknown register — generic single-byte ACK */
        resp[0] = 0x00;
        resp_len = 1;
        fprintf(stderr, "NKS4-DRM: Unknown register 0x%02X\n", reg);
    }

    /* Encode response in wire format and queue for next INT IN poll */
    s->response_len = build_drm_response(s->response, resp, resp_len);
}

static void handle_drm_write(KronosNks4State *s, uint8_t *cmd, int len)
{
    /* DRM writes (0xB8, 0xB4) are fire-and-forget — no response needed */
    fprintf(stderr, "NKS4-DRM: Write reg=0x%02X ch=0x%02X len=%d data=[",
            cmd[2], cmd[0], len);
    for (int i = 0; i < len; i++) fprintf(stderr, "%02x", cmd[i]);
    fprintf(stderr, "]\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Panel Configure Command Handler
 *
 * During OmapNKS4Module.ko initialization, the host sends a sequence of
 * configure commands and blocks until the response arrives on INT IN.
 * The response must arrive on a SUBSEQUENT INT IN poll (not the same one).
 * ═══════════════════════════════════════════════════════════════════════════ */
static void handle_panel_configure(KronosNks4State *s, uint8_t *cmd)
{
    uint8_t reg  = cmd[2];
    uint8_t type = cmd[3];

    if (type == CMD_TYPE_PANEL_KEY && reg == PANEL_REG_COMM_CHECK) {
        /*
         * CommunicationCheck (0xEE): First command after USB enumeration.
         * Host sends [xx, xx, 0xEE, 0x00], expects [xx, xx, 0x66, 0x00].
         */
        s->response[0] = cmd[0];
        s->response[1] = cmd[1];
        s->response[2] = RESP_COMM_CHECK;
        s->response[3] = CMD_TYPE_PANEL_KEY;
        memset(&s->response[4], 0, 4);
        s->response[7] = CMD_TYPE_END_MARKER;
        s->response_len = 8;
        fprintf(stderr, "NKS4: CommunicationCheck → 0x%04X\n",
                (RESP_COMM_CHECK << 8) | CMD_TYPE_PANEL_KEY);

    } else if (type == CMD_TYPE_PANEL_KEY && reg == PANEL_REG_GET_VERSION) {
        /*
         * GetVersion (0xF0): Returns firmware and hardware version.
         * Response byte[2]=0x70 signals version data in byte[0:1].
         */
        s->response[0] = NKS4_FW_VERSION;
        s->response[1] = NKS4_HW_VERSION;
        s->response[2] = RESP_GET_VERSION;
        s->response[3] = CMD_TYPE_PANEL_KEY;
        memset(&s->response[4], 0, 4);
        s->response[7] = CMD_TYPE_END_MARKER;
        s->response_len = 8;
        fprintf(stderr, "NKS4: GetVersion → fw=%d.%d hw=%d.%d\n",
                NKS4_FW_VERSION >> 4, NKS4_FW_VERSION & 0xF,
                NKS4_HW_VERSION >> 4, NKS4_HW_VERSION & 0xF);

    } else if (type == CMD_TYPE_PANEL_PORT && reg == PANEL_REG_PORT_CONFIG) {
        /*
         * ReadPortConfiguration (0xF1): Returns hardware config byte.
         * Response type=0x01, byte[2]=0x71, byte[0]=hw_config.
         */
        s->response[0] = NKS4_HW_CONFIG;
        s->response[1] = 0x00;
        s->response[2] = RESP_PORT_CONFIG;
        s->response[3] = CMD_TYPE_PANEL_PORT;
        memset(&s->response[4], 0, 4);
        s->response[7] = CMD_TYPE_END_MARKER;
        s->response_len = 8;
        fprintf(stderr, "NKS4: ReadPortConfiguration → hw_config=0x%02X\n",
                NKS4_HW_CONFIG);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Video Command Handler
 *
 * Video commands arrive byte-swapped per 32-bit word (applied by
 * SubmitOmapNKS4CmdBulkWrite in OmapNKS4Module.ko). After the swap,
 * byte[3] contains the command type.
 *
 * Transfer sequence:
 *   1. 0xC2 (region): defines base offset, row width, row count, stride
 *   2. 0xC6 (pixels): sequential pixel data packets (up to 511 bytes each)
 *   3. 0x83 (end): transfer complete, trigger display refresh
 * ═══════════════════════════════════════════════════════════════════════════ */
static void handle_video_command(KronosNks4State *s, uint8_t *cmd, int len)
{
    uint8_t type = cmd[3];

    if (type == CMD_TYPE_VIDEO_REGION && len >= 12) {
        /*
         * Region definition (0xC2): 12 bytes after byte-swap.
         * Layout: [offset_hi, offset_mid, offset_lo, 0xC2,
         *          rowbytes_hi, rowbytes_mid, rowbytes_lo, rows_hi,
         *          rows_mid, rows_lo, stride_hi, stride_lo]
         */
        s->vid_base_offset = ((cmd[0] & 0x07) << 16) | (cmd[1] << 8) | cmd[2];
        s->vid_row_bytes   = ((cmd[4] & 0x07) << 16) | (cmd[5] << 8) | cmd[6];
        s->vid_num_rows    = ((cmd[7] & 0x07) << 16) | (cmd[8] << 8) | cmd[9];
        s->vid_stride      = ((cmd[10] & 0x07) << 8) | cmd[11];
        s->vid_current_row = 0;
        s->vid_row_position = 0;

    } else if (type == CMD_TYPE_VIDEO_PIXELS) {
        /*
         * Pixel data (0xC6): up to 511 bytes of RGB565 pixel data.
         * Data arrives byte-swapped per 32-bit word — we reverse it here.
         * Pixels are written sequentially into the region defined by 0xC2.
         */
        for (int i = 0; i + 3 < len; i += 4) {
            /* Reverse the byte-swap: wire order → native order */
            uint8_t native[4] = { cmd[i+3], cmd[i+2], cmd[i+1], cmd[i] };
            for (int j = 0; j < 4 && (i > 0 || j > 0); j++) {
                if (s->vid_current_row >= s->vid_num_rows) break;

                int fb_offset = s->vid_base_offset
                              + s->vid_current_row * s->vid_stride
                              + s->vid_row_position;

                if (fb_offset >= 0 && fb_offset < NKS4_FB_SIZE) {
                    s->framebuffer[fb_offset] = native[j];
                }

                s->vid_row_position++;
                if (s->vid_row_position >= s->vid_row_bytes) {
                    s->vid_row_position = 0;
                    s->vid_current_row++;
                }
            }
        }
        s->fb_dirty = true;
        s->dirty_x1 = 0;
        s->dirty_y1 = 0;
        s->dirty_x2 = NKS4_FB_WIDTH;
        s->dirty_y2 = NKS4_FB_HEIGHT;

    } else if (type == CMD_TYPE_VIDEO_FILL && len >= 12) {
        /*
         * Rectangle fill (0xC4): fills a region with a solid color.
         * Layout: [y_hi, y_mid, y_lo, 0xC4,
         *          height_hi, height_mid, height_lo, color_idx,
         *          width_hi, width_mid, width_lo, ...]
         */
        uint32_t y      = ((cmd[0] & 0x07) << 16) | (cmd[1] << 8) | cmd[2];
        uint32_t height = ((cmd[4] & 0x07) << 16) | (cmd[5] << 8) | cmd[6];
        uint32_t width  = ((cmd[8] & 0x07) << 16) | (cmd[9] << 8) | cmd[10];
        int8_t color_idx = (int8_t)cmd[7];

        /* Convert palette index to RGB565 (approximate grayscale) */
        uint16_t pixel = (color_idx & 0x3F) * 0x0410;

        for (uint32_t row = y; row < y + height && row < NKS4_FB_HEIGHT; row++) {
            for (uint32_t col = 0; col < width && col < NKS4_FB_WIDTH; col++) {
                int off = (row * NKS4_FB_WIDTH + col) * 2;
                if (off + 1 < NKS4_FB_SIZE) {
                    s->framebuffer[off]     = pixel & 0xFF;
                    s->framebuffer[off + 1] = (pixel >> 8) & 0xFF;
                }
            }
        }
        s->fb_dirty = true;
        s->dirty_x1 = 0;
        s->dirty_y1 = y;
        s->dirty_x2 = NKS4_FB_WIDTH;
        s->dirty_y2 = y + height;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main Bulk OUT Command Dispatcher
 *
 * All host-to-device communication arrives here. Commands are dispatched
 * by type byte (cmd[3]) to the appropriate subsystem handler.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void nks4_dispatch_command(KronosNks4State *s, uint8_t *cmd, int len)
{
    if (len < 4) return;

    uint8_t type = cmd[3];

    /* DRM/Atmel commands (highest priority) */
    if (type == CMD_TYPE_NV2AC_READ) {
        handle_drm_read(s, cmd, len);
        s->drm_active = true;
        return;
    }
    if (type == CMD_TYPE_NV2AC_WRITE) {
        handle_drm_write(s, cmd, len);
        s->drm_active = true;
        return;
    }

    /* Video commands (byte-swapped on wire) */
    if (type == CMD_TYPE_VIDEO_REGION || type == CMD_TYPE_VIDEO_PIXELS ||
        type == CMD_TYPE_VIDEO_FILL   || type == CMD_TYPE_VIDEO_END) {
        handle_video_command(s, cmd, len);
        return;
    }

    /* Panel configure commands */
    uint8_t reg = cmd[2];
    if ((type == CMD_TYPE_PANEL_KEY && (reg == PANEL_REG_COMM_CHECK ||
                                        reg == PANEL_REG_GET_VERSION)) ||
        (type == CMD_TYPE_PANEL_PORT && reg == PANEL_REG_PORT_CONFIG)) {
        handle_panel_configure(s, cmd);
        return;
    }

    /*
     * All other commands: no explicit response. After the configure phase,
     * OmapNKS4Module polls for 0x71 (port config) events. We auto-generate
     * these unless a DRM transaction is in progress.
     */
    if (s->bulk_command_count > 3 && s->response_len == 0 && !s->drm_active) {
        s->response[0] = NKS4_HW_CONFIG;
        s->response[1] = 0x00;
        s->response[2] = RESP_PORT_CONFIG;
        s->response[3] = CMD_TYPE_PANEL_PORT;
        memset(&s->response[4], 0, 4);
        s->response[7] = CMD_TYPE_END_MARKER;
        s->response_len = 8;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Display Subsystem
 * ═══════════════════════════════════════════════════════════════════════════ */

static void nks4_display_update(void *opaque)
{
    KronosNks4State *s = opaque;
    if (!s->fb_dirty) return;

    DisplaySurface *surface = qemu_console_surface(s->console);
    uint32_t *dest = (uint32_t *)surface_data(surface);

    for (int y = s->dirty_y1; y < s->dirty_y2 && y < NKS4_FB_HEIGHT; y++) {
        for (int x = s->dirty_x1; x < s->dirty_x2 && x < NKS4_FB_WIDTH; x++) {
            int offset = (y * NKS4_FB_WIDTH + x) * 2;
            uint16_t rgb565 = s->framebuffer[offset]
                            | (s->framebuffer[offset + 1] << 8);

            /* RGB565 → RGB888 expansion */
            uint8_t r = (rgb565 >> 11) << 3;
            uint8_t g = ((rgb565 >> 5) & 0x3F) << 2;
            uint8_t b = (rgb565 & 0x1F) << 3;

            dest[y * NKS4_FB_WIDTH + x] = rgb_to_pixel32(r, g, b);
        }
    }

    dpy_gfx_update(s->console, s->dirty_x1, s->dirty_y1,
                   s->dirty_x2 - s->dirty_x1, s->dirty_y2 - s->dirty_y1);
    s->fb_dirty = false;
}

static const GraphicHwOps nks4_display_ops = {
    .gfx_update = nks4_display_update,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Timer (1ms periodic tick for INT IN scheduling)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void nks4_timer_tick(void *opaque)
{
    KronosNks4State *s = opaque;
    timer_mod(s->timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + NKS4_TIMER_INTERVAL_MS);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PIO Bypass (QEMU-only — allows direct command injection without USB)
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint64_t nks4_pio_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0xFFFFFFFF;
}

static void nks4_pio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    KronosNks4State *s = opaque;
    if (addr != 0 || size != 4) return;

    uint8_t cmd[4] = {
        val & 0xFF, (val >> 8) & 0xFF,
        (val >> 16) & 0xFF, (val >> 24) & 0xFF
    };
    nks4_dispatch_command(s, cmd, 4);
}

static const MemoryRegionOps nks4_pio_ops = {
    .read       = nks4_pio_read,
    .write      = nks4_pio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * USB Transfer Handlers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void nks4_usb_handle_reset(USBDevice *dev)
{
    KronosNks4State *s = KRONOS_NKS4(dev);

    s->response_len = 0;
    s->key_read_count = 0;
    s->bulk_command_count = 0;
    s->drm_active = false;
    memset(s->framebuffer, 0, NKS4_FB_SIZE);
    s->fb_dirty = true;
    s->dirty_x1 = 0;
    s->dirty_y1 = 0;
    s->dirty_x2 = NKS4_FB_WIDTH;
    s->dirty_y2 = NKS4_FB_HEIGHT;
}

static void nks4_usb_handle_control(USBDevice *dev, USBPacket *p,
                                    int request, int value, int index,
                                    int length, uint8_t *data)
{
    if (usb_desc_handle_control(dev, p, request, value, index,
                                length, data) >= 0) {
        return;
    }
    p->status = USB_RET_STALL;
}

static void nks4_usb_handle_data(USBDevice *dev, USBPacket *p)
{
    KronosNks4State *s = KRONOS_NKS4(dev);

    switch (p->pid) {
    case USB_TOKEN_IN:
        if (p->ep->nr == 1) {
            /*
             * EP1 Interrupt IN: deliver pending response or idle marker.
             * Always returns exactly NKS4_INT_IN_SIZE bytes (zero-padded).
             */
            uint8_t buf[NKS4_INT_IN_SIZE] = {0};

            if (s->response_len > 0) {
                memcpy(buf, s->response, s->response_len);
                fprintf(stderr, "NKS4-RESP: delivering %d bytes:"
                        " %02x%02x%02x%02x",
                        s->response_len,
                        buf[0], buf[1], buf[2], buf[3]);
                for (int i = 4; i < s->response_len; i++)
                    fprintf(stderr, " %02x", buf[i]);
                fprintf(stderr, "\n");
                s->response_len = 0;
            } else {
                /* Idle: end marker only */
                buf[3] = CMD_TYPE_END_MARKER;
            }

            usb_packet_copy(p, buf, NKS4_INT_IN_SIZE);

        } else if (p->ep->nr == 3) {
            /* EP3 Bulk IN: reserved for NV2AC read responses (not used) */
            p->status = USB_RET_NAK;
        }
        break;

    case USB_TOKEN_OUT:
        if (p->ep->nr == 2) {
            /*
             * EP2 Bulk OUT: receive command packet from host.
             * Packets can be 4 bytes (simple command) up to 512 bytes
             * (video pixel data).
             */
            uint8_t buf[NKS4_BULK_MAX_PACKET];
            int len = p->iov.size;
            if (len > NKS4_BULK_MAX_PACKET) len = NKS4_BULK_MAX_PACKET;
            usb_packet_copy(p, buf, len);

            fprintf(stderr, "NKS4-BULK-OUT[%d]: %02x%02x%02x%02x len=%d\n",
                    s->bulk_command_count,
                    buf[0], buf[1], buf[2], buf[3], len);

            nks4_dispatch_command(s, buf, len);
            s->bulk_command_count++;
        }
        break;

    default:
        p->status = USB_RET_STALL;
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Device Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

static void nks4_realize(USBDevice *dev, Error **errp)
{
    KronosNks4State *s = KRONOS_NKS4(dev);

    usb_desc_init(dev);

    /* Initialize display console */
    s->console = graphic_console_init(DEVICE(dev), 0, &nks4_display_ops, s);
    qemu_console_resize(s->console, NKS4_FB_WIDTH, NKS4_FB_HEIGHT);

    /* Start periodic timer */
    s->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, nks4_timer_tick, s);
    timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);

    /* Clear framebuffer */
    memset(s->framebuffer, 0, NKS4_FB_SIZE);
    s->fb_dirty = true;
    s->dirty_x1 = 0;
    s->dirty_y1 = 0;
    s->dirty_x2 = NKS4_FB_WIDTH;
    s->dirty_y2 = NKS4_FB_HEIGHT;

    /* PIO bypass port (QEMU development aid, not part of ESP32 firmware) */
    memory_region_init_io(&s->pio, OBJECT(dev), &nks4_pio_ops, s,
                          "kronos-nks4-pio", 8);
    memory_region_add_subregion(get_system_io(), NKS4_PIO_PORT, &s->pio);

    fprintf(stderr, "NKS4: Device initialized"
            " (Public ID: 74FF31B4A6A7D8, DRM key: placeholder)\n");
}

static void nks4_unrealize(USBDevice *dev)
{
    KronosNks4State *s = KRONOS_NKS4(dev);
    timer_free(s->timer);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * QOM Type Registration
 * ═══════════════════════════════════════════════════════════════════════════ */

static void nks4_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc     = DEVICE_CLASS(klass);
    USBDeviceClass *uc  = USB_DEVICE_CLASS(klass);

    uc->realize         = nks4_realize;
    uc->unrealize       = nks4_unrealize;
    uc->handle_reset    = nks4_usb_handle_reset;
    uc->handle_control  = nks4_usb_handle_control;
    uc->handle_data     = nks4_usb_handle_data;
    uc->usb_desc        = &nks4_usb_desc;
    uc->product_desc    = "Korg Kronos NKS4 Panel Interface";

    dc->desc = "Korg Kronos NKS4 coprocessor emulation (USB)";
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo nks4_type_info = {
    .name          = TYPE_KRONOS_NKS4,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(KronosNks4State),
    .class_init    = nks4_class_init,
};

static void nks4_register_types(void)
{
    type_register_static(&nks4_type_info);
}

type_init(nks4_register_types)
