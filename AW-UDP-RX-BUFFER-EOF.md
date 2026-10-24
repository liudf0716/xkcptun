# AW 上大文件下载 EOF：xkcptun UDP 接收缓冲区排查与代码修复建议

更新时间：2026-10-24
状态：根因已定位；本文只记录排查结果和后续代码修改要求，未修改运行设备或 xkcptun 代码。

## 给后续代码修改者的摘要

AW（OpenWrt）上的 `xkcp_client` 为 KCP UDP socket 请求 16 MiB 的 `SO_RCVBUF`，但 AW 的 `net.core.rmem_max` 只有 212,992 字节。Linux 可在 `setsockopt(SO_RCVBUF)` 成功的同时把请求值限制到系统上限；当前代码没有读取并记录最终生效的 socket 缓冲值。实际运行中，AW 上属于 xkcptun `redir_tunnel` 的 UDP socket 出现大量内核接收丢包，KCP 对端随即报告大量未确认段并耗尽重传次数，最终下载连接收到 EOF。

**建议优先修复 xkcptun socket 缓冲设置的可观测性和有效值问题。不要先延长 deadlink 超时，也不要让程序自动修改全局 sysctl。**

## 场景与范围

- 客户端：AW，OpenWrt，运行 `xkcp_client`，配置包含 `dns_tunnel` 和 `redir_tunnel`。
- 服务端：BWG，运行 `xkcp_server`，UDP 监听端口 9089。
- 测试对象：从 `storage.googleapis.com` 下载约 65 MB 的公开更新包到 `/dev/null`。
- xdns 已将目标连接交给 AW 的 xkcptun `redir_tunnel`；BWG 能连上对应 Google Storage 地址。
- 排查未修改 xdns、xkcptun、kcptun-s 或 SS 的运行配置。

## 关键观测

### 大文件下载复现

更新包大小为 65,067,865 字节。完整下载测试中观察到：

| 复测 | 结果 | 说明 |
| --- | --- | --- |
| 第一次完整下载 | 2,365,895 字节后中断，约 31.1 秒，curl 报 partial file | 传输中断复现 |
| 第二次，测试端设 60 秒上限 | 收到 58,851,012 字节后由测试端超时 | 未证明 xkcptun 自行中断；不能把测试超时当成 EOF 根因 |
| 第三次完整下载 | 682,314 字节后中断，约 30.86 秒，curl 报 partial file | 传输中断再次复现 |

### 同一 KCP 会话的两端日志

第三次失败会话为 `conv 542705552`，目标为 `142.251.215.251:443`：

1. AW 日志显示 `redir_tunnel` 接受连接并把该会话目标解析为 `142.251.215.251:443`。
2. BWG 日志显示同一 conv 成功连接目标。
3. BWG 随后记录该 conv 有 1,653 个 KCP 段未确认，`state=4294967295`（`-1`），并 teardown。KCP 在段重传达到 `IKCP_DEADLINK` 后设置此状态，见 [`ikcp.c`](../ikcp.c#L1106)。

另一次失败会话 `conv 2069304038` 也在 BWG 成功连接 Google Storage 后进入 KCP deadlink，日志记录 1,174 个在途段；AW 随后约 30 秒没有收到对端 KCP 包并拆除本地会话。

### AW 客户端 socket 接收丢包

AW 上 xkcptun 进程持有的 UDP socket（`fd 8`、inode `9285`，对应 `redir_tunnel` 的 KCP UDP socket）在复测前后出现以下变化：

- 复测前 `/proc/net/udp` drops：70,137。
- 第三次失败后 drops：72,201。
- 本轮增加：**2,064 个 socket 接收丢包**。
- 同期 BWG 的 xkcptun UDP 9089 监听 socket drops 为 0。

AW 内核参数为：

```text
net.core.rmem_max = 212992
net.core.wmem_max = 212992
```

BWG 对照值为 16,777,216，BWG 的 xkcptun UDP 9089 socket drops 为 0。AW 全机 `Udp:RcvbufErrors` 与上述 xkcptun socket drops 相符，说明这批接收缓冲丢包记在 xkcptun 客户端 socket 上。

## 根因分析

代码默认 `sock_buf` 为 16,777,216 字节，见 [`xkcp_config.c`](../xkcp_config.c#L86)。客户端创建每条 tunnel 的 UDP socket 后调用 [`xkcp_apply_sockbuf_param`](../xkcp_util.c#L391)，对它设置 `SO_RCVBUF` 和 `SO_SNDBUF`，见 [`xkcp_client.c`](../xkcp_client.c#L238)。

Linux 对普通 `SO_RCVBUF` 请求受 `net.core.rmem_max` 限制。AW 的上限比 xkcptun 请求值小约 80 倍；普通 `setsockopt` 在被限制时可能仍返回成功。因此当前代码没有错误日志，并不代表 socket 实际拿到了 16 MiB。代码只在 `setsockopt` 返回失败时打印 warning，没有用 `getsockopt(SO_RCVBUF)` 核对最终值。

同时，客户端 KCP 接收窗口配置为 4,096 段，服务端发送窗口也是 4,096 段级别；这允许服务端快速向客户端发送远多于约 212 KiB 接收队列可容纳的数据。AW 接收队列溢出后，KCP 数据/ACK 包会丢失；BWG 随后看到大量未确认段并达到 KCP deadlink。xkcptun 的 30 秒保护逻辑只是清理已失联会话，延长该超时不能补回丢失的数据，相关逻辑见 [`xkcp_util.c`](../xkcp_util.c#L486)。

**结论：故障点在 AW xkcptun 客户端 UDP 接收队列上限，不是 xdns 的域名识别，也不是 BWG 的 9089 socket 接收队列。**

## 后续代码修改要求

1. 在 socket 设置后调用 `getsockopt(SO_RCVBUF)` 和 `getsockopt(SO_SNDBUF)`，记录请求值与内核返回的有效值。Linux 返回值存在翻倍记账语义，日志应说明采用的平台值语义，避免错误地把返回值当成应用可用 payload 容量。
2. 如果有效接收缓冲显著小于配置请求值，按 tunnel 名称打印清楚的 warning，至少包含请求值、有效值和建议检查的 `net.core.rmem_max`。不要把“`setsockopt` 返回成功”当成缓冲设置成功的充分条件。
3. 评估提供可选的 socket 级强制缓冲设置：在支持 `SO_RCVBUFFORCE` / `SO_SNDBUFFORCE` 的 Linux 上尝试使用；若权限、内核或平台不支持，安全回退到普通 `SO_RCVBUF` / `SO_SNDBUF`，再读取有效值并报告。需用 `#ifdef` 保护平台差异，不要假定 OpenWrt 和所有 Linux 构建都支持 force 选项。
4. 应用程序不得自行写入 `/proc/sys/net/core/*` 或持久化修改系统 sysctl。若 force 选项不可用，提示管理员调整系统上限即可。
5. 默认行为及配置兼容性应保持稳定；缓冲不足时先提供可操作的诊断。避免以增加 `XKCP_DEAD_LINK_TIMEOUT_MS`、关闭 deadlink teardown 或单纯增大 KCP 窗口来掩盖问题。
6. 影响范围限制在 xkcptun 自己创建的 UDP socket；不要改 xdns BPF、kcptun-s、Shadowsocks 或其他服务。

## 修改后的验证清单

1. 为 socket buffer helper 增加测试：请求小于上限、请求大于普通上限、force 选项成功、force 选项失败后回退。测试需根据权限和平台能力跳过不适用分支，而不是假设每个 CI 都有 `CAP_NET_ADMIN`。
2. 在 AW 运行时检查 `redir_tunnel` 的实际 `SO_RCVBUF`，确认它与配置目标相符；若因权限或系统限制无法达到目标，应看到明确 warning。
3. 记录 AW `/proc/net/udp` 对应 xkcptun socket drops 的测试前后值和 `Udp:RcvbufErrors`，同时记录 BWG 9089 socket drops。
4. 经 AW → xdns → xkcptun → BWG 路径下载完整 65 MB 测试文件，要求 curl 返回 0 且收到完整字节数；期间不应出现对应 conv 的 deadlink 日志，AW xkcptun socket drops 不应持续增加。
5. 回归确认 xdns 仍能将域名连接交给 xkcptun，AW/BWG 的 xkcptun 服务正常；kcptun-s 和 SS 服务仍 active，未受代码或设备配置变更影响。

## 相关代码位置

- 默认缓冲和 KCP 窗口：[`xkcp_config.c`](../xkcp_config.c#L62)
- socket buffer 设置：[`xkcp_util.c`](../xkcp_util.c#L391)
- 客户端创建 KCP UDP socket：[`xkcp_client.c`](../xkcp_client.c#L238)
- KCP 达到 deadlink：[`ikcp.c`](../ikcp.c#L1106)
- deadlink teardown：[`xkcp_util.c`](../xkcp_util.c#L486)
- xdns TCP redirect 决策：[`xdns_bpf.c`](../../xdns-bpf/src/xdns_bpf.c#L278)
