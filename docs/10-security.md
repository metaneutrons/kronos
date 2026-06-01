# Cryptographic Security & DRM Subsystem Analysis {#sec:security}

The Korg Kronos integrates a multi-layered digital rights management (DRM) and integrity verification subsystem. This chapter provides a rigorous cryptanalysis of the partition encryption parameters, hardware coprocessor protocols, dynamic system call interception, and kernel-space anti-tamper checks.

---

## Partition-Level Image Encryption (`cryptoloop`) {#sec:encrypted-images}

The Host's read-only system partitions are stored on the CompactFlash card as loopback filesystem images. To prevent offline binary analysis or modification, these images are encrypted using kernel-space loopback encryption:

| System Partition Image | Mounting Path | Contents Description |
| :--- | :--- | :--- |
| **`Eva.img`** | `/korg/Eva` | Userspace GUI application binary and interface assets |
| **`Mod.img`** | `/korg/Mod` | Kernel driver modules, including `OA.ko` and `KorgUsbAudioDriver.ko` |
| **`WaveMotion.img`** | `/korg/rw/PCM/WaveMotion` | WaveMotion PCM samples and sound bank multi-sample files |

### Cipher and Key Derivation Parameters

*   **Symmetric Cipher**: AES-256 in Cipher Block Chaining (CBC) mode (`lo_encrypt_type=18` / LO_CRYPT_CRYPTOAPI, `lo_crypt_name=aes`).
*   **Effective Key Length**: 124-bit. Due to an implementation truncation bug, only the first 31 characters of the 32-character hex key string are utilized, leaving the final nibble unmapped.
*   **Key Assignment**: The 31-character ASCII hex keys are identical across all Kronos production hardware:
    *   `Eva.img`: `342ee59d549c7d329d835537be0540d`
    *   `Mod.img`: `a336a15cd841ec8926b99e7c3884eaa`
    *   `WaveMotion.img`: `3e72c0e59fc017a9eb7d7e1168a4cdb`

---

## Hardware Coprocessor Protocol (Atmel AT88SC0204CA) {#sec:atmel-chip}

The symmetric partition keys are not stored in plaintext. They are encrypted within a local root file (`/.pairFact3`) and decrypted at boot time. 

The decryption key is derived through an interactive challenge-response handshake with a secure EEPROM chip—an **Atmel AT88SC0204CA CryptoMemory**—integrated into the NKS4 coprocessor board:

```
loadmod.ko / GetPubIdMod.ko
      │
      ├─── stgNV2AC_sync_cmd (type 0xE0) ─────► [ OmapNKS4Module.ko ]
      └─── stgNV2AC_sync_read_cmd (type 0xE1)          │
                                                       ▼ (USB Bulk OUT)
                                            [ Coprocessor (ESP32-P4) ]
                                                       │
                                                       ▼ (I2C Bus Interconnect)
                                            [ Atmel AT88SC0204CA Chip ]
```

The Atmel communication path is multiplexed over the same USB endpoint infrastructure used for the NV2AC audio codec control (see [@sec:nv2ac]). The coprocessor routes incoming type `0xE0`/`0xE1` register commands based on the target register address.

---

## Complete Multi-Stage Key Derivation Flow {#sec:pairfact-final}

The dynamic decryption of the partition-level AES keys from `/.pairFact3` involves a multi-stage, hardware-bound cryptographic chain executed by `loadmod.ko`. Under static analysis, Korg's proprietary kernel symbols are identified, mapping to specific steps in the key-derivation pipeline:

### The Dynamic Key-Derivation Algorithm

1.  **Coprocessor Characterization (`sdflkjsvnd2g`)**: The Host reads the 7-byte unique hardware identifier (Public ID) from Atmel channel `0x19` via a type `0xE1` command. It converts the ID to big-endian order and computes its square, yielding a 14-byte value (`ID_BE²`). This value is passed to `sdflkjsvnd2g(atmel_bytes, 0, buf)`, which invokes `__gmpz_powm` (RSA modular exponentiation) using Korg's public modulus and exponent. The resulting bignum array is used to initialize the primary Blowfish key-schedule arrays (`P-array` and `S-boxes`).
2.  **Hardware Challenge Verification (`fFfFfFfFfFfF1G` / `fFfFfFfFfFfF1H`)**: The Host issues a random challenge value to Atmel channel `0x50` using a type `0xE0` command. The coprocessor signs the challenge using its internal cryptographic engine. The Host reads the response from channel `0x50` and executes a validation check via `fFfFfFfFfFfF1G` and `fFfFfFfFfFfF1H`. A response byte containing `0xFF` represents an authentication failure and aborts system initialization.
3.  **Symmetric Key Recovery (`fFfFfFfFfFfF13`)**: The Host executes three synchronous volume-write register reads to retrieve the 24-byte symmetric key material from the coprocessor:
    *   `fFfFfFfFfFfF13(channel=0x08, volume=8, buf+0x00)`: Retrieves Part 1 (8 bytes).
    *   `fFfFfFfFfFfF13(channel=0x08, volume=8, buf+0x08)`: Retrieves Part 2 (8 bytes).
    *   `fFfFfFfFfFfF13(channel=0x10, volume=8, buf+0x10)`: Retrieves Part 3 (8 bytes).
4.  **Decryption of `/.pairFact3` (`moancjsd82`)**: The Host invokes `moancjsd82(key=24_response_bytes, data=.pairFact, len=80)`. This routine initializes a fresh Blowfish-CFB-64 key-schedule using the 24 bytes of symmetric key material retrieved in Step 3. It decrypts the 80-byte `/.pairFact3` file in memory, yielding three 16-byte binary key blocks (48 bytes total).
5.  **Hash Signature Verification (`md5`)**: The Host calculates the MD5 hash of the decrypted 48-byte key block and compares it against the reference signature to verify integrity.
6.  **Buffer Allocation (`aaaaaaaaa199`)**: If the MD5 hash is verified, `loadmod.ko` calls `aaaaaaaaa199(result)` to copy the 48-byte key buffer to a secure location in Host BSS memory (`BSS+0x1200`).
7.  **Loopback Mount Hex-Encoding**: At mount time, the system hex-encodes the keys in BSS space to produce three 31-character ASCII hex passphrases, passing them to the loopback mounting system.

---

## Cryptanalysis of the Key-Recovery Dependency Loop {#sec:cryptanalysis-loop}

A classic cryptographic dependency loop prevents deriving the 24-byte symmetric key offline from the known plaintext (the 31-character AES keys) and the encrypted ciphertext (`/.pairFact3` file):

1.  **Blowfish-CFB-64 Dependencies**: The `/.pairFact3` file is decrypted using Blowfish-CFB-64. The decryption process requires both a Blowfish key-schedule and an initialization vector (IV).
2.  **State-Dependent Initialization Vector**: Under Korg's implementation, the 24-byte payload retrieved from the volume-writes constitutes the Blowfish key-schedule key. The IV is dynamically generated as the intermediate Blowfish state *after* the key-schedule is completed with the 24-byte key.
3.  **The Dependency Loop**: To decrypt `/.pairFact3`, the 24-byte key must be known. To derive the 24-byte key by reversing the Blowfish-CFB-64 cipher using known plaintext-ciphertext pairs, the IV must be known. However, the IV cannot be calculated without already knowing the 24-byte key. 

$$\text{24-Byte Key} \longrightarrow \text{Key-Schedule Setup} \longrightarrow \text{Dynamic IV} \longrightarrow \text{Blowfish-CFB-64 Decryption}$$

This dependency loop makes brute-forcing the key space mathematically difficult. The IV's dependency on the 24-byte key space requires brute-forcing up to $2^{64}$ states. Consequently, recovering the 24-byte symmetric key requires capturing the three volume-write response payloads (`[0xB2, 0x00, channel, 0x08]`) using a physical USB bus analyzer on stock hardware.

---

## Dynamic System Call Interception (`loadmod.ko`) {#sec:syscall-hijack}

Following successful hardware verification, `loadmod.ko` modifies the Host's kernel system call table (`sys_call_table`) in Ring 0 to intercept file operations:

| Intercepted Syscall | Replacement Handler Function | Security Operational Role |
| :--- | :--- | :--- |
| **`sys_mount`** | Intercepts mount calls | Detects mount requests targeting `/korg/Eva`, `/korg/Mod`, or `/korg/WaveMotion`, automatically mounting the target image via loopback using the derived AES keys |
| **`sys_umount`** | Intercepts unmount calls | Ensures clean teardown of the encrypted loopback loop device mappings |
| **`sys_oldumount`** | Intercepts legacy unmount calls | Mandates standard loopback unmounting for compatibility |
| **`sys_ioctl`** | Intercepts system I/O control calls | Transparently routes loopback status queries |

To write to the `sys_call_table`, `loadmod.ko` requires write permissions on the kernel text page. Therefore, the Host kernel must be compiled with `CONFIG_DEBUG_RODATA=n`. If write-protection is enabled on kernel pages, the syscall overwrite faults, causing a kernel panic.

---

## Synthesis Engine Tamper Protection ("Cripple Check") {#sec:cripple-check}

To prevent launching the synthesis engine if `loadmod.ko` is bypassed, the Host kernel implements a cross-module validation check:

1.  During initialization, `loadmod.ko` writes a magic registration value **`0x22FB39CC`** into kernel memory using a modified `register_cdrom()` kernel interface.
2.  When `OA.ko` initializes its real-time DSP scheduler, it calls `cdrom_find_device()` to verify that this magic registration value is present.
3.  **Synthesis Behavior Options**:
    *   *Verification Success*: `OA.ko` scales all digital audio output samples by a factor of `{1.0, 1.0, 1.0, 1.0}`.
    *   *Verification Failure*: `OA.ko` scales all digital audio output samples by a factor of `{-1.0, -1.0, -1.0, -1.0}`. This phase inversion distorts the physical output, rendering the analog signal unusable.

---

## Dynamic Self-Check and Anti-Tamper Mechanics {#sec:loadmod-antitamper}

`loadmod.ko` implements several closely coupled anti-analysis checks. A failure in any early verification phase corrupts the internal state of subsequent stages, causing a cascading failure that obfuscates the original source of the error.

### Relocated Memory Hash Self-Test (`aaaaaaaaa6`)

When loaded, `loadmod.ko` hashes its relocated `.text` and `.data` memory spaces in RAM. The resulting 16-byte MD5 hash is compared against a reference table scattered across its `.data` segment:

```
Target Reference Offsets: [0x260, 0x19b, 0x22f, 0xd3, 0xec, 0x94, 0x25f, 0x113, 
                           0x3a1, 0x31a, 0x33c, 0x370, 0x304, 0x3cc, 0x276, 0x20e]
```

Because this routine hashes the relocated binary image in RAM, the check is highly relocation-dependent. If the module is loaded at memory addresses that differ from the original physical Kronos memory mapping, the relocated offset bytes change the hash, causing the self-test to fail.

### Pseudo-Random Number Generator String Obfuscation

All system strings (including log messages, filesystem paths, and device names) are stored in the `.data` segment as obfuscated blocks. The driver decrypts these strings at runtime using a LCG PRNG located at `BSS+0x11e0`:

$$X_{n+1} = (0x0BB38435 \times X_n + 0x3619636B) \pmod{2^{32}}$$

Because the PRNG state is seeded by the MD5 self-test routine, any relocation mismatch or binary tampering corrupts the PRNG seed. As a result, all subsequent string decryptions yield garbage data, causing subsequent file opens (e.g., loading `.pairFact3`) to fail silently.

---

## Alternative Operational Architecture Configurations {#sec:boot-options}

To run the synthesis engine on custom x86 boards or virtualized environments, two alternative operational configurations are defined:

```
                           [ System Boot Selection ]
                                      │
           ┌──────────────────────────┴──────────────────────────┐
           ▼                                                     ▼
 [ Config A: Decrypted Bypass ]                        [ Config B: Coprocessor Emulated ]
           │                                                     │
           ├─► Decrypt images offline                            ├─► Keep original loopback images
           ├─► Mount partitions directly                         ├─► Deploy stock loadmod.ko (unpatched)
           ├─► Pre-seed 0x22FB39CC in kernel                     ├─► ESP32 emulates AT88SC0204CA I2C
           └─► ESP32 returns generic ACKs                        └─► ESP32 returns 24 Blowfish key bytes
```

### Configuration Alpha: Decrypted Partition Bypass Mode

This configuration bypasses the entire DRM and coprocessor challenge-response pipeline. It is the recommended mode for rapid systems development:

1.  **Offline Decryption**: Decrypt the loopback images (`Eva.img`, `Mod.img`, `WaveMotion.img`) using the known AES keys and mount their contents directly.
2.  **Kernel Patching**: Deploy a custom Host kernel that pre-initializes the magic value `0x22FB39CC` within the `register_cdrom` address space, satisfying the synthesis engine's check.
3.  **Coprocessor Requirements**: The ESP32-P4 only needs to return a generic type `0xE1` acknowledgment (`[0x00, 0x00, 0x00, 0xE1]`) to avoid blocking volume write calls.

### Configuration Beta: Coprocessor-Emulated Cryptographic Loopback Mode

This configuration enables zero-patch operations, allowing the deployment of stock, unmodified Korg recovery disks:

1.  **Coprocessor Emulation**: The ESP32-P4 fully emulates the Atmel AT88SC0204CA CryptoMemory chip over the I2C bus.
2.  **Zero-Patch Integrity**: The Host runs the stock, unmodified kernel and the unpatched `loadmod.ko` module.
3.  **Key Delivery**: The ESP32-P4 must return the exact 24-byte symmetric key matching the Host's unique `/.pairFact3` file during Stage 3 volume-reads.

#### The `.pairFact3` Reauthorization Provisioning Process

The `/.pairFact3` file is device-specific and is downloaded from Korg's support website during unit registration. The file is not generated by the Host system or restore DVDs:

1.  **Public ID Capture**: The user reads the 24-character alphanumeric Public ID from the screen (or queries the ID using `GetPubIdMod.ko`).
2.  **Korg Support Request**: The user submits the Public ID to Korg's reauthorization portal.
3.  **RSA Cipher Generation**: Korg's server uses the unit's unique Atmel hardware signature (derived from the database records of its 7-byte ID) and Korg's private RSA key to generate the encrypted 80-byte `.reauth` file.
4.  **Host Provisioning**: The user downloads the `.reauth` file and writes it to `/.pairFact3` on the root filesystem partition. Because the file is encrypted using the 24-byte symmetric coprocessor key matching that specific Atmel chip, only the matching hardware coprocessor can generate the Blowfish parameters needed to decrypt the file at boot time.

---

## Atmel AT88SC0204CA Coprocessor Emulation Specification {#sec:atmel-emulation-spec}

To satisfy the Host's cryptographic check in **Configuration Beta**, the coprocessor's USB and I2C firmware must return the following responses for incoming register reads:

| USB Command Packet (Bulk OUT) | Coprocessor Target Response (Interrupt IN) | Target Cryptographic Stage |
| :--- | :--- | :--- |
| **`[0xB6, 0x00, 0x19, 0x07]`** | `[0x74, 0xFF, 0x31, 0xB4, 0xA6, 0xA7, 0xD8]` | **Public ID** (used in RSA key-schedule setup) |
| **`[0xB6, 0x00, 0x50, 0x08]`** | `[0xD7, 0xFC, 0x94, 0xF5, 0x71, 0x11, 0xFA, 0xE1]` | **Verification Signature** (channel `0x50` check) |
| **`[0xB2, 0x00, 0x08, 0x08]`** | 8-byte Device-Specific Key Payload (Part 1) | **Symmetric Key Bytes [0:7]** |
| **`[0xB2, 0x00, 0x08, 0x08]`** | 8-byte Device-Specific Key Payload (Part 2) | **Symmetric Key Bytes [8:15]** |
| **`[0xB2, 0x00, 0x10, 0x08]`** | 8-byte Device-Specific Key Payload (Part 3) | **Symmetric IV Bytes [16:23]** |
| **Any other type `0xE1`** | `[0x00, 0x00, 0x00, 0xE1]` | Generic command acknowledgment |

### Multi-Byte Response Packing and Wire Format {#sec:response-format}

As specified in [@sec:byte-swap], multi-byte response payloads returned via the Interrupt IN endpoint must be byte-reversed per 32-bit word. 

However, the Host reads the payload length byte directly from byte 7 of the raw wire packet *before* executing the word-level byte-swap. The coprocessor's USB controller must pack the transmission frame according to the following layout:

```
Wire Byte 0-3: [xx, xx, xx, 0xE1]     — Header (Type 0xE1 at byte 3)
Wire Byte 4:   data[2]                — First word, byte-swapped
Wire Byte 5:   data[1]
Wire Byte 6:   data[0]
Wire Byte 7:   LENGTH (N)             — Read before swap (N = total data bytes)
Wire Byte 8:   data[6]                — Second word, byte-swapped
Wire Byte 9:   data[5]
Wire Byte 10:  data[4]
Wire Byte 11:  data[3]                — Maps to data[3] after Host byte-swap
```

#### Channel `0x19` Wire Transmission Example

To transmit the 7-byte Public ID (`74 FF 31 B4 A6 A7 D8`), the coprocessor transmits a 12-byte payload over the USB wire:

```
Wire Payload: [0x00, 0x00, 0x00, 0xE1, 0x31, 0xFF, 0x74, 0x07, 0xD8, 0xA7, 0xA6, 0xB4]
               └────── Header ───────┘  └─ data[2:0] ─┘  Len   └──── data[6:3] ────┘
```

#### Channel `0x50` Wire Transmission Example

To transmit the 8-byte signature (`D7 FC 94 F5 71 11 FA E1`), the coprocessor transmits a 16-byte payload:

```
Wire Payload: [0x00, 0x00, 0x00, 0xE1, 0x94, 0xFC, 0xD7, 0x08, 0xFA, 0x11, 0x71, 0xF5, 0xE1, 0x00, 0x00, 0x00]
               └────── Header ───────┘  └─ data[2:0] ─┘  Len   └──── data[6:3] ────┘   └────── data[7] ──────┘
```

### Public ID Encoding Details {#sec:pubid-encoding}

The 24-character alphanumeric Public ID displayed on the system UI (e.g., `0313VC4Q0XUA6R9RLNA9GCLA`) is a custom base32 representation of the Atmel chip's 7-byte unique ID and its 1-byte CRC-8 checksum.

*   **Custom Base32 Alphabet**: Excludes confusing characters (`B`, `I`, `O`, `S`):
    ```
    0123456789ACDEFGHJKLMNPQRTUVWXYZ
    ```
*   **Decoding Mechanics**: Decodes as 4 logical words of 6 characters each. Each character represents a 5-bit value, shifted sequentially (`27, 22, 17, 12, 7, 2`) to reconstruct the Atmel ID and CRC.
*   **Checksum Verification**: The Atmel ID uses a CRC-8 polynomial with an initial value of `0x0A` and a generator polynomial of `0xD8` (`ValidateCRC` routine, spanning 780 bytes of compiled code).

---

## Complete Coprocessor Verification Handshake Specification (10 Steps) {#sec:atmel-handshake-steps}

> **Reference implementation:** [`src/qemu/kronos-nks4.c → handle_drm_read()`](https://github.com/metaneutrons/kronos/blob/main/src/qemu/kronos-nks4.c)

During boot time, `loadmod.ko` challenges the coprocessor using a strict 10-step sequence. This table defines the exact commands, response sizes, expected values, and cryptographic outcomes representing the complete verification specification:

| Step | Command (Bulk OUT) | Expected Response (Interrupt IN) | Subclass | Data Purpose & Outcome |
| :---: | :--- | :--- | :---: | :--- |
| **1** | `[0xB6, 0x00, 0x19, 0x07]` | 7 bytes: `[0x74, 0xFF, 0x31, 0xB4, 0xA6, 0xA7, 0xD8]` | Read | Retrieves coprocessor unique Public ID |
| **2** | `[0xB8, 0x00, 0x00, 0x00]` | None | Write | Triggers dynamic Blowfish state verification |
| **3** | `[0xB6, 0x00, 0x50, 0x01]` | 1 byte: `0xD7` | Read | Queries authentication zone status (must be $\neq 0xFF$) |
| **4** | `[0xB4, 0x03, 0x00, 0x00]` | None | Write | Directs initial cryptographic challenge data |
| **5** | `[0xB6, 0x00, 0x50, 0x08]` | 8 bytes: `[0xD7, 0xFC, 0x94, 0xF5, 0x71, 0x11, 0xFA, 0xE1]` | Read | Retrieves signed verification challenge (compares against `.data+0x20`) |
| **6** | `[0xB8, ...]` (20 bytes) | None | Write | Transmits Host dynamic verification challenge |
| **7** | `[0xB6, 0x00, 0x50, 0x01]` | 1 byte: `0xD7` | Read | Verifies second challenge state (must be $\neq 0xFF$) |
| **8** | `[0xB2, 0x00, 0x08, 0x08]` | 8 bytes: Device-Specific Symmetric Key (Part 1) | Read | Retrieves symmetric key bytes [0:7] (Blowfish-CFB key) |
| **9** | `[0xB2, 0x00, 0x08, 0x08]` | 8 bytes: Device-Specific Symmetric Key (Part 2) | Read | Retrieves symmetric key bytes [8:15] (Blowfish-CFB key) |
| **10** | `[0xB2, 0x00, 0x10, 0x08]` | 8 bytes: Device-Specific Symmetric Key (Part 3) | Read | Retrieves symmetric IV bytes [16:23] (Blowfish-CFB IV) |
