cmake_minimum_required(VERSION 2.6)

project(xkcptun C)

set(src_xkcp_spy
	xkcp_spy.c)

set(src_xkcp_server
	xkcp_server.c
	tcp_client.c
	debug.c
	ikcp.c
	jwHash.c
	commandline.c
	xkcp_util.c
	xkcp_config.c
	xkcp_mon.c)
	
set(src_xkcp_client
	tcp_proxy.c
	xkcp_client.c
	xkcp_config.c
	xkcp_util.c
	commandline.c
	debug.c
	ikcp.c
	jwHash.c
	xkcp_mon.c)

set(libs
	event
	json-c)

ADD_DEFINITIONS(-Wall -g  --std=gnu99 -Wmissing-declarations)

add_executable(xkcp_spy ${src_xkcp_spy})
target_link_libraries(xkcp_spy ${libs})

add_executable(xkcp_client ${src_xkcp_client})
target_link_libraries(xkcp_client ${libs})

add_executable(xkcp_server ${src_xkcp_server})
target_link_libraries(xkcp_server ${libs})

install(TARGETS xkcp_client xkcp_server
        RUNTIME DESTINATION bin
)
