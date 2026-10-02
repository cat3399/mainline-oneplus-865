#!/usr/bin/env bash
# Manual X55 lifecycle. No flash, raw-NV writes, automatic retries or bearer setup.
set -euo pipefail
ROOT=/opt/x55
RPROC_CLASS=/sys/class/remoteproc
EFS_STATUS=/run/x55-efs/status.json
ATTEMPT=/run/x55-modem-attempt
LOCK=/run/lock/x55-modem.lock
SERVICES=(x55-mdm-helper.service x55-tqftpserv.service x55-pd-mapper.service x55-efs-sync.service)
export LD_LIBRARY_PATH="$ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

fail() { echo "$*" >&2; return 1; }

require_kernel() {
    [[ $EUID -eq 0 ]] || { fail 'Root required'; return 1; }
    [[ $(uname -r) == *-kebab-x55-* ]] || {
        fail 'Refusing: not running the X55 test kernel'; return 1;
    }
}

discover() {
    local dir name
    local -a matches=()
    for dir in "$RPROC_CLASS"/remoteproc*; do
        [[ -f "$dir/name" ]] || continue
        name=$(<"$dir/name")
        [[ "$name" == esoc* ]] && matches+=("$dir")
    done
    ((${#matches[@]} == 1)) || {
        fail "Expected one ESOC remoteproc; found ${#matches[@]}"; return 1;
    }
    RPROC=${matches[0]}
    ESOC_NAME=$(<"$RPROC/name")
    [[ "$ESOC_NAME" =~ ^esoc[0-9]+$ ]] || { fail 'Invalid ESOC name'; return 1; }
}

lock_control() {
    exec 9>"$LOCK"
    flock -n 9 || { fail 'Another modem control operation is in progress'; return 1; }
}

check_firmware() {
    local f
    for f in sbl1.mbn multi_image.mbn multi_image_qti.mbn xbl_cfg.elf apps.mbn aop.mbn tz.mbn acdb.mbn hyp.mbn \
             apdp.mbn devcfg.mbn sec.elf qdsp6sw.mbn modemr.jsn; do
        [[ -s "/lib/firmware/qcom/sdx55m/$f" ]] || {
            fail "Missing firmware: $f"; return 1;
        }
    done
    for f in efs1.bin efs2.bin efs3.bin; do
        [[ -f "/lib/firmware/qcom/sdx55m/$f" ]] &&
            [[ $(stat -c %s "/lib/firmware/qcom/sdx55m/$f") -eq 2097152 ]] || {
                fail "Expected a 2 MiB device-specific NV copy for $f, not a dummy image"; return 1;
            }
    done
    [[ -f /lib/firmware/qcom/sdx55m/mdmddr.bin ]] &&
        [[ $(stat -c %s /lib/firmware/qcom/sdx55m/mdmddr.bin) -eq 1048576 ]] || {
            fail 'Expected a 1 MiB device-specific mdmddr.bin copy'; return 1;
        }
    python3 - /lib/firmware/qcom/sdx55m/oemnvbktmp.bin <<'PY'
import os, struct, sys
p = sys.argv[1]
with open(p, 'rb') as f:
    h = struct.unpack('<10I', f.read(40))
    size = os.fstat(f.fileno()).st_size
if size != 2097152 or h[0] != 20 or h[1] != 3 or h[2] < 40 or not h[4] or h[2] + h[4] > size:
    sys.exit('Invalid own-device OEM image-20 header/size; refusing boot')
PY
    [[ $? -eq 0 ]] || return 1
    [[ -d /lib/firmware/qcom/sdx55m/modem_pr ]] || {
        fail 'Missing modem_pr remote filesystem'; return 1;
    }
}

check_loader() {
    [[ $(modinfo -F version mhi_sahara_modem) == kebab-oemnv-1 ]] || {
        fail 'Sahara module lacks the verified OnePlus OEM image-20 policy'; return 1;
    }
}

check_loaded_loader() {
    [[ -r /sys/module/mhi_sahara_modem/version ]] &&
        [[ $(< /sys/module/mhi_sahara_modem/version) == kebab-oemnv-1 ]] || {
            fail 'Loaded Sahara module is stale; no modem start submitted'; return 1;
        }
}

services_active() {
    local unit
    for unit in "${SERVICES[@]}"; do
        systemctl is-active --quiet "$unit" || {
            fail "$unit is not active; inspect its journal, do not retry the modem"; return 1;
        }
    done
}

efs_ready() {
    python3 -c 'import json,sys; s=json.load(open(sys.argv[1])); sys.exit(s.get("state") not in ("ready","saving"))' \
        "$EFS_STATUS" 2>/dev/null
}

require_live() {
    [[ $(<"$RPROC/state") == running ]] || { fail 'ESOC is not running'; return 1; }
    services_active || return 1
    efs_ready || { fail 'EFS channel is not ready; inspect x55-efs-sync'; return 1; }
    "$ROOT/bin/mdm_helper_native" --check-alive >/dev/null || {
        fail 'Modem STATUS/ERRFATAL is unhealthy; no further QMI command sent'; return 1;
    }
}

select_node() {
    local services i
    local -a nodes
    for i in {1..15}; do
        require_live || return 1
        services=$(timeout 5 "$ROOT/bin/qrtr-lookup") || return 1
        mapfile -t nodes < <(awk '$1 == 2 {print $4}' <<< "$services" | sort -u)
        if ((${#nodes[@]} == 1)) && [[ ${nodes[0]} =~ ^[0-9]+$ ]]; then
            URI="qrtr://${nodes[0]}"
            return 0
        fi
        ((${#nodes[@]} == 0)) || { fail 'Ambiguous DMS service; refusing to choose'; return 1; }
        sleep 1
    done
    fail 'No modem DMS service appeared'
}

qmi() {
    require_live || return 1
    printf 'QMI %s uptime=%s\n' "$*" "$(< /proc/uptime)" >&2
    timeout 22 "$ROOT/bin/qmicli-x55" -d "$URI" "$@"
}

start_modem() {
    local module unit
    [[ $(<"$RPROC/state") == offline ]] || {
        fail 'Modem is not offline; refusing to reset/restart it'; return 1;
    }
    [[ ! -e "$ATTEMPT" && ! -e "$EFS_STATUS" ]] || {
        fail 'This host boot already attempted X55; warm recovery is not validated. Use a clean host boot.'; return 1;
    }
    check_firmware || return 1
    check_loader || return 1
    for module in mdm_helper_native tqftpserv pd-mapper qrtr-lookup qmicli-x55 efs_sync.py; do
        [[ -x "$ROOT/bin/$module" ]] || { fail "Missing executable: $module"; return 1; }
    done
    for unit in x55-runtime.target "${SERVICES[@]}"; do
        [[ $(systemctl show "$unit" -p LoadState --value) == loaded ]] || {
            fail "Missing unit: $unit"; return 1;
        }
    done
    "$ROOT/bin/mdm_helper_native" --help | grep -q -- --check-alive || {
        fail 'Native helper does not support the required liveness check'; return 1;
    }
    printf 'disabled\n' > "$RPROC/recovery" || return 1
    [[ $(<"$RPROC/recovery") == disabled ]] || { fail 'Recovery could not be disabled'; return 1; }
    for module in mhi_sahara_modem qrtr_mhi mhi_net mhi_wwan_ctrl rmnet; do modprobe "$module" || return 1; done
    check_loaded_loader || return 1
    # Reserve this boot BEFORE starting any services; failures are not hidden
    # by automatic retry. /run disappears only with a normal host reboot.
    mkdir -m 0700 "$ATTEMPT" || return 1
    printf '%s\n' "$(< /proc/sys/kernel/random/boot_id)" > "$ATTEMPT/boot-id" || return 1
    systemctl start x55-runtime.target || return 1
    services_active || return 1
    systemd-run --unit=x55-rproc-start --no-block \
        /bin/sh -c 'printf "start\n" > "$1/state"' sh "$RPROC"
}

stop_modem() {
    if [[ $(<"$RPROC/state") == offline ]] &&
       systemctl is-active --quiet x55-rproc-start.service; then
        fail 'Firmware start is still pending; wait for the bounded boot result before stopping'; return 1
    fi
    # Keep RFS/EFS alive during normal shutdown so final synchronization can
    # finish. A failed stop does not silently remove those services.
    if [[ $(<"$RPROC/state") != offline ]]; then
        printf 'stop\n' > "$RPROC/state" || {
            fail 'Remoteproc stop failed; leaving services available for diagnosis'; return 1;
        }
    fi
    systemctl stop x55-runtime.target "${SERVICES[@]}"
}

online() {
    local i mode
    for i in {1..120}; do
        services_active || return 1
        if [[ $(<"$RPROC/state") == running ]] && efs_ready; then break; fi
        sleep 1
    done
    require_live || return 1
    select_node || return 1
    mode=$(qmi --dms-get-operating-mode) || return 1
    printf '%s\n' "$mode"
    case "$mode" in
        *"Mode: 'online'"*) ;;
        *"Mode: 'low-power'"*|*"Mode: 'persistent-low-power'"*|*"Mode: 'mode-only-low-power'"*|*"Mode: 'offline'"*|*"Mode: 'shutting-down'"*)
            [[ "$mode" == *"HW restricted: 'no'"* ]] || { fail 'Hardware restriction not clear'; return 1; }
            qmi --dms-set-operating-mode=online || return 1
            sleep 3
            ;;
        *) fail 'Unexpected operating mode; no state change requested'; return 1 ;;
    esac
    mode=$(qmi --dms-get-operating-mode) || return 1
    printf '%s\n' "$mode"
    [[ "$mode" == *"Mode: 'online'"* ]] || { fail 'Modem did not enter online'; return 1; }
}

main() {
    case "${1:-status}" in
        status)
            uname -r
            discover
            printf '%s (%s): software-state=%s recovery=%s\n' "$RPROC" "$ESOC_NAME" \
                "$(<"$RPROC/state")" "$(<"$RPROC/recovery")"
            "$ROOT/bin/mdm_helper_native" --check-alive || true
            systemctl --no-pager show "${SERVICES[@]}" -p Id -p ActiveState -p Result
            [[ ! -f "$EFS_STATUS" ]] || python3 -m json.tool "$EFS_STATUS"
            ;;
        helper)
            require_kernel || return 1
            discover || return 1
            exec "$ROOT/bin/mdm_helper_native" --debugfs-dir "/sys/kernel/debug/esoc_mdm_dbg_eng/$ESOC_NAME" \
                --startup-timeout-ms 120000 --boot-done --boot-done-timeout-ms 120000 --poll-ms 200
            ;;
        start|stop|online|probe)
            require_kernel || return 1
            discover || return 1
            lock_control || return 1
            case "$1" in
                start) start_modem ;;
                stop) stop_modem ;;
                online) online ;;
                probe)
                    select_node || return 1
                    qmi --dms-get-operating-mode || return 1
                    qmi --uim-get-card-status || return 1
                    qmi --nas-get-serving-system || return 1
                    qmi --wds-get-packet-service-status
                    ;;
            esac
            ;;
        *) echo "Usage: $0 {status|start|online|probe|stop}" >&2; return 2 ;;
    esac
}

# Sourcing exposes functions for isolated tests without running any operation.
if [[ ${BASH_SOURCE[0]} == "$0" ]]; then main "$@"; fi
