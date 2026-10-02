# OnePlus 8T（kebab）X55 适配与用户态使用说明

> 更新：2026-09-29；以本机 `6.16.7-kebab-x55-r5+` 的实测配置为准。
> 这是实验性适配，**已打通 LTE IPv4 数据，不等于长期可靠、5G、通话和休眠恢复均已完成**。

## 1. 目前做到哪一步

- 原厂固件加载、正常 ONLINE、主卡 USIM 就绪、移动 LTE 注册均已成功。
- 加入运行期 EFS 同步后，跨过了此前约 100–129 秒的 ERRFATAL 窗口；持续状态检查和后续数据测试未再观察到崩溃。
- 使用 **PCIe + QMAP/rmnet + 同一持久 WDS 客户端**，完成 IPv4 拨号、DNS、ping、HTTPS 下载和正常断开。
- 国内 CDN 各下载 8 MiB：阿里云 **35.4 Mbps**，腾讯云 **42.4 Mbps**。这是短时单连接成绩，不是持续峰值。
- 测试流量隔离在独立网络命名空间；Wi-Fi 默认路由、路由规则、DNS 未变，下载期间 Wi-Fi 网关监测 30/30 响应。

**文档编写时的状态：**调制解调器仍 ONLINE，四个运行服务仍 active；测试数据连接已断开。`x55-data` 命名空间保留，里面的两个数据接口均 DOWN。重启后这些运行状态需要重新检查，不能据此假定开机就能上网。

## 2. 适配的基本思路

8T 的 SM8250 搭配的是 **外置 X55**，不是直接套用片上 modem/IPA 的启动流程：

```text
ESOC / PMIC / GPIO 控制上电与状态
  → PCIe / MHI / Sahara 加载原厂固件和本机 NV
  → QRTR 提供 QMI 控制服务，RFS / PD mapper / EFS 维持运行
  → DMS ONLINE → UIM 就绪 → NAS 注册
  → WDA 配置数据格式 → WDS 绑定、拨号
  → mhi_hwip0 → QMAP mux 1 → rmnet → IPv4 网络
```

内核能力选择性参考 Apollo（小米 10T）的实现，但**板级接线、固件、NV 和 OnePlus 的 OEM 行为按本机原厂资料处理**，没有整体替换成 Apollo 内核。

### 必须记住的适配节点

| 节点 | 本次实际处理 |
|---|---|
| 板级与内核 | 单独增加 `sm8250-oneplus-kebab-x55.dts`，保留普通 kebab DTS；按 OnePlus 19805 的 GPIO、PCIe2、PMIC SID 8/PON `0x800` 接线。移植 ESOC、MHI/Sahara、QRTR、WWAN、rmnet 等，适配 6.16 的 PCIe API。只有带 `esoc-0` 的端点走 Fusion 路径，避免误用普通 Foxconn 配置。 |
| 固件编号 | 从本机原厂 `libmdmimgload.so` 恢复 **Sahara 线上编号**，不能拿签名中的 SW_ID 推断。关键对应：8→Q6，34→本机 DDR 副本，37→multi_image，38→XBLConfig。未知编号拒绝加载，不随便兜底。 |
| OnePlus OEM 覆写 | 原厂插件还会把 **镜像 20 从基础 EFS3 映射改为 `mdm1oemnvbktmp`**。本机现在使用其原始内容的只读主机副本 `oemnvbktmp.bin`；原来的 `efs3.bin` 和原分区保留。该行为仅作用于 kebab/Fusion。 |
| RFS 与服务发现 | TFTP 发布 service 4096/version 3/instance 0；依次应用 `tqftpserv-sdx55.patch` 和 `tqftpserv-read-fixes.patch`，修复发布参数、无限读取、边界、EOF、窗口等问题。PD mapper 需要正确的 modem/ADSP/CDSP JSON。 |
| 运行期 EFS | **启动时提供 EFS 文件，不等于运行期同步已完成。** 原厂在 MHI 10/11 通道运行 EFS 同步；本机补上了有边界、超时、配额和落盘确认的文件快照实现。两次真实 EFS1/EFS2 会话已完成。 |
| ONLINE 与数据面 | 初始 `shutting-down` 不能直接理解为主机发了关机命令。一次标准 ONLINE 可正常初始化 SIM。数据面确认 PCIe 类型 3、接口号 4，使用 QMAP 和 rmnet，而不是注册成功后直接假定能上网。 |
| 故障保护 | 早期不安全 PCIe 恢复曾伴随整机重启；现保留 fail-closed 保护、关闭自动恢复，并限制每次主机启动只做一次 modem 启动尝试。不能因为 Apollo 支持热恢复，就认为 8T 也已验证。 |

稳定性改善发生在包含 OEM 镜像 20 修正和 EFS 同步的 r5 组合上；时序证据支持此前缺少运行期同步是重要问题，但这不是单变量实验，不能宣称唯一根因已被完全证明。

**固件目录里的占位文件不能当作真实 NV：**原厂固件包中的 `efs*.bin` 只有 512 字节，需要的是本机 `mdm1m9kefs1/2/3` 的完整 2 MiB 主机副本；DDR 也使用本机 1 MiB 副本，而不是 40 字节的 `mdmddr.mbn`。这些操作只准备主机文件，不向原分区写回。

## 3. 当前已经配置好的东西

| 项目 | 位置 / 状态 |
|---|---|
| 内核与构建 | 已运行 r5，当前槽 A；独立 X55 DTB。构建目录固定复用 `out-kebab/`，配置片段为 `arch/arm64/configs/kebab_x55.config`。 |
| Sahara 模块 | 已安装匹配内核的 `mhi_sahara_modem.ko`，策略版本 `kebab-oemnv-1`；控制脚本会拒绝旧模块。 |
| 固件 | `/lib/firmware/qcom/sdx55m/`：原厂镜像、`modem_pr/`、本机 EFS1/2/3、1 MiB `mdmddr.bin`、2 MiB `oemnvbktmp.bin`。 |
| 用户态程序 | `/opt/x55/bin/`：`modem.sh`、`mdm_helper_native`、`qmicli-x55`/qmicli、`qrtr-lookup`、`tqftpserv`、`pd-mapper`、`efs_sync.py`；私有库在 `/opt/x55/lib/`。不依赖 Android 可执行程序。 |
| 服务分组 | `x55-runtime.target` 管理下面四项服务；**target 本身不负责启动 modem，也不自动 ONLINE/拨号**。 |
| RFS 写目录 | `/var/lib/x55-tqftpserv/`，不是原始 NV 分区。 |
| EFS 快照 | `/var/lib/x55-efs/<本次启动及运行ID>/session-*/`；状态在 `/run/x55-efs/status.json`。 |
| 数据拨号工具 | 仓库 `tools/remoteproc/x55/wds_session.py`，**未安装到 `/opt/x55/bin`，不是常驻/开机服务**。 |
| 本次隔离网络 | `x55-data` 内有 `mhi_hwip0` 和 `x55rmnet0`（mux 1）；专用 DNS 在 `/etc/netns/x55-data/resolv.conf`。测试结束后保留但接口已 DOWN。 |
| 备份和证据 | `/root/x55-private/` 保存原始 NV、旧程序/模块和实验日志；最近数据测试在 `data-test-20260929/`。不得公开其中的标识、NV 或原始诊断数据。 |

四项服务的分工：

- `x55-mdm-helper.service`：ESOC 请求应答、启动完成通知、状态配合。
- `x55-tqftpserv.service`：提供 modem 固件远程文件系统。
- `x55-pd-mapper.service`：运行期服务/保护域映射。
- `x55-efs-sync.service`：接收 EFS，同步为新的私有普通文件快照。

它们没有 `[Install]` 段，`is-enabled` 显示 **static** 是预期行为；当前 active 是手动启动的结果，**不是开机自启**。没有自动重试，也没有 ModemManager/NetworkManager 自动拨号集成。

## 4. 平时如何启动、查看、停止

以下操作以 root 身份执行。**当前已经 ONLINE 时只需查看，不要再执行 `start`。**

```bash
/opt/x55/bin/modem.sh status   # 软件状态、真实 STATUS/ERRFATAL、服务及 EFS 状态

# 仅在新一次主机启动、ESOC offline、尚未尝试启动时：
/opt/x55/bin/modem.sh start    # 校验固件/模块，关闭 recovery，启动服务并异步启动 modem
/opt/x55/bin/modem.sh online   # 等待就绪，再明确请求正常 ONLINE；不是拨号
/opt/x55/bin/modem.sh probe    # DMS / UIM / NAS / WDS 状态查询
```

验收看的是：`status=1 errfatal=0`、四项服务 active、USIM ready、NAS registered。新配置先稳定观察至少 10 分钟，再进行数据测试。`remoteproc` 的 `running` 可能在 modem 崩溃后仍保留，不能单独作为健康依据。

停止顺序是 **停止测试流量 → 正常断开 WDS 数据连接 → 必要时停止 modem**：

```bash
/opt/x55/bin/modem.sh stop
```

`stop` 会先停止 modem，再停运行服务，以便正常关机时仍有 RFS/EFS 可用。同一主机启动内不能再次 `start`；不要清除 `/run/x55-modem-attempt` 或 EFS 状态来绕过保护。需要新启动实验时，先保存证据并安排正常主机重启。

### 信号和频段查询

QRTR 节点号不是固定的。先用下面命令查找 **service 2（DMS）所在节点**；本机测试中 WDS 和 NAS 也在该节点。不要把 WLAN/CDSP 节点当作 modem。

```bash
LD_LIBRARY_PATH=/opt/x55/lib /opt/x55/bin/qrtr-lookup
# NODE 需根据本次枚举结果设置，不能照抄历史值。
/opt/x55/bin/qmicli-x55 -d "qrtr://$NODE" --nas-get-signal-info
/opt/x55/bin/qmicli-x55 -d "qrtr://$NODE" --nas-get-rf-band-info
```

## 5. 如何做不影响 Wi-Fi 的移动数据测试

这是**手动实验流程**，不是一键联网脚本。`wds_session.py` 只管理 QMI 会话，不会自动创建接口、配置地址或 DNS。以下命令需逐步检查结果，任何错误都先停止排查。

### 5.1 先检查本次配置与残留状态

- modem 必须健康、已注册，四项服务正常。
- 查询 `--wds-get-profile-list=3gpp`；本机已确认 **profile 3 是 CMNET/default APN**。换卡或换固件后要重新确认，不能盲用编号。
- 若 `/run/x55-data-session/status.json` 已存在，先确认旧客户端退出且状态为 `stopped`，将该目录整体移动到私有备份目录留档后，才能发起新的数据会话；不要覆盖 `failed/quarantined` 现场。
- **WDS 数据会话可以在同一个健康 modem 启动周期内另行测试；它不等于重启 modem。** 不要动 `/run/x55-modem-attempt`。
- 当前命名空间和接口已经存在，应复用。只有不存在时才执行下面的创建步骤。

```bash
# 只用于首次创建；不是每次拨号都执行。
ip netns add x55-data
ip link set mhi_hwip0 netns x55-data
ip -n x55-data link add link mhi_hwip0 name x55rmnet0 type rmnet mux_id 1

# 首次创建或复用时，准备数据接口。
ip -n x55-data link set lo up
ip netns exec x55-data sysctl -q -w net.ipv6.conf.all.disable_ipv6=1
ip -n x55-data link set mhi_hwip0 mtu 16384
ip -n x55-data link set mhi_hwip0 up
```

命名空间中不得加入 Wi-Fi、veth 或通向主机的转发链路。这里关闭的 IPv6 仅属于测试命名空间；本阶段只验证 IPv4。

### 5.2 配置已经验证的数据端点

在**没有活动测试 bearer**时执行；`NODE` 仍使用本次查询到的 modem 节点：

```bash
/opt/x55/bin/qmicli-x55 -d "qrtr://$NODE" \
  --wda-set-data-format=link-layer-protocol=raw-ip,ul-protocol=qmap,dl-protocol=qmap,dl-datagram-max-size=16384,dl-max-datagrams=8,ep-type=pcie,ep-iface-number=4
/opt/x55/bin/qmicli-x55 -d "qrtr://$NODE" \
  --wda-get-data-format=ep-type=pcie,ep-iface-number=4
```

这里的 `raw-ip` 是链路层类型，**上下行聚合必须确认返回 QMAP**，不是关闭 QMAP 后直接用裸 MHI 接口测速。接口号 4 来自参考实现，并已被本机查询、绑定和数据传输验证。

### 5.3 启动同一个持久 WDS 客户端

```bash
REPO=/root/dev/mainline-instantnoodle
UNIT=x55-data-$(date +%s)   # 新实验使用唯一 unit 名，后续断开仍使用这个变量
systemd-run --unit="$UNIT" \
  --property=Restart=no --property=SendSIGKILL=no --property=TimeoutStopSec=50 \
  /usr/bin/python3 -u "$REPO/tools/remoteproc/x55/wds_session.py" \
  start --profile 3 --mux 1 --endpoint 4 --hold-seconds 900

python3 -m json.tool /run/x55-data-session/status.json
```

等待状态变为 `connected`。该进程持有同一 QRTR socket，依次完成端点绑定、IPv4 设置、拨号、获取参数、状态检查和断开。**不能用多个独立 `qmicli` 进程加同一个 CID 来替代。**

正常到达 900 秒或收到 SIGTERM 时会断开自己持有的数据连接；失败可能进入 `quarantined`，保留 socket 等待人工处理。它的 `status` 子命令查询的是新客户端，并不代表另一个进程的活动连接；活动会话应看状态文件、所属进程及实际数据收发。

### 5.4 只给命名空间配置地址和 DNS

从刚生成的 `status.json` 的 `config` 取值，**不要沿用上一次分配的地址**：

| 字段 | 用途 |
|---|---|
| `address` / `prefix` | `x55rmnet0` 的 IPv4 地址/前缀 |
| `gateway` | 测试命名空间的默认网关 |
| `mtu` | 子接口 MTU（本次为 1500） |
| `dns` | 写入 `/etc/netns/x55-data/resolv.conf` 的 nameserver 列表 |

按这些字段设置好 `ADDR`、`PREFIX`、`GW`、`MTU` 变量后：

```bash
: "${ADDR:?需取自当前 config}" "${PREFIX:?需取自当前 config}"
: "${GW:?需取自当前 config}" "${MTU:?需取自当前 config}"
ip -n x55-data -4 address flush dev x55rmnet0
ip -n x55-data address add "$ADDR/$PREFIX" dev x55rmnet0
ip -n x55-data link set x55rmnet0 mtu "$MTU"
ip -n x55-data link set x55rmnet0 up
ip -n x55-data route replace default via "$GW" dev x55rmnet0
```

将 `config.dns` 中的每个地址写成一行 `nameserver 地址`，存入上述专用 DNS 文件；不要直接写 JSON 数组。文件应放在本测试拥有的普通目录/文件中，首次需创建 `/etc/netns/x55-data/`。**不修改 `/etc/resolv.conf`，也不在主机命名空间添加蜂窝默认路由。**

### 5.5 验证路径，再进行限量下载

```bash
ip -4 route get 223.5.5.5                   # 主机应仍走 Wi-Fi
ip -n x55-data route get 223.5.5.5          # 应走 x55rmnet0
ip netns exec x55-data ping -I x55rmnet0 -c 3 -W 2 223.5.5.5

# 国内 CDN，下载指定范围到 /dev/null，不安装或执行下载内容。
ip netns exec x55-data curl -4 --proxy '' --noproxy '*' \
  --interface x55rmnet0 --range 0-8388607 --max-filesize 8388608 \
  --connect-timeout 5 --max-time 20 -sS -o /dev/null \
  -w 'HTTP=%{http_code} bytes=%{size_download} speed_Bps=%{speed_download}\n' \
  https://mirrors.aliyun.com/ubuntu/ls-lR.gz
```

确认 HTTP 206、实际字节数为 8 MiB，并检查蜂窝接口收发计数；不能把错误页的速度当作成绩。按当前测试约定**只测下载、优先国内节点**，不做长时间不限量压力测试。`speed_Bps × 8 ÷ 1000000` 才是 Mbps。

### 5.6 正常收尾

```bash
# 健康状态下：先结束测试流量，再让持有会话的原进程正常断开。
systemctl stop "$UNIT"
python3 -m json.tool /run/x55-data-session/status.json  # 确认 stopped
ip -n x55-data link set x55rmnet0 down
ip -n x55-data link set mhi_hwip0 down
```

不需要停止整个 modem；保持 ONLINE 便于继续研究。若出现下行卡死、QMI 超时或 `quarantined`，**不要照常连发 Stop、强杀进程或反复重启**；先保存现场。参考平台在已卡死的下行之后强行 Stop Network 曾触发 ERRFATAL。

## 6. 关键注意事项

1. **只使用本机固件/NV。** 不写原始 NV 分区，不拿其他设备的校准数据或标识替换，不运行会写原分区的 Android/Apollo 原始 EFS/OEM 备份程序。
2. **EFS 快照不是已完成的持久化方案。** 目前只存新的私有普通文件，单进程限 64 次/128 MiB，不自动回灌启动固件。因此运行期 NV 的持久回放、长期归档仍待设计。`ready` 只是通道就绪，`complete.json` 才表示会话完成。
3. **不要启用自动恢复/自启。** 单次数据连接的正常断开已验证，不代表 modem 热重启、SSR、休眠唤醒已验证。MHI 当前仍依赖轮询，MSI/省电路径尚未完成验证。
4. **分清四个层次：**mission mode ≠ ONLINE；ONLINE ≠ SIM/注册正常；LTE 注册 ≠ 数据连接；一次下载成功 ≠ 长期满速可靠。
5. **编号动态发现。** remoteproc 编号、QRTR 节点号、SIM/profile 信息都要检查；不要把历史 `remoteproc0`、节点号或动态 IP 写死。
6. **保留日志但保护隐私。** 标识、EFS 和原始诊断日志不放进 Git/公开输出；不要把设备身份值或校验硬编码进脚本。Wi-Fi 路由/DNS要留前后对照。
7. **故障时先留证据。** `modem.sh status`、四项服务的 `journalctl`、私有持久内核日志优先。未证明必要的 PM/IPA/SSCTL 探针不要乱开；缺失文件也不要随意伪造。

## 7. 维护与进一步工作

日常查看和拨号**不需要重新编译或刷机**。若确需改内核：先检查资源、版本号和模块匹配，始终增量复用 `out-kebab`，不 clean/mrproper、不新建构建树、不擅自改版本号。

```bash
cd /root/dev/mainline-instantnoodle
JOBS=4 tools/remoteproc/x55/build-kernel.sh
```

安装沿用 `install-kebab.sh`，选择 X55 DTB；只有明确安排刷写时才使用 `--write-slot a` 并保留备份/回读校验。**不修改 B、不切槽、不自动重启。** 仅修改 Sahara 时也必须匹配实际内核 vermagic，不能混装模块。

仍待完善：完整用户态连接管理、EFS 安全持久回放、长时间/高负载测试、MSI 与省电、modem 热恢复，以及独立验证的 IPv6/5G/通话。当前的 namespace 和拨号进程是可控测试设施，不是桌面的一键联网功能。

相关资料：[适配对比](COMPARISON.md) · [首次稳定性测试](TEST-2026-09-29-r5.md) · [LTE 数据实测](DATA-TEST-2026-09-29.md) · [离线测试](tests/README.md)。
