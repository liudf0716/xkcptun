# xkcptun 基于kcp和libevent2库，用c语言实现的kcptun

xkcptun主要应用于LEDE，openwrt中，其原理如图：

<img src="kcptun.png" alt="kcptun" height="300px"/>

### Compile

xkcptun依赖[libevent2](https://github.com/libevent/libevent)和[json-c](https://github.com/json-c/json-c)

安装libevent2和json-c库后

git clone https://github.com/liudf0716/xkcptun.git

cd xkcptun

mkdir build && cd build

cmake ..

生成xkcp_client和xkcp_server

### QuickStart

