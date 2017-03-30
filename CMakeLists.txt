cmake_minimum_required(VERSION 2.6)

project(xkcp_client C)

set(src_xkcp_client
	main.c
	tcp_proxy.c
	xkcp_client.c
	xkcp_config.c
	xkcp_util.c
	commandline.c
	debug.c
	ikcp.c)

set(libs
	event
	json-c)

ADD_DEFINITIONS(-O2 -Wall --std=gnu99 -Wmissing-declarations)

add_executable(xkcp_client ${src_xkcp_client})
target_link_libraries(xkcp_client ${libs})

install(TARGETS xkcp_client
        RUNTIME DESTINATION bin
)
