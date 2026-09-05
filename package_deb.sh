#!/bin/bash
set -e

# 获取脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# 获取版本号，优先从 version.h 提取，否则设为 1.0.9
VERSION=$(grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' version.h 2>/dev/null | head -n 1 || echo "1.0.9")
ARCH=$(dpkg --print-architecture 2>/dev/null || echo "amd64")

echo "=== 正在构建 xkcptun (${VERSION}) ==="
cmake -B build -S .
cmake --build build -j$(nproc)

DIST_DIR="$SCRIPT_DIR/dist"
mkdir -p "$DIST_DIR"

# -------------------------------------------------------------
# 1. 构建 xkcptun-server deb 包
# -------------------------------------------------------------
echo "=== 打包 xkcptun-server ==="
SERVER_PKG="/tmp/pkg_xkcptun_server"
rm -rf "$SERVER_PKG"
mkdir -p "$SERVER_PKG/DEBIAN"
mkdir -p "$SERVER_PKG/usr/local/bin"
mkdir -p "$SERVER_PKG/etc/xkcptun"
mkdir -p "$SERVER_PKG/lib/systemd/system"

# 二进制文件
cp build/xkcp_server "$SERVER_PKG/usr/local/bin/"
cp build/xkcp_spy "$SERVER_PKG/usr/local/bin/"

# 默认服务端配置文件
cat << 'TOML' > "$SERVER_PKG/etc/xkcptun/server.toml"
[global]
syslog = true
mon_port = 9087
local_port = 9089
key = "7f7ac683d720e1437019692eadb0b811"
TOML

# systemd 服务
cat << 'SVC' > "$SERVER_PKG/lib/systemd/system/xkcp_server.service"
[Unit]
Description=xkcptun Server Service
After=network.target

[Service]
Type=simple
User=root
ExecStart=/usr/local/bin/xkcp_server -c /etc/xkcptun/server.toml -f
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
SVC

# deb control 信息
cat << CTRL > "$SERVER_PKG/DEBIAN/control"
Package: xkcptun-server
Version: ${VERSION}
Section: net
Priority: optional
Architecture: ${ARCH}
Maintainer: liudf0716 <liudf0716@gmail.com>
Depends: libc6, libevent-2.1-7
Description: High speed and loss-resilient encrypted KCP tunnel server for Linux
CTRL

# 标记配置文件，升级时不覆盖用户配置
echo "/etc/xkcptun/server.toml" > "$SERVER_PKG/DEBIAN/conffiles"

# 构建 server deb
SERVER_DEB="$DIST_DIR/xkcptun-server_${VERSION}_${ARCH}.deb"
dpkg-deb --build "$SERVER_PKG" "$SERVER_DEB"
echo "已生成服务端安装包: $SERVER_DEB"

# -------------------------------------------------------------
# 2. 构建 xkcptun-client deb 包
# -------------------------------------------------------------
echo "=== 打包 xkcptun-client ==="
CLIENT_PKG="/tmp/pkg_xkcptun_client"
rm -rf "$CLIENT_PKG"
mkdir -p "$CLIENT_PKG/DEBIAN"
mkdir -p "$CLIENT_PKG/usr/local/bin"
mkdir -p "$CLIENT_PKG/etc/xkcptun"
mkdir -p "$CLIENT_PKG/lib/systemd/system"

# 二进制文件
cp build/xkcp_client "$CLIENT_PKG/usr/local/bin/"

# 默认客户端配置文件
cat << 'TOML' > "$CLIENT_PKG/etc/xkcptun/client.toml"
[global]
remote_addr = "127.0.0.1"        # 替换为远端 VPS IP
remote_port = 9089
mode = "fast3"
fec = 1
key = "7f7ac683d720e1437019692eadb0b811"

# DNS 透明转发隧道 (本地 5353 经 KCP 转到海外 8.8.8.8)
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
TOML

# systemd 服务
cat << 'SVC' > "$CLIENT_PKG/lib/systemd/system/xkcp_client.service"
[Unit]
Description=xkcptun Client Service
After=network.target

[Service]
Type=simple
User=root
ExecStart=/usr/local/bin/xkcp_client -c /etc/xkcptun/client.toml -f
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
SVC

# deb control 信息
cat << CTRL > "$CLIENT_PKG/DEBIAN/control"
Package: xkcptun-client
Version: ${VERSION}
Section: net
Priority: optional
Architecture: ${ARCH}
Maintainer: liudf0716 <liudf0716@gmail.com>
Depends: libc6, libevent-2.1-7
Description: High speed and loss-resilient encrypted KCP tunnel client for Linux/Omarchy
CTRL

# 标记配置文件
echo "/etc/xkcptun/client.toml" > "$CLIENT_PKG/DEBIAN/conffiles"

# 构建 client deb
CLIENT_DEB="$DIST_DIR/xkcptun-client_${VERSION}_${ARCH}.deb"
dpkg-deb --build "$CLIENT_PKG" "$CLIENT_DEB"
echo "已生成客户端安装包: $CLIENT_DEB"

# 清理临时目录
rm -rf "$SERVER_PKG" "$CLIENT_PKG"
echo "=== xkcptun 所有 deb 安装包生成完毕 ==="
