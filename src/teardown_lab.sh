#!/bin/bash
set -e

IMG="$PWD/lab_disk.img"
MNT="$PWD/mnt_lab"

# 1. Unmount if currently mounted, then remove mount directory
if mountpoint -q "$MNT"; then
    sudo umount "$MNT"
fi
if [ -d "$MNT" ]; then
    rmdir "$MNT" 2>/dev/null || true
fi

# 2. Detach any loop device(s) linked to the image
LOOP_DEVS=$(losetup -j "$IMG" 2>/dev/null | cut -d: -f1 || true)
for dev in $LOOP_DEVS; do
    sudo losetup -d "$dev"
done

# 3. Delete the disk image safely
rm -f "$IMG"

echo "Media B teardown complete."