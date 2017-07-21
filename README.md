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
[10]: https://github.com/liudf0716/xkcptung/issues/new
[11]: https://img.shields.io/badge/Platform-%20OpenWRT%20%7CLEDE%20%7CCentOS%20-brightgreen.svg?style=plastic
[12]: https://github.com/gigibox/openwrt-xkcptun
[13]: https://img.shields.io/badge/KunTeng-Inside-blue.svg?style=plastic
[14]: http://rom.kunteng.org

# xkcptun 基于kcp和libevent2库，用c语言实现的kcptun

xkcptun主要应用于LEDE，openwrt中，其原理如图：

<img src="kcptun.png" alt="kcptun" height="300px"/>

### Compile

xkcptun依赖[libevent2](https://github.com/libevent/libevent)

安装libevent2库后 (apt-get install libevent-dev)

git clone https://github.com/liudf0716/xkcptun.git

cd xkcptun

mkdir build && cd build

cmake .. (camke -DBUILD_STATIC_LINK=yes .. //静态链接)

make


生成xkcp_client, xkcp_server, xkcp_spy

#### 参考文档

1, [安装libjson c的问题](https://github.com/liudf0716/xkcptun/wiki/%E5%AE%89%E8%A3%85libjson-c%E7%9A%84%E9%97%AE%E9%A2%98)

2, [bbr vs kcp  优化http下载性能对比报告](https://github.com/liudf0716/xkcptun/wiki/bbr-vs-kcp-%E4%BC%98%E5%8C%96http%E4%B8%8B%E8%BD%BD%E6%80%A7%E8%83%BD%E5%AF%B9%E6%AF%94%E6%8A%A5%E5%91%8A)

3, [如何在centos上部署xkcptun server](https://github.com/liudf0716/xkcptun/pull/11)

### OpenWrt
编译及安装请参考 [openwrt-xkcptun](https://github.com/gigibox/openwrt-xkcptun)

### QuickStart

为方便理解和使用，我们将使用场景放在同一台pc上，pc使用ubuntu系统，我们通过xkcptun来访问本机的http server

假设pc的 eth0 ip 为 192.168.199.18， http server的监听端口为80端口，xkcptun的server和client配置分别如下：

server.json 如下：
```
{
  "localinterface": "eth0",
  "localport": 9089,
  "remoteaddr": "192.168.199.18",
  "remoteport": 80,
  "key": "14789632a",
  "crypt": "none",
  "mode": "fast3",
  "mtu": 1350,
  "sndwnd": 1024,
  "rcvwnd": 1024,
  "datashard": 10,
  "parityshard": 3,
  "dscp": 0,
  "nocomp": true,
  "acknodelay": false,
  "nodelay": 0,
  "interval": 20,
  "resend": 2,
  "nc": 1,
  "sockbuf": 4194304,
  "keepalive": 10
}
```

client.json如下：
```
{
  "localinterface": "eth0",
  "localport": 9088,
  "remoteaddr": "192.168.199.18",
  "remoteport": 9089,
  "key": "14789632a",
  "crypt": "none",
  "mode": "fast3",
  "mtu": 1350,
  "sndwnd": 1024,
  "rcvwnd": 1024,
  "datashard": 10,
  "parityshard": 3,
  "dscp": 0,
  "nocomp": true,
  "acknodelay": false,
  "nodelay": 0,
  "interval": 20,
  "resend": 2,
  "nc": 1,
  "sockbuf": 4194304,
  "keepalive": 10
}
```

分别运行：

xkcp_server -c server.json -f -d 7

xkcp_client -c client.json -f -d 7


[注] 以上命令都是运行在debug和前台运行模式，正式部署的时候要把 -f 去掉， -d 0 如： xkcp_server -c server.json -d 0

curl http://192.168.199.18:9088

其执行效果与curl http://192.168.199.18 等同


xkcp_spy -h 192.168.199.18 -s -t status

查看服务器端的情况

xkcp_spy -h 192.168.199.18 -c -t status

查看客户端的情况

### Todo

Compatible with [kcptun](https://github.com/xtaci/kcptun)  <img src="https://github.com/xtaci/kcptun/blob/master/logo-small.png" alt="kcptun" height="24px" /> 



### How to contribute our project(给本项目做贡献)


欢迎大家给本项目提供意见和贡献，提供意见的方法可以在本项目的[Issues](https://github.com/liudf0716/xkcptun/issues/new)提，更加欢迎给项目提PULL REQUEST，具体提交PR的方法请参考[CONTRIBUTING](https://github.com/liudf0716/xkcptun/blob/master/CONTRIBUTING.md)


### Contact me 

QQ群 ： [331230369](https://jq.qq.com/?_wv=1027&k=47QGEhL)


## Please support us and star our project
