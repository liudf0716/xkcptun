#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET="$1"

if [ -z "$TARGET" ]; then
    echo "用法: $0 <01|02|03|04|05|06|07|08>"
    echo "例如: $0 05"
    exit 1
fi

MATCH_DIR=$(find "$DIR" -maxdepth 1 -type d -name "${TARGET}*" | head -n 1)

if [ -z "$MATCH_DIR" ] || [ ! -f "$MATCH_DIR/server.toml" ]; then
    echo "错误: 找不到以 '$TARGET' 开头的配置目录！"
    ls -d "$DIR"/0*
    exit 1
fi

echo "=========================================================="
echo "  切换至配置: $(basename "$MATCH_DIR")"
echo "=========================================================="

echo "[1/3] 上传 server.toml 到 bwg VPS..."
scp -O "$MATCH_DIR/server.toml" bwg:/etc/xkcptun/server.toml

echo "[2/3] 重启 bwg VPS 上 xkcptun-server 服务..."
ssh bwg "systemctl restart xkcptun-server && sleep 1 && xkcp_spy -p 9087 list"

echo "[3/3] 杀死本地已有的 xkcp_client 并用新配置启动..."
pkill -f "xkcp_client" 2>/dev/null || true
sleep 0.5
"$DIR/../build/xkcp_client" -c "$MATCH_DIR/client.toml" -d 4 >/tmp/xkcp_client.log 2>&1 &
sleep 1

echo "客户端已启动在后台（监听 127.0.0.1:2222，日志: /tmp/xkcp_client.log）"
echo "现在您可以在本地执行如下命令测试："
echo "  time scp -P 2222 root@127.0.0.1:/tmp/test_25m.bin /tmp/test_tunnel.bin"
echo "=========================================================="
