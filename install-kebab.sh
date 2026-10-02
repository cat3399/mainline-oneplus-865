#!/usr/bin/env bash
# Build and install the OnePlus 8T (kebab) kernel without cleaning the build cache.
# Android boot partitions are NEVER written unless --write-slot is supplied
# explicitly. The script never activates a slot and never reboots.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# Persistent build directory. NEVER create a new OUT_DIR (e.g. out-kebab2, a fresh
# clone's out dir, or a timestamped directory) just to "start clean": unless a
# build cache is severely corrupted/conflicting, always reuse the existing
# OUT_DIR and rely on the incremental make below so the object cache is kept.
OUT_DIR="${OUT_DIR:-${SCRIPT_DIR}/out-kebab}"
DEFCONFIG="${DEFCONFIG:-kebab_defconfig}"
DTB_NAME="${DTB_NAME:-sm8250-oneplus-kebab}"
JOBS="${JOBS:-$(nproc)}"
BOOT_DIR="${BOOT_DIR:-/boot}"
BACKUP_DIR="${BACKUP_DIR:-${BOOT_DIR}/boot-backups}"
TARGET_SLOT=""
SKIP_BUILD=0
WRITE_SLOT=0
STAGE_ONLY=0

usage() {
	cat <<'EOF'
Usage: sudo ./install-kebab.sh [options]

Options:
  --write-slot a|b Write the generated image to this explicitly named Android
                   boot slot after making and verifying a full backup.
                   Without this option no boot partition is touched.
  --skip-build     Reuse existing OUT_DIR artifacts without invoking make.
  --stage-only     Install matching modules and prepare files under
                   OUT_DIR/boot-stage (or BOOT_DIR). Do not change /boot links,
                   run update-initramfs hooks, write a slot, or reboot.
                   Requires --skip-build and is incompatible with --write-slot.
  -h, --help       Show this help.

Environment:
  OUT_DIR=path     Persistent build directory (default: ./out-kebab)
  DTB_NAME=name    DTB basename without .dtb (default: sm8250-oneplus-kebab)
  BOOT_DIR=path    File destination (default: /boot, or OUT_DIR/boot-stage
                   with --stage-only)
  JOBS=N           Parallel build jobs (default: nproc)
  BACKUP_DIR=path  Destination for pre-write full slot backups

This script never runs clean or mrproper, never guesses that an inactive slot
is disposable, never activates a slot, and never reboots.
EOF
}

while (($#)); do
	case "$1" in
		--write-slot)
			[[ $# -ge 2 ]] || { echo "--write-slot requires a value" >&2; exit 2; }
			TARGET_SLOT="$2"
			WRITE_SLOT=1
			shift 2
			;;
		--skip-build)
			SKIP_BUILD=1
			shift
			;;
		--stage-only)
			STAGE_ONLY=1
			shift
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			echo "Unknown option: $1" >&2
			usage >&2
			exit 2
			;;
	esac
done

[[ $EUID -eq 0 ]] || { echo "This script must run as root." >&2; exit 1; }
[[ "$DTB_NAME" =~ ^[a-zA-Z0-9_-]+$ ]] || { echo "Invalid DTB_NAME" >&2; exit 2; }
if ((STAGE_ONLY)); then
	((SKIP_BUILD && !WRITE_SLOT)) || {
		echo "--stage-only requires --skip-build and cannot write a slot." >&2
		exit 2
	}
	[[ "$BOOT_DIR" != /boot ]] || BOOT_DIR="$OUT_DIR/boot-stage"
	command -v mkinitramfs >/dev/null
fi
command -v qbootctl >/dev/null
command -v mkbootimg >/dev/null
command -v update-initramfs >/dev/null

CURRENT_SLOT="$(qbootctl -c | sed -n 's/^Current slot: _\([ab]\)$/\1/p')"
[[ "$CURRENT_SLOT" =~ ^[ab]$ ]] || { echo "Unable to determine current slot." >&2; exit 1; }

# The current slot holds the Linux (postmarketOS/Armbian) system. The OTHER slot
# is the Android slot and MUST NEVER be touched, overwritten, or treated as
# disposable. This script therefore never writes any slot unless --write-slot is
# passed explicitly, and even then only the named slot is written.

if [[ -z "$TARGET_SLOT" ]]; then
	# Generate a boot image suitable for the currently running slot, but do not
	# write it anywhere. Never infer that the other slot is disposable.
	TARGET_SLOT="$CURRENT_SLOT"
fi
[[ "$TARGET_SLOT" =~ ^[ab]$ ]] || { echo "Slot must be a or b." >&2; exit 2; }

ROOT_UUID="$(sed -n 's/.* root=UUID=\([^ ]*\).*/\1/p' /proc/cmdline)"
if [[ -z "$ROOT_UUID" ]]; then
	ROOT_UUID="$(findmnt -no UUID /)"
fi
[[ -n "$ROOT_UUID" ]] || { echo "Unable to determine root filesystem UUID." >&2; exit 1; }

cd "$SCRIPT_DIR"

if ((SKIP_BUILD == 0)); then
	# Keep the persistent object cache, but always synchronize .config with the
	# canonical device defconfig so newly added drivers are not silently omitted.
	make ARCH=arm64 O="$OUT_DIR" "$DEFCONFIG"
	# Deliberately incremental: never clean the persistent output directory, and
	# never point O= at a new directory just because a build failed or looks
	# stale. Reusing $OUT_DIR is what preserves the object cache; only a
	# genuinely corrupted/severe conflict justifies abandoning it (and even then
	# prefer repairing or moving the old one aside rather than starting fresh).
	make -j"$JOBS" ARCH=arm64 O="$OUT_DIR"
fi

KERNEL_RELEASE="$(make -s ARCH=arm64 O="$OUT_DIR" kernelrelease)"
IMAGE="$OUT_DIR/arch/arm64/boot/Image"
IMAGE_GZ="$OUT_DIR/arch/arm64/boot/Image.gz"
DTB="$OUT_DIR/arch/arm64/boot/dts/qcom/$DTB_NAME.dtb"
SYSTEM_MAP="$OUT_DIR/System.map"
KCONFIG="$OUT_DIR/.config"

for artifact in "$IMAGE" "$IMAGE_GZ" "$DTB" "$SYSTEM_MAP" "$KCONFIG"; do
	[[ -s "$artifact" ]] || { echo "Missing build artifact: $artifact" >&2; exit 1; }
done

EXPECTED_RESERVE="0 80b00000 0 af00000"
ACTUAL_RESERVE="$(fdtget -t x "$DTB" /reserved-memory/memory@80b00000 reg)"
[[ "$ACTUAL_RESERVE" == "$EXPECTED_RESERVE" ]] || {
	echo "Unexpected kebab reserved-memory range: $ACTUAL_RESERVE" >&2
	exit 1
}
[[ "$(fdtget -t s "$DTB" / model)" == "OnePlus 8T" ]] || {
	echo "DTB model is not OnePlus 8T." >&2
	exit 1
}
MP2650_NODE="/soc@0/geniqup@9c0000/i2c@994000/charger@5c"
[[ "$(fdtget -t s "$DTB" "$MP2650_NODE" compatible)" == "mps,mp2650" ]] || {
	echo "MP2650 charger node is missing from the kebab DTB." >&2
	exit 1
}
BQ28Z610_NODE="/soc@0/geniqup@8c0000/i2c@888000/bq27541-battery@55"
[[ "$(fdtget -t s "$DTB" "$BQ28Z610_NODE" compatible)" == "ti,bq28z610" ]] || {
	echo "BQ28Z610 fuel-gauge node is missing from the kebab DTB." >&2
	exit 1
}

if [[ "$DTB_NAME" == sm8250-oneplus-kebab-x55 ]]; then
	[[ "$(fdtget -t s "$DTB" /soc@0/modem compatible)" == "qcom,ext-sdx55m" ]] || {
		echo "X55 modem node missing from test DTB." >&2; exit 1;
	}
	[[ "$KERNEL_RELEASE" == *-kebab-x55-* ]] &&
		grep -q '^CONFIG_QCOM_ESOC=y$' "$KCONFIG" &&
		grep -qE '^CONFIG_MHI_BUS_SAHARA_MODEM=[ym]$' "$KCONFIG" || {
		echo "X55 DTB requires the matching X55 kernel configuration." >&2; exit 1;
	}
fi
if ((STAGE_ONLY)) && [[ "$KERNEL_RELEASE" == "$(uname -r)" ]]; then
	echo "Staging must use a distinct release, not the running kernel." >&2
	exit 1
fi
mkdir -p "$BOOT_DIR"

printf 'Installing kernel release %s\n' "$KERNEL_RELEASE"
printf 'Running slot: _%s; target slot: _%s\n' "$CURRENT_SLOT" "$TARGET_SLOT"

# Modules are installed alongside the currently running release and do not
# affect it. Repeated runs update this release in place.
make -j"$JOBS" ARCH=arm64 O="$OUT_DIR" modules_install INSTALL_MOD_PATH=/
depmod "$KERNEL_RELEASE"

install -m 0644 "$IMAGE" "$BOOT_DIR/vmlinuz-$KERNEL_RELEASE"
install -m 0644 "$SYSTEM_MAP" "$BOOT_DIR/System.map-$KERNEL_RELEASE"
install -m 0644 "$KCONFIG" "$BOOT_DIR/config-$KERNEL_RELEASE"

DTB_INSTALL_DIR="$BOOT_DIR/dtb-$KERNEL_RELEASE"
mkdir -p "$DTB_INSTALL_DIR/qcom"
install -m 0644 "$DTB" "$DTB_INSTALL_DIR/qcom/$DTB_NAME.dtb"

if ((STAGE_ONLY)); then
	# No update-initramfs post-update hooks: some distributions flash from them.
	grep -q '^CONFIG_RD_GZIP=y$' "$KCONFIG"
	mkinitramfs -c gzip -o "$BOOT_DIR/initrd.img-$KERNEL_RELEASE" "$KERNEL_RELEASE"
else
	if [[ -L "$BOOT_DIR/vmlinuz" ]]; then
		old_kernel="$(basename -- "$(readlink -f "$BOOT_DIR/vmlinuz")")"
		[[ "$old_kernel" == "vmlinuz-$KERNEL_RELEASE" ]] || ln -sfn "$old_kernel" "$BOOT_DIR/vmlinuz.old"
	fi
	if [[ -L "$BOOT_DIR/initrd.img" ]]; then
		old_initrd="$(basename -- "$(readlink -f "$BOOT_DIR/initrd.img")")"
		[[ "$old_initrd" == "initrd.img-$KERNEL_RELEASE" ]] || ln -sfn "$old_initrd" "$BOOT_DIR/initrd.img.old"
	fi

	if [[ -e "$BOOT_DIR/initrd.img-$KERNEL_RELEASE" ]]; then
		update-initramfs -u -k "$KERNEL_RELEASE"
	else
		update-initramfs -c -k "$KERNEL_RELEASE"
	fi

	ln -sfn "vmlinuz-$KERNEL_RELEASE" "$BOOT_DIR/vmlinuz"
	ln -sfn "vmlinuz-$KERNEL_RELEASE" "$BOOT_DIR/Image"
	ln -sfn "initrd.img-$KERNEL_RELEASE" "$BOOT_DIR/initrd.img"
	ln -sfn "dtb-$KERNEL_RELEASE" "$BOOT_DIR/dtb"
fi

WORK_DIR="$OUT_DIR/install-work"
mkdir -p "$WORK_DIR"
KERNEL_DTB_PAYLOAD="$WORK_DIR/Image.gz-dtb-$KERNEL_RELEASE"
BOOT_IMAGE="$BOOT_DIR/armbian-kernel-$KERNEL_RELEASE-slot_${TARGET_SLOT}.img"
cat "$IMAGE_GZ" "$DTB" > "$KERNEL_DTB_PAYLOAD"

mkbootimg \
	--header_version 0 \
	--kernel "$KERNEL_DTB_PAYLOAD" \
	--ramdisk "$BOOT_DIR/initrd.img-$KERNEL_RELEASE" \
	--base 0x0 \
	--second_offset 0x00f00000 \
	--cmdline "root=UUID=$ROOT_UUID slot_suffix=_$TARGET_SLOT" \
	--kernel_offset 0x8000 \
	--ramdisk_offset 0x1000000 \
	--tags_offset 0x100 \
	--pagesize 4096 \
	-o "$BOOT_IMAGE"

BOOT_IMAGE_SIZE="$(stat -c %s "$BOOT_IMAGE")"
SLOT_DEVICE="/dev/disk/by-partlabel/boot_$TARGET_SLOT"
SLOT_SIZE="$(blockdev --getsize64 "$SLOT_DEVICE")"
((BOOT_IMAGE_SIZE <= SLOT_SIZE)) || {
	echo "Boot image ($BOOT_IMAGE_SIZE bytes) exceeds slot size ($SLOT_SIZE bytes)." >&2
	exit 1
}

if ((WRITE_SLOT == 1)); then
	mkdir -p "$BACKUP_DIR"
	BACKUP_IMAGE="$BACKUP_DIR/boot_${TARGET_SLOT}-before-$KERNEL_RELEASE-$(date +%Y%m%d-%H%M%S).img"
	dd if="$SLOT_DEVICE" of="$BACKUP_IMAGE" bs=4M iflag=fullblock status=progress
	sync "$BACKUP_IMAGE"
	sha256sum "$BACKUP_IMAGE" > "$BACKUP_IMAGE.sha256"

	dd if="$BOOT_IMAGE" of="$SLOT_DEVICE" bs=4M conv=fsync,notrunc status=progress
	cmp -n "$BOOT_IMAGE_SIZE" "$BOOT_IMAGE" "$SLOT_DEVICE"
	sync
	printf 'Boot image verified on %s.\n' "$SLOT_DEVICE"
else
	printf 'No boot partition was written (use --write-slot a|b explicitly).\n'
fi

printf '\nInstallation complete.\n'
printf 'Kernel:      %s\n' "$KERNEL_RELEASE"
printf 'Boot image:  %s\n' "$BOOT_IMAGE"
printf 'Image slot:  _%s\n' "$TARGET_SLOT"
if ((WRITE_SLOT == 1)); then
	printf 'Written slot: _%s (not activated)\n' "$TARGET_SLOT"
else
	printf 'Written slot: none\n'
fi
printf 'Active slot remains: _%s\n' "$CURRENT_SLOT"
printf 'No reboot was performed.\n'
if ((STAGE_ONLY)); then
	printf 'Staging only: existing /boot files and symlinks were not changed.\n'
fi
