#!/bin/bash
# SpinOS: boot partition (firmware, kernel, config.txt) + squashfs root + small /data -> sdcard.img
set -e
BOARD_DIR="$(dirname "$0")"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"

# boot files: everything the Pi firmware needs, plus the device trees and the kernel
FILES=()
for i in "${BINARIES_DIR}"/*.dtb "${BINARIES_DIR}"/rpi-firmware/*; do
	FILES+=("${i#${BINARIES_DIR}/}")
done
FILES+=("Image")
BOOT_FILES=$(printf '\t\t\t"%s",\n' "${FILES[@]}")
sed "s|#BOOT_FILES#|${BOOT_FILES}|" "${BOARD_DIR}/genimage.cfg.in" >"${BINARIES_DIR}/genimage.cfg"

# an empty ext4 for /data; S01data grows it to the whole card on the first start
rm -f "${BINARIES_DIR}/data.ext4"
truncate -s 64M "${BINARIES_DIR}/data.ext4"
"${HOST_DIR}/sbin/mkfs.ext4" -q -F -L SPINDATA "${BINARIES_DIR}/data.ext4"

trap 'rm -rf "${ROOTPATH_TMP}"' EXIT
ROOTPATH_TMP="$(mktemp -d)"
rm -rf "${GENIMAGE_TMP}"
genimage --rootpath "${ROOTPATH_TMP}" --tmppath "${GENIMAGE_TMP}" --inputpath "${BINARIES_DIR}" \
	--outputpath "${BINARIES_DIR}" --config "${BINARIES_DIR}/genimage.cfg"
echo "SpinOS: ${BINARIES_DIR}/sdcard.img"
