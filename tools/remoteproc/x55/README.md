# OnePlus 8T X55 / LTE bring-up (experimental)

[中文适配、当前配置与用户态使用说明](README.zh-CN.md)

This is an **experimental, partially hardware-tested** port. LTE IPv4 data has
passed one isolated short test; long-term reliability and 5G are not claimed.
Do not use it as the only bootable kernel. No call audio, satellite
proxy, IPA offload or NR validation is included. Sustained high-rate downloads
are known to wedge the Apollo reference implementation.

**Current hardware result (2026-09-29):** after the first sustained ONLINE/EFS
check, the r5 runtime completed an isolated CMNET IPv4 data call using QMAP,
rmnet and one persistent QRTR WDS client. DNS, ping and HTTPS worked. Domestic
8 MiB downloads measured **35.39 Mbit/s (Aliyun)** and **42.42 Mbit/s (Tencent)**;
host Wi-Fi routes/DNS were unchanged and concurrent gateway pings had no loss.
See [the data test](DATA-TEST-2026-09-29.md),
[initial stability test](TEST-2026-09-29-r5.md) and
[stock/Apollo comparison](COMPARISON.md) for exact scope and limits.
The data call was stopped cleanly; the modem remains ONLINE with manual
services active. Boot enablement and automatic recovery remain off. Historical
r4 entries below retain their original, earlier three-service results.

## Provenance and local adaptations

Modem code imported selectively from:

- https://github.com/royka1/apollo-linux
- source tip: `fd5210de352d` (apollo-6.17)
- delta base: `6b468be3533a`
- original Qualcomm/vendor copyright and license headers retained.

Includes ESOC, PMIC modem reset, split MHI/Sahara loading, external sysmon/PDR,
QRTR, WWAN and PCIe recovery. Deliberately excludes Apollo's display, power
supply, audio, IPA and satellite changes. Linux 6.16 PCIe helpers were adapted
to the single-PHY API, and reject non-SM8250 controllers/live-link resets.
Fusion selection requires the PCI endpoint's DT `esoc-0` property, rather than
replacing the Foxconn match for every device sharing subsystem ID 17cb:010c.
Removed generic-MHI diagnostic sleeps and unchecked PCI casts; extra boot EE
handling is gated on the fusion AMSS image. ESOC request-engine waits are
bounded and PMIC reset errors are propagated. Firmware paths consistently use
`qcom/sdx55m/`. Unknown Sahara image IDs fail instead of serving an arbitrary
image. These changes still need hardware regression tests (especially Wi-Fi).

Board wiring comes from OnePlus project 19805 `kona.dtsi`, `kona-mhi.dtsi` and
`kona-pmic-overlay.dtsi`. The normal `sm8250-oneplus-kebab.dtb` is unchanged:
all modem nodes are in **sm8250-oneplus-kebab-x55.dts**. Modem auto-boot is off.
The running 6.16.7-kebab+ kernel enumerated 17cb:0306 / 17cb:010c at
0002:01:00.0 (Gen3 x2), but its Foxconn configuration failed MHI power-up.

## Build (does not install or flash)

```sh
JOBS=4 tools/remoteproc/x55/build-kernel.sh
```

Reuses the existing `out-kebab/` config and object cache, merging only
`kebab_x55.config`. No clean, mrproper, or fresh output directory. The release
suffix follows the current config. Reuse the project's installer in its
**stage-only** mode:

```sh
DTB_NAME=sm8250-oneplus-kebab-x55 JOBS=4 \
  ./install-kebab.sh --skip-build --stage-only
```

This adds modules under a distinct release and prepares initramfs/boot images
under `out-kebab/boot-stage/`. It does not change existing `/boot` links, run
update-initramfs post-update hooks, write a boot slot, activate a slot or reboot.
No separate kernel installation script is maintained. Never load the new MHI
modules into the old kernel (the shared MHI API changed).

## Firmware / NV

Use **this phone's** stock `modem_a`/`modem_b` matching its software slot.
Read the filesystem read-only. Required images are in `image/sdx55m/`:
`sbl1.mbn`, `multi_image.mbn`, `multi_image_qti.mbn`, `xbl_cfg.elf`,
`qdsp6sw.mbn`, `apps.mbn`, `aop.mbn`, `tz.mbn`,
`hyp.mbn`, `devcfg.mbn`, `apdp.mbn`, `acdb.mbn`, `sec.elf`, `modemr.jsn`.

Install under `/lib/firmware/qcom/sdx55m/`. Copy `image/modem_pr/` into that
same directory for RFS. Never use Xiaomi firmware, NV, calibration or IMEIs.

The stock firmware filesystem contains **512-byte dummy efs*.bin files**.
Replace the *host copies* with complete backups of this phone's
`mdm1m9kefs1/2/3` (2 MiB each). Preserve originals and hashes outside the repo;
do not write back to raw partitions. An erased-looking third partition must
not be replaced with another device's data. Size checks alone do not prove
NV validity. Keep these private files out of git and diagnostic uploads.

On **OnePlus kebab**, the final stock loader is not just that base EFS map:
its OEM plugin replaces Sahara image 20 with `mdm1oemnvbktmp`. Stage a verified
read-only, own-device copy as `qcom/sdx55m/oemnvbktmp.bin` (2 MiB, image-20
header). Preserve `efs3.bin` and all original partitions unchanged. Do not run
the Android OEM preparation/backup binaries or fabricate replacement NV.
The board-scoped loader policy is identified by module version
`kebab-oemnv-1`; the controller rejects missing or stale modules before start.
[The comparison](COMPARISON.md) records the disassembly evidence and limits
of using the original cached OEM image rather than regenerating it.

The userspace PD mapper scans the firmware directory of every remoteproc.
Make sure the appropriate `adspr.jsn`, `adspua.jsn`, `cdspr.jsn` are also
available beside the **OnePlus ADSP/CDSP** firmware, not just in its parent
directory. The experimental DT switches the in-kernel PD mapper to userspace.

## Native userspace (staged, no Android daemons)

Initial bring-up uses explicit qmicli queries and a controlled normal ONLINE
power-up request, not ModemManager auto-probing.
This avoids the old ModemManager LOC assertion. It is not yet an automated
bearer manager. For eventual NetworkManager integration, use ModemManager
>=1.25 and review Apollo's two qcom-soc/PCIe bearer patches against that exact
version. QMAP/rmnet is mandatory for data testing; do not speedtest raw IP.

Sources used for the staged native binaries:

| Project | Revision |
|---|---|
| linux-msm/qrtr | 29e36ae164389580a0f8ea7a7fdb728140ae978d |
| linux-msm/pd-mapper | 5ecd2fe926aca7abfe40724177f63b942cff3947 |
| linux-msm/tqftpserv | c2559a26098f6d2b36a946ef0b6ad02223264c3e |

`tqftpserv-sdx55.patch` applies to that tqftpserv revision. It publishes
`qrtr_publish(fd, 4096, 3, 0)` (packed QRTR instance value 3), adds bounded
EAGAIN retries, fixes firmware-path buffer handling, and uses the private
`/var/lib/x55-tqftpserv` write directory. **Apollo's documented patch has the
opposite direction (3 -> 1); do not use it.** No raw NV block device is served.

Apply `tqftpserv-read-fixes.patch` **after** that patch. It repairs unlimited
reads, `rsize` boundaries, EOF/window termination, block rollover, empty-file
size negotiation and the no-options WRQ acknowledgement. It also checks short
reader packets and safely terminates reader error messages. These are verified
protocol fixes. A runtime test completed MCFG transfers with low FD usage but
still ERRFATALed; they have not established modem stability.
See [the offline regression tests](tests/README.md).

Native helper: `cc -O2 -Wall -Wextra -o mdm_helper_native
 tools/remoteproc/mdm_helper_native.c` (one shell command).

qrtr/tqftpserv use Meson; pd-mapper uses Make and libqrtr/liblzma. Build with
qrtr's pkg-config directory on PKG_CONFIG_PATH. Stage executables in
`/opt/x55/bin` and shared libraries in `/opt/x55/lib`. qmicli 1.35.2 plus the
matching Ubuntu arm64 libqmi/libqrtr-glib/libmbim libraries were extracted
into the private staging tree, **not installed via apt**. No proxy is required
for read-only QRTR queries. A future persistent bearer client must preserve
the same WDS session across endpoint binding and Start Network; do not assume
separate qmicli QRTR processes can retain a client merely by reusing a CID.

Install `modem.sh`, `efs_sync.py`, the updated native helper, all four
`x55-*.service` units and `x55-runtime.target` manually. Do not enable them:
they deliberately have no `[Install]` sections and use no automatic restart.
The EFS service saves bounded, immutable private snapshots in `/var/lib/x55-efs`,
never raw partitions or boot firmware. It is not yet a production NV archive.
Do not run qrtr-ns (the kernel already supplies the QRTR name service), rmtfs,
the OEM raw-partition backup daemon, or sysmon-probe-native.

## First boot acceptance checks

1. Keep a known-good boot image/slot and a recovery method. Boot the new
   Image, matching modules/initramfs and **X55 DTB together**.
2. Verify Wi-Fi, USB access, screen, charging and CDSP before touching modem
   state. Firmware/QMI control tests can run without a SIM; actual LTE data
   validation requires one.
3. Verify `qcom-sdx55m-fusion` selected, PMIC SID 8 PON probed, and an ESOC
   remoteproc exists. **remoteproc0 is CDSP on the original kernel**; never
   blindly write `remoteproc0/state`.
4. Once firmware/services are installed, explicitly run:

   ```sh
   tools/remoteproc/x55/modem.sh start
   journalctl -fu x55-mdm-helper.service
   dmesg -w
   ```

   The script discovers ESOC, checks firmware/loader prerequisites, disables
   recovery, starts the runtime target and submits one asynchronous start.
   It permits one attempt per host boot, refuses stale/already-running state,
   and does not silently reset or retry a failed modem.
5. Require Sahara image transfers and mission mode. Use the unified explicit
   power-up command, which waits for EFS channel readiness, checks the GPIO
   liveness state and discovers the modem QRTR node:

   ```sh
   tools/remoteproc/x55/modem.sh online
   ```

   The initial state was `shutting-down`, without hardware restriction; a
   normal ONLINE request initialized the SIM. This state string alone does
   not prove a host shutdown call. No RF activation occurs in `start` itself.
6. Query with the same guarded controller:

   ```sh
   tools/remoteproc/x55/modem.sh probe
   tools/remoteproc/x55/modem.sh status
   ```

   `probe` reads DMS operating mode, UIM status, NAS serving-system and WDS
   state. It checks real STATUS/ERRFATAL and runtime health before every QMI
   command, not just the potentially stale remoteproc software state.

   Do not print IMSI/IMEI/EFS in public logs. Require ONLINE, ready USIM,
   confirmed NAS registration and no ERRFATAL for at least 10 minutes before
   data testing. LTE service availability alone does not prove a bearer works.
   Band/network-mode overrides and APN selection remain deferred.
7. Next milestone: persistent QMI WDA/WDS client, correct PCIe endpoint,
   QMAP mux and rmnet child interface; route only test traffic through LTE.
   Do not replace the SSH/default route. First verify low-rate IPv4 data, then
   longer transfers and recovery. No 5G or voice claim until separately tested.

## Private staging on this device

- NV backups: `/root/x55-private/nv-before-x55-20260928/` (SHA256SUMS included).
- Firmware/native programs: `/root/x55-private/stage/`.
- Build output/cache: `out-kebab/` (compiled objects from the unnecessary
  initial `out-x55/` build were merged back without discarding the cache).

At the initial staging checkpoint only the distinct
`6.16.7-kebab-x55-r1+` module release had been installed. The following runtime
entries supersede that checkpoint: firmware and native programs are now live
under `/lib/firmware` and `/opt`, and A has been flashed with explicit approval.
Modem-related services remain disabled at boot.

### Validation, 2026-09-28

- Full kernel/modules/X55 DTB build passed. After merging the completed cache
  into `out-kebab`, the final incremental build recompiled only three changed
  driver sources plus the version timestamp and relinked successfully, with
  no compiler warnings in that incremental log.
- Original `install-kebab.sh --skip-build --stage-only`, with
  `DTB_NAME=sm8250-oneplus-kebab-x55`, succeeded.
- Prepared `out-kebab/boot-stage/armbian-kernel-6.16.7-kebab-x55-r1+-slot_a.img`
  (45,654,016 bytes). Header v0, embedded Image.gz + X55 DTB, matching initramfs
  and 96 MiB slot size were checked. SHA256 file is beside the image.
- MHI net, QRTR MHI, rmnet and ath11k module vermagic matches the test release.
- NV backup hashes verified; the read-only modem_a mount was unmounted.
- Shell syntax checks, invalid installer-option checks, helper --help and
  qmicli --version passed. Git diff whitespace checks passed.
- Build-time status: boot, mission mode, SIM, LTE bearer and recovery were
  not yet tested at that point.

### First runtime attempt, 2026-09-28 23:43–23:48 CST

- Booted `6.16.7-kebab-x55-r1+` successfully from A. Wi-Fi connected; no
  kernel crash/pstore record. The original A backup matches the pre-flash
  hash, and B still matches its pre-flash hash.
- Native programs, firmware copies, PD JSONs and static service units are now
  installed. No unit is enabled for automatic startup. Userspace PD mapper
  and TFTP publish their expected QRTR services.
- PMIC SID 8 PON and ESOC probe, fusion PCI selection, warm reset, SBL upload,
  MHI SBL transition, boot-log channel and Sahara HELLO all worked.
- **Blocked:** the first Sahara request is image ID **37**, offset 0, length
  52. The strict image map lacks this ID, so no subsequent firmware is sent.
  Need to establish the correct OnePlus image mapping before another attempt.
- No mission mode or DMS service appeared; IMEI was not read and no cellular
  registration/data tests were performed. Stopped the helper retry and sent
  BOOT_FAIL to terminate the pending start; ESOC remoteproc is offline.
- Private logs: `/root/x55-private/logs/first-modem-start-*.txt`.
- All further builds must remain incremental in `out-kebab/`.

### r2: incorrect inferred XBLConfig mapping / modular loader

**Historical failed mapping; corrected below from the stock userspace loader.**
Read-only analysis of this phone's SBL ELF found a boot-table record with
image type 0x25 (37), partition name `xbl_config`, label `XBLConfig` (name
pointer at file offset 0x37e7c). `xbl_cfg.elf` is present in the original
OnePlus modem firmware. Added an explicit 37 -> xbl_cfg.elf mapping and the
35 -> multi_image_qti.mbn entry; unknown IDs still fail closed. Do not restore
Apollo's arbitrary fallback of every unknown ID to qdsp6sw.mbn.

Sahara is now a module, explicitly loaded by modem.sh; subsequent loader-only
fixes can be tested without changing the MHI core. The helper no longer
restarts automatically after a failed boot, waits up to 120 seconds and sends
BOOT_FAIL on timeout to release the pending remoteproc start.

r2 incremental build passed (11 C compilations, no warnings). The existing
installer wrote and read-back verified A; its complete r1 backup matches the
pre-write partition hash and B is unchanged. Image:
`/boot/armbian-kernel-6.16.7-kebab-x55-r2+-slot_a.img` (45,674,496 bytes).
r2 was subsequently booted. It transferred the image-37 ELF header, program
headers and hash segment, then SBL reported `100302e at sbl1_sahara.c Line 601`.

### Controlled r2 failure and r3 host-safety changes

A controlled attempt with no user/agent reboot reproduced a whole-AP reboot.
Fsynced `/dev/kmsg` log: `kmsg-cadff326-af92-4012-b4ea-014a933a89b2.log` in the
private logs directory. The last record is the automatic post-BHI PCIe retrain
at uptime 437.37 s, following the SBL error at 435.82 s. This strongly implicates
that recovery path; it is not proof of a specific exception or reset mechanism.
Do not keep reproducing this failure with r2.

r3 disables fusion early generic recovery and scheduling of the post-BHI
retrain worker. MSI polling stops when PCI config space becomes unreachable,
instead of continuing endpoint BAR reads. Explicit ESOC recovery is still
unvalidated; do not claim SSR is working on this device.

Read-only analysis of this exact SBL identifies line 601 at 0x1483bf6e. It
combines BOOT error group 0x01000000 with the result of 0x148219c8 -> 0x14825aac.
That wrapper returns the generic 0x302e if the metadata-verification call
(SMC function 0x82000210) fails. It does not expose the underlying reason.
Thus the error alone does NOT establish a wrong image ID/signature. The
original XBLConfig ELF+program-header SHA-384 matches its own hash segment;
its metadata SW_ID is 0x25. These are useful consistency checks, not proof that
Sahara's image-number mapping is correct or that the modem accepts the image.
No firmware authentication bypass or NV/IMEI modification has been attempted.
Research snapshots: `/root/x55-private/research-r3/`.

r3 incremental build passed (10 C compilations, no warnings). The original
installer wrote/read-back verified A, verified its r2 backup against the
pre-write hash, and verified B unchanged. r3 image is
`/boot/armbian-kernel-6.16.7-kebab-x55-r3+-slot_a.img` (45,674,496 bytes).
r3 subsequently booted and reproduced the metadata failure. The endpoint
became unreachable, MSI polling stopped, and the AP stayed up with the same
boot ID for more than 15 minutes. The helper was stopped and BOOT_FAIL released
the pending start. This verifies the fail-closed behavior for this one failure,
not general modem recovery. Durable log:
`/root/x55-private/logs/kmsg-8dd08c09-71a8-4295-ba36-dd0bfcc3fad7.log`.

### Stock Sahara wire mapping recovered, 2026-09-29

Read the super partition's checksummed LP metadata and exposed vendor_a with a
**read-only** loop device, then extracted files using read-only debugfs. No
Android binary was executed, and the loop device was detached afterwards.
`/vendor/lib64/libmdmimgload.so` contains explicit SDX55M `ID:path` strings:

| Wire ID | Host firmware file under qcom/sdx55m/ |
|---|---|
| 6 | apps.mbn |
| 8 | qdsp6sw.mbn |
| 16 / 17 / 20 | efs1.bin / efs2.bin / efs3.bin (base map used by this r4 test; image 20 later corrected by the OnePlus OEM override, see COMPARISON.md) |
| 21 | sbl1.mbn |
| 23 / 25 / 29 / 33 | aop.mbn / tz.mbn / acdb.mbn / hyp.mbn |
| 34 | mdmddr.bin (private copy of the phone's raw mdmddr partition) |
| 36 | multi_image_qti.mbn |
| 37 | multi_image.mbn (controller amss_image) |
| 38 | xbl_cfg.elf (stock loader calls it xbl_config.elf) |
| 40 / 41 / 42 | apdp.mbn / devcfg.mbn / sec.elf |

**SBL security SW_ID is not a Sahara wire ID.** The earlier 37 -> XBLConfig,
35 -> multi_image_qti and 21 -> qdsp6sw mappings were wrong for this device.
The strict loader now uses the stock map and still rejects unknown IDs.
Raw payload hex dumps were downgraded to debug logging.

Stock-loader reference files are private at
`/root/x55-private/vendor-loader-reference/`; libmdmimgload.so SHA-256:
`b62218dcacca19d680404e2da5a747ef62b03a42280c100f111a295084ea910a`.
The 1 MiB mdmddr backup, verified against the raw read-only source, is at
`/root/x55-private/mdmddr-before-sahara-20260929/` with SHA256SUMS. Its host
firmware copy is mdmddr.bin; the original 40-byte mdmddr.mbn placeholder and
all raw NV partitions remain unchanged.

The user independently built/flashed r4 while the AP was running r3. Their
changes/version were preserved. Only Sahara was incrementally rebuilt in
out-kebab (one C compilation), producing matching r4 vermagic, and installed
under `/lib/modules/6.16.7-kebab-x55-r4+/`. The old r4 module is backed up under
`/root/x55-private/module-backups/`. A was read-only compared with the existing
r4 boot image and already matches; no additional slot flash was performed.
The subsequent r4 runtime attempt below used that rootfs module (hash checked,
not preloaded from initramfs).

### r4 runtime: successful mission mode, then modem-only failure

First start in boot `26777b13-9de1-4423-9605-56b0d3dc2a61`:

- Corrected Sahara transfers completed, including the 79.8 MB Q6 firmware
  and APPS. MDM2AP_STATUS rose at uptime 107.73 s; MHI entered mission mode
  at 108.20 s and ESOC reached running at 108.29 s.
- QRTR exposed SDX55 application services on node 2 and modem services on
  node 3, including DMS, UIM, NAS, WDS and WDA. `mhi_swip0`, `mhi_hwip0` and
  the WWAN control/DIAG ports appeared.
- A read-only DMS identifier query succeeded. Private identifiers are not
  recorded here. Subsequent operating-mode/card-status queries timed out;
  later sends returned EAGAIN. No registration or bearer success was obtained.
- TFTP served many original MCFG files; the last logged request was at
  uptime 181.27 s. MDM2AP_ERRFATAL fired at 216.56 s (about 108 s after
  mission mode), followed by STATUS low. Cause is not established. Do not
  conflate this firmware failure with the earlier wrong Sahara mapping.
- RDDM download timed out and no restart reason was retrieved. Post-failure
  MHI counters were M0=1, M2=0, M3=0; no MHI sleep transition was observed.
- The AP stayed up with the same boot ID. Recovery was disabled before ESOC
  attempted SSR. A software remoteproc 'running' state after such a failure
  is not proof that the modem is still operational.

The user inserted a SIM after the first failed attempt. One explicit modem-only
stop/restart was tried, without an AP reboot. It exercised SPMI hard reset,
PCIe link reconstruction/re-enumeration, and again completed all Sahara images.
However the endpoint disappeared after SBL finished (uptime 1049.50 s), before
mission mode/STATUS high. Live PCI vendor/device reads were both ffff. The
120-second helper timeout returned the remoteproc start to offline. **This
attempt does not establish whether the inserted SIM works.** Warm recovery
is still unvalidated and is not a reliable substitute for a clean host boot.

`modem.sh` now validates the private 1 MiB mdmddr.bin copy and disables
remoteproc automatic recovery before each manual start, in addition to the
kernel's early-recovery guard. Shell syntax and diff whitespace checks passed.
At this checkpoint helper/TFTP/PD-mapper are stopped, recovery is disabled,
and ESOC is offline. No kernel rebuild, slot flash, route changes or raw NV
writes were made in these runtime tests. LTE connectivity remains unverified.

Private evidence:

- `logs/kmsg-26777b13-9de1-4423-9605-56b0d3dc2a61.log`
- `logs/r4-tqftpserv.log`, `logs/r4-helper.log`, `logs/r4-pd-mapper.log`
- `logs/r4-*-after-warm-retry.log`
- `logs/mhi-observe-26777b13-9de1-4423-9605-56b0d3dc2a61-1790615889.jsonl`

Paths above are relative to `/root/x55-private/`. The bounded private
`observe-mhi.py` records cached MHI states and reads DIAG only when a port
appears; its DIAG file was empty in the unsuccessful warm restart. Next test
should use a clean host boot with SIM already inserted and collect these
observations from the first mission-mode transition, before further bearer work.

### Clean r4 boot with SIM, 2026-09-29

Boot `354eda4e-e23f-4b99-b71c-4190fb68d8db` again reached mission mode
(uptime 111.69 s). Early UIM card-status reported no initialized logical cards;
a later physical-slot query successfully detected a present, active UICC in
physical slot 1, mapped to logical slot 1, with zero applications yet. Slot 2
was absent. Subscriber/card identifiers are deliberately omitted here.

Two DMS operating-mode queries returned **shutting-down**, hardware restricted
**no**. No ONLINE request or data-call request was sent in this attempt.
ERRFATAL followed at 211.81 s, about 100 s after mission mode. The AP boot ID
did not change, and automatic recovery remained disabled. MHI stayed in M0
with M2/M3 counters zero; pending transfers continued completing until the
failure, then accumulated. Opening the DIAG port produced no captured bytes.
This is not yet a successful SIM initialization or network-registration test.

The returned power state alone does not prove the host sent a shutdown request.
SSCTL message IDs and SSR event values match the original vendor kernel.
The system qrtr-ns process is dormant because the kernel name server already
owns its address; it was not modified. Upstream ModemManager maps both offline
and shutting-down to POWER_STATE_OFF and uses DMS ONLINE for normal power-up:
https://github.com/linux-mobile-broadband/ModemManager/blob/main/src/mm-broadband-modem-qmi.c
A reference snapshot is under `/root/x55-private/control-reference/`.
This supports testing the standard ONLINE transition, but does not establish
that it fixes this modem's crash.

Prepared (not yet run) `/root/x55-private/cold-test-online-once.sh`: invoke only
on the next explicitly authorized clean host boot. It uses the existing
modem.sh, captures kernel/DIAG/state logs, traces sysmon/SSCTL calls in its own
ftrace instance, and enables only QRTR packet metadata logging (not payload/ID
dumps). It makes at most one normal ONLINE request for a recognized off/low
power state without hardware restriction, then checks mode/card/system status.
It stops on errors; it does not start a bearer, change routes or write raw NV.
Trace setup was tested independently and shell syntax validation passed. Its
trace instance stops automatically after 300 s; the script is not a boot service.

Evidence is in `logs/kmsg-354eda4e-e23f-4b99-b71c-4190fb68d8db.log`,
`logs/sim-cold-*-354eda4e.log`, and the matching `logs/mhi-observe-354eda4e*`
files under `/root/x55-private/`. At this checkpoint ESOC is offline and the
three manual X55 services are stopped. No additional reboot was scheduled.

### ONLINE request and offline TFTP repair, 2026-09-29

On the next user-performed clean boot,
`9a5a007f-3c7d-4a10-9816-573749baba42`, the single ONLINE request succeeded.
DMS changed from shutting-down to online, slot 1's USIM became ready with
primary GW provisioning, and NAS system-info reported available, non-roaming
LTE service on PLMN 460-00. No data call was made. A later serving-system
query timed out, so registration was not separately confirmed.

This did **not** fix stability: mission mode occurred at uptime 136.57 s and
ERRFATAL at 239.21 s (about 103 s later). AP boot ID stayed unchanged; MHI
M2/M3 counts were zero and DIAG remained empty. The full 300-second SSCTL trace
contained all 54 recorded events, with no sysmon_stop/ssctl_request_shutdown
call during the test. The returned initial power state therefore must not be
presented as evidence of a host shutdown request. Logs are under
`/root/x55-private/logs/`, keyed by this boot ID.

Offline tests against the exact deployed tqftpserv source found reproducible
reader faults. Without `rsize`, reads stopped after their first block; at an
exact block-sized nonzero limit, the server sent data outside the requested
range. EOF/window handling and 16-bit rollover were also incorrect. The
original code passed only 2/9 initial cases. The new separate patch passes
15/15 expanded tests with ASan/UBSan/leak checks, including actual request
handlers over isolated AF_UNIX sockets and synthetic memfds. Patch application
was round-trip verified byte-for-byte. The modified server translation unit
also builds with `-Wall -Wextra -Werror`; an existing signedness warning in
unmodified zstd-decompress.c remains outside this fix.

Only the private userspace server was replaced; kernel, slots, firmware and
raw NV were unchanged. The original executable, private RFS-directory snapshot,
source and test results are preserved in
`/root/x55-private/tqftp-read-fixes-20260929/`. Installed server SHA-256:
`5d7f46de849b4ff7220a0960d161532c788a10a64416942cad35628847c9eb91`.
**The corrected server has not yet been exercised against the modem.**

ESOC and all three manual services are now stopped. The next explicitly
invoked clean-boot test can run the existing private script with
`X55_TFTP_DEBUG=1`: it adds a runtime-only service override for TFTP request
metadata, a bounded persistent userspace journal capture, per-second server
FD/RSS counts, and prompt registration/WDS/WDA queries. There is still no
automatic boot start or bearer activation, and no new reboot has been scheduled.

### Confirmed LTE registration, still runtime-unstable, 2026-09-29

Clean boot `fcacaa9b-5eec-4c7a-89b2-4e6062118188` used the repaired TFTP
server, unchanged r4 kernel/Sahara module and the same original phone firmware.
Mission mode was reached at uptime 113.62 s; ONLINE succeeded at 116.06 s.
At 119.34 s NAS serving-system explicitly reported **registered**, **CS and
PS attached**, **LTE**, operator **CMCC (460-00)** and no roaming. USIM was
ready. WDS reported disconnected: no bearer or data traffic was started.

The unqualified WDA format query returned QMI InvalidArgument (48), not a
transport timeout. This requires a verified transport endpoint, so it has been
removed from the automatic private observation sequence rather than retried
with guessed endpoint numbers.

ERRFATAL nevertheless occurred at 242.59 s, about **129 s after mission mode**.
The last logged MCFG transfer, CMCC Volte_OpenMkt, completed with its final
ACK at 205.44 s. The roughly 905 KiB private userspace log records 860 read
requests; the reported file errors were the two attempts to read the absent
`ota_firewall/ruleset`. No fake ruleset was created. Sampled TFTP FD counts
stayed between 4 and 8; RSS at the final sample was 1824 KiB. The repair did
not establish stability, and descriptor exhaustion was not observed.

The AP boot ID did not change. RDDM failed again, DIAG stayed empty, MHI M2/M3
counters remained zero, and the full 54/54-event SSCTL trace contained no host
shutdown call. Modem MHI MSI counts were zero, with the existing poll fallback
active; its source has no elapsed-time cutoff. This observation alone does
not identify the crash mechanism.

A separate runtime dependency remains unimplemented: **EFS synchronization on
MHI channel 10**. This boot created wwan0efs0, while the native helper contains
no EFS-sync implementation. The actual OnePlus stock mdm_helper contains the
EFS-sync process/KS diagnostics and pipe_10 paths. Apollo also provides
`tools/remoteproc/mhi_efs_sync.c`, but its README does not establish that this
service is necessary to prevent our crash. Treat it as the next candidate,
not a confirmed root cause.

Reference source/service files are saved under
`/root/x55-private/runtime-reference/`; no vendor executable or EFS writer was
run. The reference daemon has a raw-partition-write option and lacks several
bounds/path/deadline checks, so it must not be launched unchanged. Any follow-up
implementation must be limited to bounded, new private regular-file snapshots;
it must not write raw partitions or replace the original boot EFS images.

At this checkpoint the modem and three manual services are stopped, automatic
recovery remains disabled, and no further reboot has been scheduled. Evidence
includes `online-once-<boot-id>.log`, `userspace-<boot-id>-*.log`, the corresponding
MHI samples/SSCTL trace, and `rfs-fixed-services-<boot-id>.log` in the private
log directory.

### Unified runtime staged after stock comparison, 2026-09-29

The subsequent stock-binary audit identified the image-20 OEM mapping hook
and EFS synchronization as distinct omissions. See [COMPARISON.md](COMPARISON.md)
for exact evidence, intentional deviations from raw-writing Android services,
and the implemented safeguards. The original firmware/NV copies remain intact.

The user's already prepared r5 image matches A. Only the Sahara module was
rebuilt incrementally in `out-kebab` and installed for that r5 release; no
kernel image/DTB/slot was changed by this work. The running r4 remains stopped.
The EFS service, native GPIO liveness query and unified manual controller are
installed, but all services remain inactive/static. A marker prevents a warm
retry in this already-used boot. No reboot is scheduled.

Offline validation: 33 new EFS/lifecycle/mapping tests plus the 15 existing
sanitized TFTP tests passed; module/helper compilation, service verification
and shell/Python syntax checks passed. Backups and results are in
`/root/x55-private/unified-runtime-20260929/`. The next clean-boot test is still
required before judging whether the runtime crash is resolved.

