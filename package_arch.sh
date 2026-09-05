#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

VERSION=$(grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' version.h 2>/dev/null | head -n 1 || echo "1.0.9")
BUILDDATE=$(date +%s)
DIST_DIR="$SCRIPT_DIR/dist"
mkdir -p "$DIST_DIR"

echo "=== 正在构建 xkcptun (${VERSION}) ==="
cmake -B build -S .
cmake --build build -j$(nproc)

# 打包 client pkg.tar.zst
PKGDIR="/tmp/arch_xkcptun_client"
rm -rf "$PKGDIR"
mkdir -p "$PKGDIR/usr/bin"
mkdir -p "$PKGDIR/etc/xkcptun"
mkdir -p "$PKGDIR/usr/lib/systemd/system"

cp build/xkcp_client "$PKGDIR/usr/bin/"

cat << 'TOML' > "$PKGDIR/etc/xkcptun/client.toml"
[global]
remote_addr = "127.0.0.1"        # 替换为远端 VPS 的公网 IP
remote_port = 9089
mode = "fast3"
fec = 1
key = "7f7ac683d720e1437019692eadb0b811"

# DNS 透明转发隧道 (本地 5353 转发至海外 8.8.8.8)
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

cat << 'SVC' > "$PKGDIR/usr/lib/systemd/system/xkcp_client.service"
[Unit]
Description=xkcptun Client Service
After=network.target

[Service]
Type=simple
User=root
ExecStart=/usr/bin/xkcp_client -c /etc/xkcptun/client.toml -f
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
SVC

cat << PKGINFO > "$PKGDIR/.PKGINFO"
pkgname = xkcptun-client
pkgbase = xkcptun
pkgver = ${VERSION}-1
pkgdesc = High speed and loss-resilient encrypted KCP tunnel client for Omarchy/Arch
url = https://github.com/liudf0716/xkcptun
builddate = ${BUILDDATE}
packager = liudf0716 <liudf0716@gmail.com>
size = 140000
arch = x86_64
license = MIT
depend = libevent
backup = etc/xkcptun/client.toml
PKGINFO

cd "$PKGDIR"
tar --zstd -cf "$DIST_DIR/xkcptun-client-${VERSION}-1-x86_64.pkg.tar.zst" .PKGINFO etc usr
rm -rf "$PKGDIR"
echo "已生成 Omarchy/Arch 安装包: $DIST_DIR/xkcptun-client-${VERSION}-1-x86_64.pkg.tar.zst"
