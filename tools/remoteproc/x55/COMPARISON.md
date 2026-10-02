# X55: stock / Apollo / kebab comparison and unified runtime

Status: the unified r5 runtime has now passed its first >=10-minute
ONLINE/ready-USIM/CMCC-LTE-registration test without ERRFATAL, including two
completed EFS snapshot sessions. Final queries stayed healthy after over 14
minutes ONLINE. See [the hardware test record](TEST-2026-09-29-r5.md).
**Follow-up:** a persistent PCIe/QMAP WDS client subsequently completed an
isolated IPv4 data test and clean stop, with domestic CDN downloads and no
host Wi-Fi routing/DNS changes; see [the data test](DATA-TEST-2026-09-29.md).
Long-term reliability is still unproven. Earlier r4 tests registered on CMCC
LTE but crashed after 100–129 seconds.

## Evidence and decisions

| Layer | Stock OnePlus / reference | Earlier local implementation | Current decision |
|---|---|---|---|
| Sahara boot | SDX55M base mapping from `libmdmimgload.so`, then an OEM mapping hook | Corrected wire IDs, but only the base NV mapping was applied | Keep the verified wire IDs; apply the OnePlus-only image-20 override |
| Image 20 | `oemnvbk_add_map_config(..., 40, 1)` replaces every `20:` argument with `20:/dev/block/bootdevice/by-name/mdm1oemnvbktmp` | Served the original, erased-looking `mdm1m9kefs3` copy | Serve a byte-for-byte **own-device cached OEM image** from `qcom/sdx55m/oemnvbktmp.bin`; retain EFS3 unchanged |
| EFS boot vs runtime | Boot images provide initial NV; after boot `ks -m` services MHI pipe 10 | Boot images were provided, but no process opened EFS | Add a file-only EFS protocol service; these are separate responsibilities |
| OEM backup daemon | OnePlus starts `oemnvbkdaemon`; it watches local NV files with inotify and calls the backup library | Not implemented | Do not run it: it writes raw backup partitions. Archive runtime files instead; no claim of reproducing Android's backup/regeneration policy |
| TFTP/RFS | Stock firmware uses service 4096, version 3, instance 0 (packed instance 3) | Correct publication and paths; upstream reader bugs subsequently found | Keep both tested patches. Runtime FD counts 4–8 and completed MCFG transfers did not eliminate the crash |
| PD mapper | Required for secure-NV/service registry; correct firmware domain JSONs | Private mapper and modem/ADSP/CDSP JSONs installed | Keep shared discovery; verify it is active as part of the runtime group |
| ESOC helper | Vendor polls status, acknowledges boot, supervises EFS child | Native debugfs request engine acknowledged boot but was a separate lifecycle | Retain native engine; discover ESOC by name; add notification-free GPIO liveness query |
| Radio power | Android/MM performs normal ONLINE transition | Initially only read-only queries; later single ONLINE was successful | Explicit `online` command, never implicit at boot; no hardware-restriction override |
| MHI channels | Vendor EFS UL10/DL11: 64 elements, event ring 1, AMSS, DL wake-capable | This topology and EFS WWAN binding already present; QMI1/IP_CTRL keepalive bound | No kernel channel changes needed for EFS. Preserve existing working QRTR topology |
| Interrupt/PM | Current port uses polling under a DMA IOMMU domain | MHI MSI counters zero; continuous polling; no M2/M3 in failures | Keep this explicit known limitation. Poller has no 100-second cutoff; do not speculate that identity IOMMU or PM changes fix it |
| Recovery | Stock and Apollo attempt warm recovery/SSR | Kebab early recovery previously reset the AP; warm modem retry also failed | Preserve fail-closed kernel guards and disabled ESOC recovery; controller permits one start per host boot |
| Experimental reference daemons | PM shim, IPA proxy, SSCTL probes, DIAG/EFS writers coexist in Apollo tools | Not installed as requirements | Do not blanket-enable them. Acknowledge only protocols actually implemented; no fake restart/shutdown success |
| Data | Persistent WDS session, correct PCIe endpoint, QMAP/rmnet | Initially transient queries and an unqualified WDA GET | PCIe type 3/interface 4 verified; QMAP mux 1 and persistent WDS client passed an isolated IPv4 transfer/stop test; no default-route takeover or production MM integration |

### Stock binary evidence, not just strings

The read-only reference copies are in
`/root/x55-private/vendor-loader-reference/`. No Android executable was run.

- `mdm_helper`: argv construction at `0x4aec..0x4b6c`, fork at `0x4d44`,
  execve at `0x4d78`. Arguments are `ks -m -p <EFS device> -w <destination>
  -t -1 -l [-g <prefix>]`; the stock destination is the block-label directory.
- `libmdmimgload.so`: model-gated calls at `0x23ac` (mapping hook) and
  `0x2490` (prepare OEM temporary image), including SDX55M/SDXPRAIRIE.
- `liboemnvbk_img_helper.so`: `oemnvbk_add_map_config` implementation at
  `0x4e48`. The replacement branch compares the first three bytes against
  `20:` and substitutes the OEM temporary-image path. This is an effective
  override, not another Sahara wire ID or an SBL security SW_ID.
- The OnePlus post-boot path calls `setup_oemnvbk_daemon_v2`. The extracted
  16 KiB daemon uses inotify/poll and `oemnvbk_sync_partition_v2`; it has no
  QMI/socket transport imports. It is a separate host backup-maintenance
  layer, not a reason to start an unimplemented QMI shim.

OEM temporary/static/dynamic partitions were copied read-only, compared to
source and hashed under `/root/x55-private/oemnv-before-unified-20260929/`.
The existing 2 MiB temporary image has the stock image-20/version-3 header
and an in-bounds payload. Only that existing opaque image is staged; it is
**not reconstructed or edited**. Initial EFS1/2/3 and DDR host files still
match the original backups. This is not a full validation of NV internals.

The loader override is restricted to `oneplus,kebab` plus the Fusion
controller. Other machines keep image 20 -> EFS3. Missing OEM firmware and
unknown wire IDs fail closed. Module version `kebab-oemnv-1` lets the controller
reject stale rootfs or preloaded initramfs modules before issuing a start.

## Unified manual lifecycle

All runtime services belong to `x55-runtime.target`; none has an Install
section or automatic restart. The target alone **does not boot the modem**.

```sh
/opt/x55/bin/modem.sh start    # prerequisites, recovery off, services, one async boot
/opt/x55/bin/modem.sh online   # explicit RF activation, waits for runtime readiness
/opt/x55/bin/modem.sh probe    # mode, UIM, NAS registration, WDS state; no bearer
/opt/x55/bin/modem.sh status   # software state + GPIO liveness + service/EFS status
/opt/x55/bin/modem.sh stop     # stop modem first, then all four services
```

The four services are ESOC helper, TFTP, PD mapper and EFS sync. Startup loads
WWAN before launching EFS, whose Type=simple process waits up to 120 seconds
for the correct port without blocking remoteproc startup. QRTR nodes and ESOC
remoteproc numbers are discovered, not assumed. `--check-alive` only reads
STATUS/ERRFATAL ioctls; it sends no ESOC notifications. Each control query
requires healthy GPIO levels and active runtime services. Failure stops the
query sequence, not the phone, and does not trigger a retry/reset.

`/run/x55-modem-attempt` and preserved EFS status prevent repeated starts on
one host boot. Do not bypass them to test known-unreliable warm recovery.
Services stay available during normal shutdown for final filesystem activity;
a failed shutdown does not silently kill the filesystem services.

## EFS service safety and limits

`efs_sync.py` implements only Sahara memory-debug on a validated Fusion WWAN
**EFS** port. It has no raw destination, arbitrary device, partition-write,
image-upload or execute-command option. The service's device policy also
denies block devices.

- Recognized region basenames: `m9kefs1/2/3`, `m9kefsc`, `oemnvbktmp`.
  Limits follow this phone's partition sizes: 2 MiB, except 4 KiB for the cookie.
- Validate the complete 32/64-bit region table, names, counts, duplicate or
  overlapping ranges and overflow before creating any region output.
- Reads are at most 8 KiB. Exchanges have 10-second deadlines; a complete
  session has a 60-second absolute deadline, not a resettable per-byte timer.
- Snapshot root `/var/lib/x55-efs` is private. Each run/session gets a new
  directory; files are 0600, exclusive, no-follow, regular files. Existing
  files and partial results are never overwritten or removed.
- Region data, hash manifest and directory are fsynced **before** the normal
  EFS-session Sahara RESET/RESET_RESP completion exchange. No reset is sent
  after read/validation/storage failure. This protocol completion is not an
  ESOC reset, PCI reset or host reboot command.
- Process limits: 128 MiB of declared snapshot data, at most 64 sessions.
  Quota exhaustion stops with an explicit failure, never a fake persistence
  acknowledgement. This is a bounded bring-up service, not yet a production
  archival/garbage-collection policy.
- `/run/x55-efs/status.json` distinguishes waiting, channel-ready, saving,
  failed and stopped. Channel-ready alone does not prove an EFS session
  completed. `manifest.json` means data persisted; `complete.json` also means
  the target returned the expected completion response.

No snapshot is copied into boot firmware automatically. Original raw NV and
boot EFS files remain untouched; changed runtime NV is intentionally not yet
replayed on subsequent boots. Unsupported table variants are diagnostic
failures requiring review, not invitations to broaden memory access blindly.

## Verification and deployment checkpoint

- 22 isolated EFS tests: synthetic AF_UNIX peers, 32/64-bit sessions, OEM
  region, durable-output ordering, limits, malformed input, partial reads,
  short writes, timeout, storage failure, path/device checks and discovery.
- 10 lifecycle tests: fake sysfs/services/QMI, ESOC discovery, one attempt,
  stale-loader guards, no QMI after ERRFATAL/EFS failure, explicit ONLINE,
  hardware restriction and shutdown ordering.
- 1 compiled test of the actual Sahara mapping function, including board
  scoping, unchanged IDs and unknown-ID rejection.
- Existing 15 TFTP tests still pass with ASan/UBSan/leak detection.
- Native helper and module compiled without warnings; service verification
  and shell/Python syntax checks pass. No test contacts a physical modem.

The running kernel at staging is r4. The user's existing A image is r5 and
was compared read-only to its prepared image. Only Sahara and its module
metadata were rebuilt incrementally in **out-kebab**, then the matching r5
rootfs module was installed; no new build tree, kernel image, slot write,
slot switch or reboot. Original binaries/units/module and test logs are in
`/root/x55-private/unified-runtime-20260929/`.

The planned user-performed r5 clean boot and 900-second capture completed;
the >=10-minute control-plane stability gate passed. A later authorized data
test verified PCIe endpoint 4, QMAP/rmnet, a persistent WDS socket and an
isolated CMNET IPv4 call. It stopped cleanly without changing root Wi-Fi
routes/DNS. These successes do not replace long-duration, peak data-load,
suspend, modem warm-recovery or full connection-manager validation.
