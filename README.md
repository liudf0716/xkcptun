![xkcptun](https://github.com/liudf0716/xkcptun/blob/master/logo-big.png)

[![Build Status][1]][2] 
[![Powered][3]][4]
[![license][5]][6]
[![PRs Welcome][7]][8]
[![Issue Welcome][9]][10]
[![OpenWRT][11]][12]
[![KunTeng][13]][14]

[1]: https://img.shields.io/travis/liudf0716/xkcptun.svg?style=plastic
[2]: https://travis-ci.org/liudf0716/xkcptun
[3]: https://img.shields.io/badge/KCP-Powered-blue.svg?style=plastic
[4]: https://github.com/skywind3000/kcp
[5]: https://img.shields.io/badge/license-GPLV3-brightgreen.svg?style=plastic
[6]: https://github.com/KunTengRom/xfrp/blob/master/LICENSE
[7]: https://img.shields.io/badge/PRs-welcome-brightgreen.svg?style=plastic
[8]: https://github.com/liudf0716/xkcptun/pulls
[9]: https://img.shields.io/badge/Issues-welcome-brightgreen.svg?style=plastic
[10]: https://github.com/liudf0716/xkcptun/issues/new
[11]: https://img.shields.io/badge/Platform-%20OpenWRT%20%7CLEDE%20%7CCentOS%20-brightgreen.svg?style=plastic
[12]: https://github.com/gigibox/openwrt-xkcptun
[13]: https://img.shields.io/badge/KunTeng-Inside-blue.svg?style=plastic
[14]: http://rom.kunteng.org

# xkcptun 基于kcp和libevent2库，用c语言实现的kcptun

xkcptun主要应用于LEDE，openwrt中，其原理如图：

<img src="kcptun.png" alt="kcptun" height="300px"/>

### Compile

xkcptun依赖[libevent2](https://github.com/libevent/libevent)

干净 Ubuntu/Debian 上建议先装编译工具链：

```
sudo apt-get install -y build-essential cmake
```

`cmake` 默认会检测 `pkg-config` 和 `libevent`；若缺失，会尝试用 `apt` 自动安装 `pkg-config`、`libevent-dev`（需要 root，或当前用户可用 `sudo` 且无需交互输密码）。关闭自动安装：`cmake -DAUTO_INSTALL_DEPS=OFF ..`

也可手动安装依赖：
- **Ubuntu / Debian**：`sudo apt-get install -y pkg-config libevent-dev`
- **CentOS 7+ / RHEL / Rocky / AlmaLinux**：`sudo yum install -y gcc gcc-c++ make cmake pkgconfig libevent-devel`（注意：CentOS 6 自带的 `libevent` 为 1.4 版本过旧，需源码编译安装 `libevent 2.x` 并配置 `PKG_CONFIG_PATH`）

```shell
git clone https://github.com/liudf0716/xkcptun.git
cd xkcptun
mkdir build && cd build
cmake ..
make
sudo make install
```

生成 `xkcp_client`、`xkcp_server`、`xkcp_spy`（`make install` 默认安装至 `/usr/local/bin/`）。

#### 参考文档

1, [安装libjson c的问题](https://github.com/liudf0716/xkcptun/wiki/%E5%AE%89%E8%A3%85libjson-c%E7%9A%84%E9%97%AE%E9%A2%98)

2, [bbr vs kcp  优化http下载性能对比报告](https://github.com/liudf0716/xkcptun/wiki/bbr-vs-kcp-%E4%BC%98%E5%8C%96http%E4%B8%8B%E8%BD%BD%E6%80%A7%E8%83%BD%E5%AF%B9%E6%AF%94%E6%8A%A5%E5%91%8A)

3, [AW 上大文件下载 EOF：xkcptun UDP 接收缓冲区排查与代码修复建议](AW-UDP-RX-BUFFER-EOF.md)

### OpenWrt
编译及安装请参考 [openwrt-xkcptun](https://github.com/gigibox/openwrt-xkcptun)

### QuickStart（TOML 配置与服务端部署）

现代版本 `xkcptun` 采用 **TOML** 配置文件（参考仓库根目录的 `server.toml` 与 `client.toml`）。

1. **服务端配置（`/etc/xkcptun/server.toml`）**

假设服务端开放 UDP 端口 `9089`（请确保云服务器安全组与本地防火墙放行 UDP `9089`）：

```toml
[global]
syslog = true
mon_port = 9087
local_address = "0.0.0.0" # 也可填写具体网卡名如 "eth0" 或指定 IP
local_port = 9089
key = "your-secret-key"
mode = "fast3"
mtu = 1350
sndwnd = 1024
rcvwnd = 1024
datashard = 10
parityshard = 3
sockbuf = 4194304
keepalive = 10
# 可选：若需固定默认转发目标（非 dynamic_target 场景），可同时指定 remote_addr / remote_port
# remote_addr = "127.0.0.1"
# remote_port = 80
```

2. **客户端配置（`client.toml`）**

支持多隧道并行，可通过 `proxy_type` 指定转发类型（`forward` 固定端口转发、`socks5` 内置 SOCKS5 代理、`redir` 透明代理），或通过 `proto = "udp"` 启用直通 UDP 转发：

```toml
[global]
syslog = false
mon_port = 9086
key = "your-secret-key"
remote_addr = "your-server-ip"
remote_port = 9089
mode = "fast3"
mtu = 1350
sndwnd = 1024
rcvwnd = 1024
datashard = 10
parityshard = 3
sockbuf = 4194304
keepalive = 10

# 示例 1：固定目标 TCP 端口转发（本地 9088 -> 服务端 127.0.0.1:80）
[[tunnel]]
name = "web_forward"
local_address = "127.0.0.1" # 填 "any" 或 "0.0.0.0" 监听全网卡；macOS/Windows Cygwin 可直接填 IP
local_port = 9088
target_addr = "127.0.0.1"
target_port = 80

# 示例 2：内置 SOCKS5 代理隧道
[[tunnel]]
name = "socks5_proxy"
proxy_type = "socks5"
local_address = "127.0.0.1"
local_port = 1080

# 示例 3：UDP 转发（如 DNS）
[[tunnel]]
name = "dns_udp"
proto = "udp"
local_address = "127.0.0.1"
local_port = 5353
target_addr = "8.8.8.8"
target_port = 53
```

注意：`mode` 字段（`fast3`/`fast2`/`fast`/`normal`）会覆盖 `nodelay`、`interval`、`resend`、`nc` 四个字段的值（与 kcptun 行为一致）。如需精细调优，请设置 `mode = "manual"` 或省略 `mode`。

3. **启动与 systemd 守护部署**

前台调试运行：
```shell
xkcp_server -c server.toml -f -d 7
xkcp_client -c client.toml -f -d 7
```

正式后台运行可去掉 `-f` 并将 `-d` 设为 `0`~`2`，或使用仓库提供的 [xkcptun.service](xkcptun.service) 注册 systemd 服务：
```shell
sudo mkdir -p /etc/xkcptun
sudo cp server.toml /etc/xkcptun/server.toml
sudo cp xkcptun.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now xkcptun
```

## FEC 前向纠错（实验性）

在两端配置中增加布尔字段 `fec` 即可启用前向纠错：

```
"fec": 1
```

- 启用后，每个 UDP 报文会带上 8 字节自描述帧头，并按 `datashard`/`parityshard` 分组生成 Reed-Solomon 校验包；接收端只要收到组内任意 `datashard` 个分片即可还原全部数据，无需重传。
- FEC 只在持续满速传输时提供保护：未攒满一组的零散包会立即裸发，**不增加任何延迟**。
- 启用 FEC 时 KCP 的 MTU 会自动下调 8 字节以容纳帧头，无需手工调整 `mtu`。
- **两端必须同时启用或同时关闭**，且 `datashard`/`parityshard` 取值一致，否则无法互通。取值范围：`datashard` 1~64，`parityshard` 0~64。链路较差时可增大 `parityshard`（带宽开销约为 `parityshard/(datashard+parityshard)`）；好链路建议直接关闭以省流量。

### 测试与状态监控

验证本地转发效果：
```shell
curl http://127.0.0.1:9088
```

查看服务端/客户端运行状态：
```shell
xkcp_spy -h 127.0.0.1 -s -t status
xkcp_spy -h 127.0.0.1 -c -t status
```

### Todo

Compatible with [kcptun](https://github.com/xtaci/kcptun)  <img src="https://github.com/xtaci/kcptun/blob/master/logo-small.png" alt="kcptun" height="24px" /> 



### How to contribute our project(给本项目做贡献)


欢迎大家给本项目提供意见和贡献，提供意见的方法可以在本项目的[Issues](https://github.com/liudf0716/xkcptun/issues/new)提，更加欢迎给项目提PULL REQUEST，具体提交PR的方法请参考[CONTRIBUTING](https://github.com/liudf0716/xkcptun/blob/master/CONTRIBUTING.md)


### Contact me 

QQ群 ： [331230369](https://jq.qq.com/?_wv=1027&k=47QGEhL)

