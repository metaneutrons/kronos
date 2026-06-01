#!/bin/bash
#
# Build a deterministic QEMU disk image for Korg Kronos emulation.
#
# Downloads the KRONOS 3.2.2 update from Korg's CDN (419MB), extracts
# the system, decrypts filesystem images, and applies minimal patches.
#
# Usage: ./qemu/build-disk.sh
#
# Requirements: curl, 7z, python3 (pycryptodome), objdump, sudo
#
set -euo pipefail
cd "$(dirname "$0")/.."

# --- Configuration ---
DVD1_URL="https://storage.korg.com/kronos_dvd/KRONOS3/KronosDVD1_3_2_1.iso"
DVD1_SHA256="b7550e50dd7b9b319864b283d5876f7ec5b895320f01fd89957806f0d69bb8bf"
DVD1_FILE="downloads/KronosDVD1_3_2_1.iso"

UPDATE_URL="https://cdn.korg.com/us/support/download/files/59180c871025155934ae1d5cb7e237bc.zip?response-content-disposition=attachment%3Bfilename%2A%3DUTF-8%27%27KRONOS_Update_3_2_2.zip&response-content-type=application%2Foctet-stream%3B"
UPDATE_SHA256="19d7b6bbb1ce3895377a576d2324d65c44a78aa0da556a9969af23d46afcf6fd"
UPDATE_ZIP="downloads/KRONOS_Update_3_2_2.zip"

DISK="local/kronos.img"
REAUTH="$(ls local/*.reauth 2>/dev/null | head -1)"

# AES-256-CBC keys (identical across all Kronos versions)
KEY_EVA="342ee59d549c7d329d835537be0540d"
KEY_MOD="a336a15cd841ec8926b99e7c3884eaa"
KEY_WAV="3e72c0e59fc017a9eb7d7e1168a4cdb"

echo "=== Korg Kronos QEMU Disk Builder (3.2.2) ==="

# --- Step 1: Download ---
mkdir -p downloads
if [ ! -f "$DVD1_FILE" ]; then
    echo "[1/7] Downloading KRONOS 3.2.1 DVD1 (7.8GB)..."
    curl -L -o "$DVD1_FILE.tmp" "$DVD1_URL" --progress-bar
    mv "$DVD1_FILE.tmp" "$DVD1_FILE"
else
    echo "[1/7] DVD1 already present."
fi
echo "  Verifying DVD1 checksum..."
echo "$DVD1_SHA256  $DVD1_FILE" | sha256sum -c --quiet

if [ ! -f "$UPDATE_ZIP" ]; then
    echo "  Downloading KRONOS 3.2.2 update (419MB)..."
    curl -L -o "$UPDATE_ZIP.tmp" "$UPDATE_URL" --progress-bar
    mv "$UPDATE_ZIP.tmp" "$UPDATE_ZIP"
fi
echo "  Verifying update checksum..."
echo "$UPDATE_SHA256  $UPDATE_ZIP" | sha256sum -c --quiet

# --- Step 2: Extract system ---
echo "[2/7] Extracting base system from DVD1 + 3.2.2 update..."
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

7z e "$DVD1_FILE" -o"$TMPDIR" system.tar.gz -y >/dev/null 2>&1
unzip -o -j "$UPDATE_ZIP" "KRONOS_Update_3_2_2/KRONOS_Update_3_2_2.tar.gz" -d "$TMPDIR" >/dev/null

# --- Step 3: Create disk image ---
echo "[3/7] Creating 60GB disk image..."
mkdir -p local
rm -f "$DISK"
qemu-img create -f raw "$DISK" 60G >/dev/null
sfdisk "$DISK" >/dev/null 2>&1 <<'PARTS'
label: dos
p1 : start=2048, size=65536, type=83, bootable
p2 : start=67584, size=4194304, type=83
p3 : start=4261888, size=1048576, type=82
p4 : start=5310464, size=120518656, type=5
p5 : start=5312512, size=2097152, type=83
p6 : start=7411712, size=118417408, type=83
PARTS

# --- Step 4: Format and populate ---
echo "[4/7] Formatting and extracting system..."
LOOP=$(sudo losetup --find --show --partscan "$DISK")
sudo mkfs.ext2 -q "${LOOP}p2"
sudo mkfs.ext2 -q "${LOOP}p5"
sudo mkfs.ext3 -q "${LOOP}p6"
sudo mkswap "${LOOP}p3" >/dev/null

ROOT="/tmp/kronos_root_$$"
sudo mkdir -p "$ROOT"
sudo mount "${LOOP}p2" "$ROOT"

# Extract base system from DVD1
sudo tar xzf "$TMPDIR/system.tar.gz" -C "$ROOT" 2>/dev/null
if [ -d "$ROOT/mnt/sbin" ]; then
    sudo cp -a "$ROOT"/mnt/* "$ROOT/" && sudo rm -rf "$ROOT/mnt"
fi

# Overlay 3.2.2 update (newer kernel, modules, images)
sudo tar xzf "$TMPDIR/KRONOS_Update_3_2_2.tar.gz" -C "$ROOT" 2>/dev/null
if [ -d "$ROOT/mnt/sbin" ]; then
    sudo cp -a "$ROOT"/mnt/* "$ROOT/" && sudo rm -rf "$ROOT/mnt"
fi

# Extract kernel
mkdir -p local
cp "$ROOT/boot/bzImage" local/bzImage-korg 2>/dev/null || true

# --- Step 5: Decrypt filesystem images ---
echo "[5/7] Decrypting filesystem images..."
sudo mkdir -p "$ROOT/korg/ro"
for img_name in Eva.img Mod.img WaveMotion.img; do
    src="$ROOT/korg/ro/$img_name"
    [ -f "$src" ] || continue
    sudo cp "$src" "$TMPDIR/$img_name"
    sudo chown $(id -u):$(id -g) "$TMPDIR/$img_name"
    case "$img_name" in
        Eva.img)        key="$KEY_EVA" ;;
        Mod.img)        key="$KEY_MOD" ;;
        WaveMotion.img) key="$KEY_WAV" ;;
    esac
    python3 -c "
from Crypto.Cipher import AES
import sys
key = b'${key}\x00' if '${key}' else sys.exit(1)
with open('$TMPDIR/$img_name', 'rb') as f:
    data = f.read()
dec = bytearray()
for i in range(0, len(data), 512):
    iv = (i // 512).to_bytes(16, 'little')
    dec.extend(AES.new(key, AES.MODE_CBC, iv).decrypt(data[i:i+512]))
with open('$TMPDIR/$img_name', 'wb') as f:
    f.write(dec)
"
    sudo cp "$TMPDIR/$img_name" "$src"
    echo "  ✓ $img_name ($(du -h "$src" | cut -f1))"
done

# Extract OA.ko from decrypted Mod.img
mkdir -p local/firmware
MODMNT="$TMPDIR/mod_mount"
sudo mkdir -p "$MODMNT"
sudo mount -o loop,ro "$ROOT/korg/ro/Mod.img" "$MODMNT"
cp "$MODMNT/OA.ko" local/firmware/
cp "$MODMNT/KorgUsbAudioDriver.ko" local/firmware/
sudo umount "$MODMNT"

sudo mkdir -p "$ROOT/korg/Mod"
sudo cp local/firmware/OA.ko "$ROOT/korg/Mod/"
sudo cp local/firmware/KorgUsbAudioDriver.ko "$ROOT/korg/Mod/"

# --- Step 6: Apply patches ---
echo "[6/7] Applying patches..."

# loadmod.ko: 4 anti-tamper NOPs (see Appendix F)
LOADMOD="$ROOT/sbin/loadmod.ko"
INIT_OFF=$(objdump -h "$LOADMOD" 2>/dev/null | awk '/.init.text/{print "0x"$6}')
printf '\x90\x90\x90\x90\x90\x90' | sudo dd of="$LOADMOD" bs=1 seek=$(($INIT_OFF + 0x3b)) conv=notrunc 2>/dev/null
printf '\x90\x90\x90\x90\x90\x90' | sudo dd of="$LOADMOD" bs=1 seek=$(($INIT_OFF + 0x48)) conv=notrunc 2>/dev/null
printf '\x90\x90\x90\x90\x90\x90' | sudo dd of="$LOADMOD" bs=1 seek=$(($INIT_OFF + 0x5b)) conv=notrunc 2>/dev/null
printf '\x90\x90' | sudo dd of="$LOADMOD" bs=1 seek=$(($INIT_OFF + 0xbd)) conv=notrunc 2>/dev/null
echo "  ✓ loadmod.ko (4 NOP patches)"

# OA.ko: Atmel auth skip (see Appendix F)
OA="$ROOT/korg/Mod/OA.ko"
OA_INIT_OFF=$(objdump -h "$OA" 2>/dev/null | awk '/.init.text/{print "0x"$6}')
printf '\xeb' | sudo dd of="$OA" bs=1 seek=$(($OA_INIT_OFF + 0x19f)) conv=notrunc 2>/dev/null
echo "  ✓ OA.ko (Atmel auth skip)"

# OA.ko: Keybed COM port bypass (optional, for D525 without ESP32 UART)
if [ "${PATCH_KEYBED:-}" = "1" ]; then
    # Offset varies by version: 0x244 (3.0.1/3.1.3) or 0x2a2 (3.2.x)
    KEYBED_OFF=0x2a2
    printf '\xeb' | sudo dd of="$OA" bs=1 seek=$(($OA_INIT_OFF + $KEYBED_OFF)) conv=notrunc 2>/dev/null
    echo "  ✓ OA.ko (keybed bypass at +$KEYBED_OFF)"
fi

# Install .pairFact3
if [ -f "$REAUTH" ]; then
    sudo cp "$REAUTH" "$ROOT/.pairFact3"
fi

# --- Step 7: Create boot script ---
echo "[7/7] Creating boot configuration..."
sudo mkdir -p "$ROOT/korg/Eva" "$ROOT/korg/Mod" "$ROOT/korg/ro/WaveMotion"

sudo tee "$ROOT/sbin/loadoa.sh" >/dev/null <<'BOOT'
#!/bin/bash
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mount -o loop,ro /korg/ro/Eva.img /korg/Eva 2>/dev/null
mount -o loop,ro /korg/ro/Mod.img /korg/Mod 2>/dev/null
mount -o loop,ro /korg/ro/WaveMotion.img /korg/ro/WaveMotion 2>/dev/null
insmod /usr/realtime/modules/rtai_hal.ko rtai_cpufreq_arg=2496000000 rtai_apicfreq_arg=1000000000
insmod /usr/realtime/modules/rtai_sched.ko
insmod /usr/realtime/modules/rtai_sem.ko
insmod /usr/realtime/modules/rtai_fifos.ko
insmod /usr/realtime/modules/rtai_ndbg.ko
insmod /sbin/STGEnabler.ko
insmod /sbin/OmapNKS4Module.ko
insmod /sbin/OmapVideoModule.ko
insmod /korg/Mod/KorgUsbAudioDriver.ko
insmod /sbin/STGGmp.ko
insmod /sbin/USBMidiAccessory.ko
insmod /sbin/loadmod.ko
insmod /korg/Mod/OA.ko
echo "OA: $?" > /dev/ttyS0
echo "=== DONE ===" > /dev/ttyS0
BOOT
sudo chmod +x "$ROOT/sbin/loadoa.sh"

# Symlink loadoa → loadoa.sh
if [ -f "$ROOT/sbin/loadoa" ]; then
    sudo mv "$ROOT/sbin/loadoa" "$ROOT/sbin/loadoa.bin.orig"
fi
sudo ln -sf /sbin/loadoa.sh "$ROOT/sbin/loadoa"

# Cleanup
sudo umount "$ROOT"
sudo losetup -d "$LOOP"
sudo rmdir "$ROOT"

echo ""
echo "=== Done: $DISK ==="
echo "Run with: make run"
