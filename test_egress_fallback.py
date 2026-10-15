#!/usr/bin/env python3
"""
Integration test for xkcptun egress_proxy and automatic fallback to direct connection.
Tests:
1. Normal egress proxy routing through SOCKS5 proxy (Colab alive).
2. Automatic fallback to direct target connection when SOCKS5 proxy is dead (Colab disconnected).
3. Automatic resumption of egress proxy when SOCKS5 proxy comes back up (Colab reconnected).
"""

import socket
import threading
import time
import subprocess
import os
import sys

TARGET_PORT = 19001
SOCKS5_PORT = 11080
XKCP_SERVER_PORT = 19089
XKCP_CLIENT_PORT = 12345

XKCP_SERVER_BIN = "/media/windows/github/xkcptun/build-x86/xkcp_server"
XKCP_CLIENT_BIN = "/media/windows/github/xkcptun/build-x86/xkcp_client"

stop_target = False
stop_socks5 = False
socks5_hits = 0
direct_hits = 0

def target_server():
    """Simple target server that echoes and identifies itself"""
    global stop_target, direct_hits
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", TARGET_PORT))
    s.listen(5)
    s.settimeout(1.0)
    while not stop_target:
        try:
            conn, addr = s.accept()
            direct_hits += 1
            data = conn.recv(1024)
            conn.sendall(b"DIRECT_RESPONSE: " + data)
            conn.close()
        except socket.timeout:
            pass
    s.close()

def socks5_server():
    """Minimal SOCKS5 proxy implementation for testing"""
    global stop_socks5, socks5_hits
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", SOCKS5_PORT))
    s.listen(5)
    s.settimeout(1.0)
    while not stop_socks5:
        try:
            conn, addr = s.accept()
            socks5_hits += 1
            # 1. Greeting
            greet = conn.recv(256)
            conn.sendall(b"\x05\x00")
            # 2. Connect request
            req = conn.recv(256)
            # Response: success
            conn.sendall(b"\x05\x00\x00\x01\x7f\x00\x00\x01\x00\x50")
            # Read client data
            cdata = conn.recv(1024)
            # Forward modified data indicating it came through proxy
            conn.sendall(b"PROXY_RESPONSE: " + cdata)
            conn.close()
        except socket.timeout:
            pass
    s.close()

def main():
    global stop_target, stop_socks5, socks5_hits, direct_hits

    # 1. Start target server
    t_target = threading.Thread(target=target_server, daemon=True)
    t_target.start()

    # 2. Start mock SOCKS5 server
    t_socks5 = threading.Thread(target=socks5_server, daemon=True)
    t_socks5.start()
    time.sleep(0.5)

    # 3. Create server and client TOML configs
    server_toml = "/tmp/test_server_egress.toml"
    with open(server_toml, "w") as f:
        f.write(f"""
[global]
local_port = {XKCP_SERVER_PORT}
key = "test-secret"
mode = "fast3"
egress_proxy = "127.0.0.1:{SOCKS5_PORT}"
egress_fallback_direct = true
""")

    client_toml = "/tmp/test_client_egress.toml"
    with open(client_toml, "w") as f:
        f.write(f"""
[global]
remote_addr = "127.0.0.1"
remote_port = {XKCP_SERVER_PORT}
key = "test-secret"
mode = "fast3"

[[tunnel]]
name = "test_redir"
local_port = {XKCP_CLIENT_PORT}
target_addr = "127.0.0.1"
target_port = {TARGET_PORT}
dynamic_target = 1
""")

    p_server = subprocess.Popen([XKCP_SERVER_BIN, "-c", server_toml, "-f", "-d", "1"])
    time.sleep(0.5)
    p_client = subprocess.Popen([XKCP_CLIENT_BIN, "-c", client_toml, "-f", "-d", "1"])
    time.sleep(1.0)

    try:
        # TEST 1: Colab SOCKS5 Proxy is ALIVE
        print("\n=== TEST 1: Egress Proxy Online (Via SOCKS5) ===")
        socks5_hits = 0
        direct_hits = 0
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(("127.0.0.1", XKCP_CLIENT_PORT))
        s.sendall(b"PING1")
        s.settimeout(3.0)
        resp1 = s.recv(1024)
        s.close()
        print(f"Response: {resp1}")
        assert b"PROXY_RESPONSE: PING1" in resp1, f"Expected PROXY_RESPONSE, got: {resp1}"
        assert socks5_hits == 1
        print("✔ TEST 1 PASSED: Successfully routed through SOCKS5 proxy!")

        # TEST 2: Colab SOCKS5 Proxy GOES DOWN (simulate disconnect)
        print("\n=== TEST 2: Egress Proxy Offline -> Automatic Fallback to Direct ===")
        stop_socks5 = True
        t_socks5.join()
        time.sleep(0.5)

        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(("127.0.0.1", XKCP_CLIENT_PORT))
        s.sendall(b"PING2")
        s.settimeout(3.0)
        resp2 = s.recv(1024)
        s.close()
        print(f"Response: {resp2}")
        assert b"DIRECT_RESPONSE: PING2" in resp2, f"Expected DIRECT_RESPONSE, got: {resp2}"
        assert direct_hits == 1
        print("✔ TEST 2 PASSED: Automatically fell back to direct target connection with ZERO loss!")

        # TEST 3: Colab SOCKS5 Proxy COMES BACK UP (simulate reconnect)
        print("\n=== TEST 3: Egress Proxy Restored -> Auto Resumption of Proxy ===")
        stop_socks5 = False
        socks5_hits = 0
        t_socks5 = threading.Thread(target=socks5_server, daemon=True)
        t_socks5.start()
        time.sleep(0.5)

        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(("127.0.0.1", XKCP_CLIENT_PORT))
        s.sendall(b"PING3")
        s.settimeout(3.0)
        resp3 = s.recv(1024)
        s.close()
        print(f"Response: {resp3}")
        assert b"PROXY_RESPONSE: PING3" in resp3, f"Expected PROXY_RESPONSE, got: {resp3}"
        assert socks5_hits == 1
        print("✔ TEST 3 PASSED: Automatically resumed using SOCKS5 proxy without restarting xkcp_server!")

        print("\n=======================================================")
        print("🎉 ALL INTEGRATION TESTS PASSED PERFECTLY!")
        print("=======================================================")

    finally:
        p_client.terminate()
        p_server.terminate()
        stop_target = True
        stop_socks5 = True
        if os.path.exists(server_toml): os.remove(server_toml)
        if os.path.exists(client_toml): os.remove(client_toml)

if __name__ == "__main__":
    main()
