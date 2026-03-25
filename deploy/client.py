#!/usr/bin/env python3
"""
Minimal Pensieve CLI client.

Usage:
    python3 client.py <host> <port> PUT <key> <value>
    python3 client.py <host> <port> GET <key>
    python3 client.py <host> <port> DEL <key>

Examples:
    python3 client.py 127.0.0.1 11211 PUT hello world
    python3 client.py 127.0.0.1 11211 GET hello
    python3 client.py 127.0.0.1 11212 GET hello   # query a different node
    python3 client.py 127.0.0.1 11211 DEL hello
"""

import socket
import struct
import sys

GET, PUT, DEL = 1, 2, 3
STATUS_NAMES = {0: "OK", 1: "NOT_FOUND", 2: "ERROR"}


def send_request(sock, opcode, key, value=b""):
    key_bytes = key.encode() if isinstance(key, str) else key
    val_bytes = value.encode() if isinstance(value, str) else value
    header = struct.pack("<BBhI", opcode, 0, len(key_bytes), len(val_bytes))
    sock.sendall(header + key_bytes + val_bytes)


def recv_response(sock):
    hdr = b""
    while len(hdr) < 8:
        chunk = sock.recv(8 - len(hdr))
        if not chunk:
            raise ConnectionError("connection closed")
        hdr += chunk
    status, flags, reserved, value_len = struct.unpack("<BBhI", hdr)
    value = b""
    while len(value) < value_len:
        chunk = sock.recv(value_len - len(value))
        if not chunk:
            raise ConnectionError("connection closed during value read")
        value += chunk
    return status, value


def main():
    if len(sys.argv) < 4:
        print(__doc__.strip())
        sys.exit(1)

    host = sys.argv[1]
    port = int(sys.argv[2])
    op = sys.argv[3].upper()
    key = sys.argv[4] if len(sys.argv) > 4 else ""
    value = sys.argv[5] if len(sys.argv) > 5 else ""

    opcodes = {"GET": GET, "PUT": PUT, "DEL": DEL}
    if op not in opcodes:
        print(f"Unknown operation: {op}. Use GET, PUT, or DEL.")
        sys.exit(1)

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))

    send_request(sock, opcodes[op], key, value if op == "PUT" else "")
    status, resp_value = recv_response(sock)
    sock.close()

    status_name = STATUS_NAMES.get(status, f"UNKNOWN({status})")
    if op == "GET" and status == 0:
        print(f"{status_name}: {resp_value.decode(errors='replace')}")
    else:
        print(status_name)


if __name__ == "__main__":
    main()
