#!/usr/bin/env bash
# Build only: never install modules, change /boot, flash a partition or reboot.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
OUT=${OUT_DIR:-"$ROOT/out-kebab"}
JOBS=${JOBS:-4}
[[ $JOBS =~ ^[1-9][0-9]*$ ]] || { echo 'Invalid JOBS' >&2; exit 1; }
[[ -d "$OUT" && -s "$OUT/.config" ]] || {
    echo 'Reuse an existing configured build directory (default: out-kebab)' >&2
    exit 1
}
OUT=$(cd "$OUT" && pwd)
[[ "$OUT" != "$ROOT" ]] || { echo 'Refusing an in-tree build' >&2; exit 1; }
free -h
uptime
available=$(awk '/MemAvailable:/ {print $2}' /proc/meminfo)
(( available >= 2097152 )) || { echo 'Need at least 2 GiB available RAM' >&2; exit 1; }
cd "$ROOT"
# Preserve the existing config and object cache; merge only the X55 fragment.
scripts/kconfig/merge_config.sh -m -O "$OUT" "$OUT/.config" arch/arm64/configs/kebab_x55.config
make O="$OUT" ARCH=arm64 olddefconfig
make O="$OUT" ARCH=arm64 -j"$JOBS" Image Image.gz modules qcom/sm8250-oneplus-kebab-x55.dtb
mkdir -p "$OUT/x55-userspace/bin"
${CC:-cc} -O2 -Wall -Wextra -o "$OUT/x55-userspace/bin/mdm_helper_native" tools/remoteproc/mdm_helper_native.c
printf 'Built only. Kernel release: '
make -s O="$OUT" ARCH=arm64 kernelrelease
