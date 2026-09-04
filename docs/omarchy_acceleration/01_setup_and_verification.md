# Omarchy / Linux 本机 AI 与 GitHub 极速透明加速环境搭建指南 (xdns-bpf + xkcptun)

## 1. 概述与应用背景

在现代 AI 辅助研发工作流中，开发者频繁使用各类 AI 编程助理（如 **Codex**、**Google Antigravity (AGY)**、**Claude Code**、**Grok / xAI**）以及 **GitHub Copilot / Git CLI**。这些工具在日常运行中面临两大痛点：
1. **API / 节点长连接抖动与超时**：AI CLI（如 `claude`、`agy`）多基于 HTTP/2 或 WebSocket 与云端保持流式输出（SSE），公网链路波动极易造成输出中断。
2. **传统代理配置割裂**：很多命令行工具或底层库不遵循系统的 `http_proxy` 环境变量，或者部分依赖纯 IP 直连，导致开发者频繁陷入繁琐的网络排错与证书/环境变量配置。

本项目结合 **`xdns-bpf`（基于 Linux eBPF 的内核层透明拦截）** 与 **`xkcptun`（基于 KCP 的抗弱网极速加密隧道）**，为开发者提供**系统级无感透明加速**方案：
- **真正的全系统透明代理**：通过 eBPF Socket 级 Hook（`cgroup/connect4`），应用程序发起 `connect()` 瞬间即被自动引导至加速隧道，无需配置任何 `export https_proxy`，对所有 AI CLI、Git 及开发工具完全无感知；
- **原生兼容标准透明代理**：通过 `cgroup/getsockopt` 钩子原生支持标准 `SO_ORIGINAL_DST` 查询；
- **智能按需代理**：仅对目标域名列表（如 `github.com`、`anthropic.com`、`openai.com`、`x.ai`）自动解析学习 IP 并实时劫持加速，国内流量与局域网直接直连。

---

## 2. 软件要求与架构设计

### 架构图示

```
+-----------------------------------------------------------------------------------+
| 本机 Host 环境 (Omarchy / Linux Mint / Ubuntu / Arch)                             |
|                                                                                   |
|  [AI CLI / Apps]                                                                  |
|   (Claude Code / AGY / Codex / Grok / git)                                        |
|         |                                                                         |
|         | 1. connect() 访问目标 IP (如 GitHub / OpenAI / Claude)                   |
|         v                                                                         |
|  +-----------------------------------------------------------------------------+  |
|  | 内核层 eBPF Socket Hooks (xdns-bpf)                                         |  |
|  |                                                                             |  |
|  |  * cgroup/connect4 : 命中 IP 集合立即改写为 127.0.0.1:12345 (极速无需过网卡) |  |
|  |  * cgroup/sendmsg4 : 拦截未连接 DNS 查询重定向到 127.0.0.1:5353               |  |
|  |  * sockops         : 握手建立瞬间记录 Client Port 与原始目的地址映射         |  |
|  |  * cgroup/getsockopt: 拦截 SO_ORIGINAL_DST 返回真实目的 IP                   |  |
|  +-----------------------------------------------------------------------------+  |
|         |                                                                         |
|         v (本地回环重定向)                                                         |
|  +-----------------------------------------------------------------------------+  |
|  | 本地 xkcptun Client (透明代理端口 12345 / DNS 端口 5353)                     |  |
|  +-----------------------------------------------------------------------------+  |
+---------|-------------------------------------------------------------------------+
          | 2. KCP 高速抗丢包加密 UDP 隧道 (端口 9089)
          v
+-----------------------------------------------------------------------------------+
| 远端 VPS 服务器 (如 bwg: 172.96.252.145)                                          |
|                                                                                   |
|  [xkcptun Server (xkcp_server)]                                                   |
|         |                                                                         |
|         | 3. 高速出口直连                                                         |
|         v                                                                         |
|   [GitHub / Anthropic / OpenAI / xAI CDN 节点]                                    |
+-----------------------------------------------------------------------------------+
```

### 依赖环境
- **操作系统**：Linux 5.10+（推荐 Arch Linux / Omarchy / Linux Mint 22 / Ubuntu 22.04+，内核开启 cgroup v2 与 BPF 支持）
- **编译工具**：`clang`, `llvm`, `cmake`, `make`, `gcc`, `libbpf-dev`, `libevent-dev`
- **系统工具**：`bpftool`, `curl`, `iproute2`

---

## 3. 代码下载与编译

### 3.1 安装系统编译依赖

在 Omarchy / Arch 衍生系统上：
```bash
sudo pacman -S base-devel cmake clang llvm libbpf libevent bpftool
```
（若在 Ubuntu / Linux Mint 上）：
```bash
sudo apt update
sudo apt install -y build-essential cmake clang llvm libbpf-dev libevent-dev linux-tools-generic linux-tools-$(uname -r)
```

### 3.2 编译 xdns-bpf (控制端 CLI 与 eBPF 目标文件)

```bash
# 进入工作目录
git clone https://github.com/liudf0716/xdns-bpf.git
cd xdns-bpf

# 使用 CMake 构建 (默认构建 HOST 模式)
cmake -B build -S .
cmake --build build -j$(nproc)

# 验证编译产物
ls -l build/xdns-ctl build/xdns_bpf.o
```

### 3.3 编译 xkcptun (客户端隧道程序)

```bash
# 进入 xkcptun 代码库
git clone https://github.com/liudf0716/xkcptun.git
cd xkcptun

# 使用 CMake 构建客户端与依赖组件
cmake -B build -S .
cmake --build build -j$(nproc)

# 验证编译产物
ls -l build/xkcp_client
```

---

## 4. 服务端与客户端配置

### 4.1 远端 VPS (bwg) 服务端配置

在远端服务器（如 `172.96.252.145`）上，运行 `xkcp_server`：
配置文件 `/etc/xkcptun/server.toml`：
```toml
[global]
syslog = true
mon_port = 9087
local_port = 9089
key = "7f7ac683d720e1437019692eadb0b811"
```
启动服务端：
```bash
sudo /usr/local/bin/xkcp_server -c /etc/xkcptun/server.toml -f &
```

### 4.2 本机 xkcptun 客户端配置

创建本地配置文件 `client_host.toml`：
```toml
[global]
remote_addr = "172.96.252.145"   # 替换为你的 bwg/远端 VPS IP
remote_port = 9089
mode = "fast3"
fec = 1
sndwnd = 1024
rcvwnd = 4096
datashard = 10
parityshard = 3
sockbuf = 16777216
key = "7f7ac683d720e1437019692eadb0b811"

# DNS 透明转发隧道 (转发至海外公共 DNS 8.8.8.8)
[[tunnels]]
name = "dns_tunnel"
proto = "udp"
local_port = 5353
target_addr = "8.8.8.8"
target_port = 53

# TCP 透明重定向隧道
[[tunnels]]
name = "redir_tunnel"
local_port = 12345
proxy_type = "redir"
```

---

## 5. 启动与运行测试

### 5.1 启动 xkcptun 客户端

```bash
sudo ./build/xkcp_client -c ./client_host.toml -f &
```
控制台会输出：
```text
[dns_tunnel] UDP Proxy listening on 0.0.0.0:5353 -> Remote 172.96.252.145:9089 -> Target 8.8.8.8:53
[redir_tunnel] TCP Proxy listening on 0.0.0.0:12345 -> Remote 172.96.252.145:9089
```

### 5.2 启动 xdns-bpf Host 模式并添加代理域名

执行 `xdns-ctl` 进行全局无感挂载与域名注册：

```bash
# 1. 启动 Host 模式（全自动载入并挂载 cgroup）
sudo ./build/xdns-ctl host-start

# 2. 配置 xkcptun 本地监听端点 (DNS: 127.0.0.1:5353, TCP: 127.0.0.1:12345)
sudo ./build/xdns-ctl set-config 127.0.0.1:5353 127.0.0.1:12345 1

# 3. 添加加速域名 (包含通配符子域名自动支持)
# GitHub 加速
sudo ./build/xdns-ctl add-domain "github.com"
sudo ./build/xdns-ctl add-domain "githubusercontent.com"

# AI 常用域名加速 (Anthropic, OpenAI, xAI, Google)
sudo ./build/xdns-ctl add-domain "anthropic.com"
sudo ./build/xdns-ctl add-domain "claude.ai"
sudo ./build/xdns-ctl add-domain "openai.com"
sudo ./build/xdns-ctl add-domain "oaistatic.com"
sudo ./build/xdns-ctl add-domain "x.ai"
sudo ./build/xdns-ctl add-domain "grok.com"
```

### 5.3 测试验证

#### 验证 1：查看内核拦截状态与 IP 集合
```bash
sudo ./build/xdns-ctl list-domains
sudo ./build/xdns-ctl list-ips
```

#### 验证 2：直接使用 curl 验证透明代理与原始目的地址还原
直接执行 curl 请求目标节点（无需带 `--proxy` 参数）：
```bash
curl -v -k https://github.com/
```
观察 `xkcp_client` 终端输出：
```text
[redir_tunnel] accept new client [15] in, conv [1703847521]
[redir_tunnel] conv [1703847521] SO_ORIGINAL_DST target [172.182.252.133]:[443]
[redir_tunnel] conv [1703847521] sent authenticated dynamic target header [172.182.252.133]:[443]
* SSL connection using TLSv1.3
* Server certificate: CN=github.com
HTTP/2 200 OK
```
`xkcptun` 成功通过内核 `getsockopt` 还原了被代理的目标地址，端到端握手完全成功。

#### 验证 3：运行 AI 编程助手
在当前终端直接启动任意 AI 工具，无需任何环境配置：
```bash
# 测试 GitHub CLI 与 Git clone
git clone https://github.com/liudf0716/xkcptun.git

# 测试 Claude Code
claude

# 测试 Google Antigravity / Codex
agy
```
所有流量均在发起瞬间被 eBPF 静默定向至高可靠的 KCP 隧道，彻底告别长连接中断与连接超时。

### 5.4 停止与清理
如果需要停止本地加速，仅需一条命令：
```bash
sudo ./build/xdns-ctl host-stop
sudo killall xkcp_client
```
系统将自动从 cgroup 脱离所有 eBPF 挂钩，恢复原生直连网络。
