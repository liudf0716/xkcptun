cmake_minimum_required(VERSION 3.10)

project(xkcptun C)

# Automatically update version.h from git if available
find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
	execute_process(
		COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/update_version.sh
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		OUTPUT_QUIET
		ERROR_QUIET
	)
endif()

option(AUTO_INSTALL_DEPS
	"Install missing Debian/Ubuntu packages via apt during configure"
	ON)

# Packages needed on Debian/Ubuntu (pkg-config is used to locate libevent).
set(XKCP_DEBIAN_PACKAGES pkg-config libevent-dev)

function(xkcp_apt_install)
	if(NOT AUTO_INSTALL_DEPS)
		message(FATAL_ERROR
			"Missing packages: ${ARGN}\n"
			"On Ubuntu/Debian run:\n"
			"  sudo apt-get install -y ${XKCP_DEBIAN_PACKAGES}\n"
			"Or re-run cmake with -DAUTO_INSTALL_DEPS=ON")
	endif()

	find_program(XKCP_APT_GET apt-get)
	if(NOT XKCP_APT_GET)
		message(FATAL_ERROR
			"Missing packages: ${ARGN}\n"
			"apt-get not found; install them manually:\n"
			"  ${XKCP_DEBIAN_PACKAGES}")
	endif()

	execute_process(COMMAND id -u OUTPUT_VARIABLE XKCP_UID OUTPUT_STRIP_TRAILING_WHITESPACE)
	if(XKCP_UID STREQUAL "0")
		set(XKCP_SUDO "")
	else()
		find_program(XKCP_SUDO sudo)
		if(NOT XKCP_SUDO)
			message(FATAL_ERROR
				"Need root to install: ${ARGN}\n"
				"Re-run cmake as root, or install:\n"
				"  sudo apt-get install -y ${XKCP_DEBIAN_PACKAGES}")
		endif()
	endif()

	message(STATUS "Installing missing packages with apt: ${ARGN}")
	execute_process(
		COMMAND ${XKCP_SUDO} ${XKCP_APT_GET} update
		RESULT_VARIABLE XKCP_APT_UPDATE)
	if(NOT XKCP_APT_UPDATE EQUAL 0)
		message(WARNING "apt-get update failed (continuing with install)")
	endif()

	execute_process(
		COMMAND ${XKCP_SUDO} env DEBIAN_FRONTEND=noninteractive
			${XKCP_APT_GET} install -y ${ARGN}
		RESULT_VARIABLE XKCP_APT_INSTALL)
	if(NOT XKCP_APT_INSTALL EQUAL 0)
		message(FATAL_ERROR "Failed to install packages: ${ARGN}")
	endif()
endfunction()

find_package(PkgConfig QUIET)
if(NOT PKG_CONFIG_FOUND)
	xkcp_apt_install(pkg-config)
	unset(PKG_CONFIG_EXECUTABLE CACHE)
	find_package(PkgConfig REQUIRED)
endif()

pkg_search_module(Libevent QUIET libevent)
if(NOT Libevent_FOUND)
	xkcp_apt_install(libevent-dev)
	pkg_search_module(Libevent REQUIRED libevent)
endif()
include_directories(${Libevent_INCLUDE_DIRS})

set(src_xkcp_spy
	xkcp_spy.c)

set(src_xkcp_server
	xkcp_server.c
	tcp_client.c
	udp_server.c
	debug.c
	ikcp.c
	jwHash.c
	commandline.c
	xkcp_util.c
	xkcp_auth.c
	xkcp_config.c
	toml.c
	xkcp_mon.c
	fec.c)

set(src_xkcp_client
	tcp_proxy.c
	udp_proxy.c
	xkcp_client.c
	xkcp_config.c
	xkcp_util.c
	xkcp_auth.c
	commandline.c
	debug.c
	ikcp.c
	jwHash.c
	toml.c
	xkcp_mon.c
	fec.c)

set(libs
    m
	${Libevent_LIBRARIES})

ADD_DEFINITIONS(-Wall -O2 --std=gnu99 -Wmissing-declarations)


add_executable(xkcp_spy ${src_xkcp_spy})
target_link_libraries(xkcp_spy ${link_flag} ${libs})

add_executable(test_fec test_fec.c fec.c)
add_test(NAME fec COMMAND test_fec)

add_executable(test_toml test_toml.c xkcp_config.c xkcp_auth.c toml.c debug.c)
add_test(NAME toml COMMAND test_toml)

enable_testing()

add_executable(xkcp_client ${src_xkcp_client})
target_link_libraries(xkcp_client ${link_flag} ${libs})

add_executable(xkcp_server ${src_xkcp_server})
target_link_libraries(xkcp_server ${link_flag} ${libs})

install(TARGETS xkcp_client xkcp_server xkcp_spy
        RUNTIME DESTINATION bin
)
