#!/bin/bash
set -e

IMG="$PWD/lab_disk.img"
MNT="$PWD/mnt_lab"

# 1. Create 64 MB disk image if it does not exist
if [ ! -f "$IMG" ]; then
    dd if=/dev/zero of="$IMG" bs=1M count=64
fi

# 2. Attach to a loop device if not already attached
LOOP_DEV=$(losetup -j "$IMG" 2>/dev/null | cut -d: -f1 | head -n 1)
if [ -z "$LOOP_DEV" ]; then
    LOOP_DEV=$(sudo losetup -f --show "$IMG")
fi

# 3. Format with ext4 only if no filesystem exists yet
if ! sudo blkid "$LOOP_DEV" >/dev/null 2>&1; then
    sudo mkfs.ext4 "$LOOP_DEV"
fi

# 4. Create mount directory and mount if not already mounted
mkdir -p "$MNT"
if ! mountpoint -q "$MNT"; then
    sudo mount "$LOOP_DEV" "$MNT"
fi

echo "Media B ready: $LOOP_DEV mounted at $MNT"