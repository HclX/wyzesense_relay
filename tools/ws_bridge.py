#!/usr/bin/env python3
"""Our gateway's UART to wyzesense2mqtt-rs over the network, as its `dongle_bridge` does for a stock dongle:
a WebSocket client of the server's /ws/bridge (config `bridge.enabled: true`) that relays bytes both ways.
Over that WebSocket each binary message carries host-protocol bytes, without the CH554's HID length byte
(dongle_bridge strips it), and the server reassembles packets from the stream: the UART bytes go as they are.

    uv run python tools/ws_bridge.py [--device /dev/ttyACM0] [--gateway ws://127.0.0.1:8080/ws/bridge]
                                     [--token SECRET]

Standard library only (a minimal RFC 6455 client). Reconnects every 5 s, like dongle_bridge.
"""

import argparse
import base64
import os
import select
import socket
import struct
import sys
import termios
import time
import urllib.parse

OP_CONT, OP_TEXT, OP_BINARY, OP_CLOSE, OP_PING, OP_PONG = 0, 1, 2, 8, 9, 10


def open_serial(port):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = a[1] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[3] = 0
    a[4] = a[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, a)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def ws_connect(url, query):
    u = urllib.parse.urlsplit(url)
    if u.scheme != 'ws':
        raise ValueError('only ws:// is supported')
    sock = socket.create_connection((u.hostname, u.port or 80), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    path = (u.path or '/') + '?' + urllib.parse.urlencode(query)
    sock.sendall(
        (
            f'GET {path} HTTP/1.1\r\nHost: {u.netloc}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
            f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n'
        ).encode()
    )
    reply = b''
    while b'\r\n\r\n' not in reply:
        chunk = sock.recv(1024)
        if not chunk:
            raise ConnectionError('closed during the handshake')
        reply += chunk
    head, rest = reply.split(b'\r\n\r\n', 1)
    if not head.startswith(b'HTTP/1.1 101'):
        raise ConnectionError(head.split(b'\r\n')[0].decode())
    sock.settimeout(None)
    return sock, rest


def ws_frame(op, payload):
    """A client frame: final, masked (RFC 6455 §5.3)."""
    n = len(payload)
    head = bytes([0x80 | op])
    if n < 126:
        head += bytes([0x80 | n])
    elif n < 65536:
        head += bytes([0x80 | 126]) + struct.pack('>H', n)
    else:
        head += bytes([0x80 | 127]) + struct.pack('>Q', n)
    mask = os.urandom(4)
    return head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload))


def ws_frames(buf):
    """Complete frames at the start of buf: ([(op, payload)], what is left)."""
    out = []
    while len(buf) >= 2:
        op, n, at = buf[0] & 15, buf[1] & 127, 2
        if n == 126:
            if len(buf) < 4:
                break
            n, at = struct.unpack_from('>H', buf, 2)[0], 4
        elif n == 127:
            if len(buf) < 10:
                break
            n, at = struct.unpack_from('>Q', buf, 2)[0], 10
        mask = b''
        if buf[1] & 0x80:
            mask, at = buf[at : at + 4], at + 4
        if len(buf) < at + n:
            break
        payload = buf[at : at + n]
        if mask:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        out.append((op, payload))
        buf = buf[at + n :]
    return out, buf


def relay(ser, sock, pending):
    # pending: what came with the handshake reply, which can already hold the server's first message (the
    # gateway sends its first command as soon as the WebSocket is up)
    chunk = b''
    while True:
        frames, pending = ws_frames(pending + chunk)
        for op, payload in frames:
            if op in (OP_BINARY, OP_CONT):
                os.write(ser, payload)
            elif op == OP_PING:
                sock.sendall(ws_frame(OP_PONG, payload))
            elif op == OP_CLOSE:
                sock.sendall(ws_frame(OP_CLOSE, payload[:2]))
                return 'closed by the server'
        chunk = b''
        ready, _, _ = select.select([ser, sock], [], [])
        if ser in ready:
            try:
                data = os.read(ser, 1024)
            except BlockingIOError:
                data = None
            if data == b'':
                return 'device hung up'  # unplugged, or a host gateway's pty closed
            if data:
                sock.sendall(ws_frame(OP_BINARY, data))
        if sock in ready:
            chunk = sock.recv(4096)
            if not chunk:
                return 'connection closed'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--device', '-d', default='/dev/ttyACM0')
    ap.add_argument('--gateway', '-g', default='ws://127.0.0.1:8080/ws/bridge')
    ap.add_argument('--token', '-t')
    args = ap.parse_args()
    query = {'device': args.device}
    if args.token:
        query['token'] = args.token
    while True:
        try:
            ser = open_serial(args.device)
            sock, pending = ws_connect(args.gateway, query)
            print(f'{args.device} <-> {args.gateway}', flush=True)
            try:
                why = relay(ser, sock, pending)
            finally:
                sock.close()
                os.close(ser)
            print(f'bridge ended: {why}', file=sys.stderr, flush=True)
        except KeyboardInterrupt:
            return
        except OSError as e:
            print(f'bridge failed: {e}', file=sys.stderr, flush=True)
        try:
            time.sleep(5)
        except KeyboardInterrupt:
            return


if __name__ == '__main__':
    main()
