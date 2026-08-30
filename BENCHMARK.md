# xkcptun vs kcptun vs 直连 对比测试方案

## 1. 测试通道（均已就绪）

| 编号 | 通道 | 链路 | 命令入口 |
|---|---|---|---|
| A | **直连** | 本机 → TCP 27334 → bwg sshd | `ssh bwg` |
| B | **xkcptun** | 本机 TCP:2222 →(KCP/UDP 9089)→ bwg → sshd:27334 | `ssh -p 2222 root@192.168.8.226` |
| C | **kcptun (Go)** | 本机 TCP:2223 →(KCP/UDP 9999)→ bwg → sshd:27334 | `ssh -p 2223 root@127.0.0.1` |

### 参数对齐声明（评估时必须知道）

两端共同的 KCP 参数：`mode=fast3, mtu=1350, sndwnd=rcvwnd=1024, crypt=none, nocomp=true`。

- C 通道 kcptun 开着 FEC（datashard=10, parityshard=3，kcptun 默认值）
- B 通道 xkcptun 当前 `fec=0, losscntl=0`（这两个开关可对比，见 §5）

基线链路：RTT ≈ 171ms，ICMP 丢包率 5–10%（时段波动大），bwg 已开 BBR+fq，
`net.core.rmem_max/wmem_max` 已调至 16MB（临时 sysctl，重启失效，需持久化到
`/etc/sysctl.d/`）。

## 2. 测试矩阵

| # | 项目 | 方法 | 指标 | 预期区分度 |
|---|---|---|---|---|
| T1 | 单流吞吐 | 32MB dd，每通道 5 轮交叉执行 | 耗时中位数 + 最大/最小 | 核心指标 |
| T2 | 交互延迟 | ControlMaster 复用下连续执行 20 次 `true` | 每条命令毫秒数（中位 + P95） | 打字手感 |
| T3 | 丢包敏感性 | tc netem 注入 5%/10%/20%/30% 丢包，重复 T1 | 各丢包率下的吞吐曲线 | KCP 方案的核心价值 |
| T4 | 长稳 | 单次 512MB 传输 | 总耗时 + 期间 spy 采样重传率 | 有无恶性退化 |
| T5 | 并发流 | 4 条并行 dd 各 16MB | 总完成时间 | 队列/调度的公平性 |

## 3. 标准化操作步骤

### 准备（每次测试会话开始前）

```bash
# 0) 确认三个通道存活
ssh bwg true && ssh -p 2222 root@192.168.8.226 true && ssh -p 2223 root@127.0.0.1 true

# 1) 记录链路基线（结论要和它对照）
ping -c 30 -i 0.2 172.96.252.145 | tail -2

# 2) 建立复用连接（T2 必需；T1/T3 用不用均可，但要全程一致）
ssh -o ControlMaster=auto -o ControlPath=/tmp/mA -o ControlPersist=1800 -fN bwg
ssh -o ControlMaster=auto -o ControlPath=/tmp/mB -o ControlPersist=1800 -fN -p 2222 root@192.168.8.226
ssh -o ControlMaster=auto -o ControlPath=/tmp/mC -o ControlPersist=1800 -fN -p 2223 root@127.0.0.1
```

### T1 单流吞吐（三通道交叉执行，消除时段漂移）

```bash
run() {  # $1=标签 $2=ssh参数
  local t0=$(date +%s.%N)
  ssh $2 'dd if=/dev/zero bs=1M count=32 2>/dev/null' | wc -c
  local t1=$(date +%s.%N)
  echo "$1 $(echo "$t1 - $t0" | bc)s"
}
for i in 1 2 3 4 5; do
  run direct  "-o ControlPath=/tmp/mA bwg"
  run xkcptun "-o ControlPath=/tmp/mB -p 2222 root@192.168.8.226"
  run kcptun  "-o ControlPath=/tmp/mC -p 2223 root@127.0.0.1"
done
```

判定规则：记录三轮的**中位数**和**最差值**。历史教训：单轮直连曾从 26s 崩到
4m17s，所以"最差值"比均值更能反映隧道价值。

### T2 交互延迟

```bash
lat() { time (for i in $(seq 20); do ssh -o ControlPath=$1 <目标> true; done); }
lat /tmp/mA   # direct
lat /tmp/mB   # xkcptun
lat /tmp/mC   # kcptun
```

判定规则：比较 20 次的中位数和 P95（去掉 3 个最差后取 max）。
注意 mux 复用是必要前提，否则测的是握手不是延迟。

### T3 丢包敏感性（最有区分度的实验）

在**本机 br0 出方向**对 VPS IP 加 netem（影响三条通道与直连，保证公平）：

```bash
# 每档丢包率下各跑一轮 T1，测完立即删除
for LOSS in 5 10 20 30; do
  sudo tc qdisc add dev br0 root netem loss ${LOSS}% delay 100ms
  echo "=== loss ${LOSS}% ==="
  run direct "-o ControlPath=/tmp/mA bwg"
  run xkcptun "-o ControlPath=/tmp/mB -p 2222 root@192.168.8.226"
  run kcptun "-o ControlPath=/tmp/mC -p 2223 root@127.0.0.1"
  sudo tc qdisc del dev br0 root
done
```

判定规则：直连吞吐应随丢包率断崖式下降；两条 KCP 通道下降越平缓越好。
警告：测试期间该 IP 的所有流量（含 ssh 本身）都会变慢，测完务必 `del`。

### T4 长稳（512MB）

```bash
for ch in "direct|-o ControlPath=/tmp/mA bwg" \
          "xkcptun|-o ControlPath=/tmp/mB -p 2222 root@192.168.8.226" \
          "kcptun|-o ControlPath=/tmp/mC -p 2223 root@127.0.0.1"; do
  tag=${ch%%|*}; args=${ch#*|}
  /usr/bin/time -f "$tag %es" ssh $args 'dd if=/dev/zero bs=1M count=512 2>/dev/null' | wc -c
done
# 传输中途采样 xkcptun 会话健康度（另开终端，第 30 秒执行 2-3 次）：
cd /media/windows/github/xkcptun/build && ./xkcp_spy -h 192.168.8.226 -c -t status
```

健康标准：`srtt` 稳定在 170–300ms、`retrans` < 30%、无 `state [-1]` 会话残留、
服务端 `wc -l server.log` 不再增长（0 = 无 EAGAIN 丢包）。

### T5 并发流

```bash
par() {  # 4×16MB 并行，$1=ssh参数
  for j in 1 2 3 4; do
    ssh $1 'dd if=/dev/zero bs=1M count=16 2>/dev/null' | wc -c &
  done; wait
}
time par "-o ControlPath=/tmp/mA bwg"; time par "-o ControlPath=/tmp/mB -p 2222 root@192.168.8.226"; time par "-o ControlPath=/tmp/mC -p 2223 root@127.0.0.1"
```

## 4. 结果记录表模板

| 指标 | 直连 | xkcptun | kcptun |
|---|---|---|---|
| T1 中位 / 最差 (s) | | | |
| T2 中位 / P95 (ms) | | | |
| T3 吞吐@5/10/20/30% | | | |
| T4 512MB (s) | | | |
| T5 4 并行 (s) | | | |

## 5. 附加 A/B 开关（可选深入项）

xkcptun 两个新特性默认关闭，追平 kcptun 后值得单独验证其增益：

1. **FEC**：两端 json 置 `"fec": 1`（部分组 100ms 刷 parity + 自适应比例），
   预期改善 T2 交互延迟尾部和高丢包场景（T3 的 20/30% 档）
2. **losscntl（AIMD 限窗）**：两端置 `"losscntl": 1`，预期改善 T4 长稳的
   重传率，代价是吞吐峰值可能小幅回落

每次只动一个开关，改完两端都要改并重启（客户端 `pkill -f "^\./xkcp_client"`
后于 build 目录重启；服务端同法），否则数据不可比。

## 6. 环境清理

- netem 用完必须 `sudo tc qdisc del dev br0 root`
- 复用连接：`ssh -O exit -o ControlPath=/tmp/mX <目标>`
- bwg 上的对照服务：`pkill -f kcptun_server`（UDP 9999，不用可清）
