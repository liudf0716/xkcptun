# 🛠️ xkcptun 性能基准测试与手动操作指南

本目录包含了 8 种典型网络配置（从传统 TCP 拥塞控制模式到终极黄金 FEC 模式）的完整服务端与客户端配置文件。您可以随时手动切换并进行实测对比。

---

## 📁 目录结构与各配置说明

| 目录名称 | 核心参数特征 | 适用场景 / 测试目的 |
| :--- | :--- | :--- |
| **`01_normal/`** | `mode="normal", fec=0, interval=30ms, nc=0` | 传统 TCP 类似模式（遇丢包频繁慢启动） |
| **`02_fast/`** | `mode="fast", fec=0, interval=20ms, nc=0` | 缩短内部时钟模式 |
| **`03_fast2/`** | `mode="fast2", fec=0, nodelay=1, interval=10ms, nc=0` | 极速响应但保留常规拥塞窗口减半 |
| **`04_fast3_nofec/`** | `mode="fast3", fec=0, nodelay=1, nc=1` | **激进加速**：关闭常规拥塞退缩 |
| **`05_fast3_fec10_3_golden/`** | `mode="fast3", fec=1 (10:3), wnd=1024, pacing=128` | **🏆 黄金默认推荐组合**（满速抗弱网） |
| **`06_fast3_fec5_2_high_loss/`** | `mode="fast3", fec=1 (5:2), 40% 校验冗余` | 极端恶劣弱网（>15% 丢包） |
| **`07_fast3_fec10_3_wnd512/`** | `mode="fast3", fec=1 (10:3), wnd=512` | 紧凑小窗口（抗突发微丢包） |
| **`08_fast3_fec10_3_wnd2048/`** | `mode="fast3", fec=1 (10:3), wnd=2048` | 大带宽万兆骨干链路 |

每个目录下均包含：
- **`server.toml`**：部署在 VPS（`bwg`）上的服务端配置文件
- **`client.toml`**：运行在本地客户端（Host 主机或路由器）的配置文件

---

## 🚀 手动测试完整操作步骤

### 第一步：在服务端部署并生效指定配置（以 05_golden 为例）

将选定目录下的 `server.toml` 上传到 VPS 并重启服务：
```bash
# 1. 复制服务端配置到 bwg VPS
scp benchmark_configs/05_fast3_fec10_3_golden/server.toml bwg:/etc/xkcptun/server.toml

# 2. 重启 VPS 上的 xkcptun 服务
ssh bwg "systemctl restart xkcptun-server"

# 3. 验证服务端运行状态
ssh bwg "xkcp_spy -p 9087 list"
```

---

### 第二步：在本地 Host 启动客户端

在本地终端启动 `xkcp_client`（建议开一个独立终端或后台运行）：
```bash
# 启动客户端（前台运行便于查看连接）
./build/xkcp_client -c benchmark_configs/05_fast3_fec10_3_golden/client.toml -f -d 4
```

> **客户端工作端口**：本地监听 `127.0.0.1:2222`，所有连接将被加速转发至 VPS 的 SSH（端口 `27334`）。

---

### 第三步：执行真实下载测试与对比

在 VPS 上已准备好 25MB 测试文件 `/tmp/test_25m.bin`。在本地打开新终端执行：

#### 1. 测试 Direct SCP 直连（原生 TCP 作为基准）
```bash
time scp -P 27334 root@172.96.252.145:/tmp/test_25m.bin /tmp/direct_test.bin
```

#### 2. 测试 xkcptun 隧道加速（走本地 2222 端口）
```bash
time scp -P 2222 root@127.0.0.1:/tmp/test_25m.bin /tmp/tunnel_test.bin
```

---

### 第四步：模拟真实弱网丢包环境（可选进阶对比）

若想验证晚高峰 5% 真实丢包下的抗网络抖动能力：

```bash
# 1. 在 VPS 上主动注入 5% 物理丢包
ssh bwg "tc qdisc add dev eth0 root netem loss 5%"

# 2. 再次执行上述的 Direct SCP 直连 与 xkcptun 隧道测试，对比速率差距
# （直连通常会瞬间降至几十 KB/s，而 xkcptun 依靠 FEC 仍能平稳传输）

# 3. 测试完毕后清除 VPS 丢包模拟
ssh bwg "tc qdisc del dev eth0 root 2>/dev/null || true"
```

---

## ⚡ 一键切换脚本 (`switch_env.sh`)

为了方便一键切换配置，您也可以直接运行：
```bash
# 切换到 05 黄金配置并启动
./benchmark_configs/switch_env.sh 05

# 切换到 01 传统 normal 模式并启动
./benchmark_configs/switch_env.sh 01
```
