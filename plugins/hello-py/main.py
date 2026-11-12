#!/usr/bin/env python3
"""Minimal brickdesk plugin in Python.

It speaks the same newline-delimited JSON protocol as the C hello plugin:
host.hello -> plugin.ready, plugin.ui.set for declarative UI, and a
shared-memory RGBA frame announced with plugin.tile.frame.
"""

import argparse
import json
import mmap
import os
import select
import socket
import struct
import sys
import time

API_VERSION = 1
FRAME_MAGIC = 0x4C544442  # "BDTL"
FRAME_VERSION = 1
FRAME_FORMAT_RGBA8888 = 1
FRAME_SIZE = 168


def send(sock, message):
    data = json.dumps(message, separators=(",", ":")).encode("utf-8") + b"\n"
    sock.sendall(data)


def notify(sock, method, params):
    send(sock, {"jsonrpc": "2.0", "method": method, "params": params})


def tile_tree(show_meta):
    children = [
        {
            "type": "row",
            "gap": 8,
            "children": [
                {"type": "icon", "name": "python"},
                {"type": "text", "text": "Hello Python", "style": "title"},
            ],
        }
    ]
    if show_meta:
        children.append(
            {"type": "text", "text": "Same JSON protocol", "style": "caption"}
        )
    children.append({"type": "progress", "value": 38, "max": 100})
    children.append({"type": "text", "text": "any language", "style": "caption"})
    return {"type": "column", "padding": 14, "gap": 8, "children": children}


SETTINGS_TREE = {
    "type": "column",
    "padding": 20,
    "gap": 14,
    "children": [
        {"type": "text", "text": "Hello Python settings", "style": "title"},
        {
            "type": "toggle",
            "id": "show_meta",
            "label": "Show meta",
            "value": True,
        },
        {
            "type": "slider",
            "id": "pace",
            "label": "Pace",
            "value": 3,
            "min": 1,
            "max": 5,
            "step": 1,
        },
        {
            "type": "select",
            "id": "mode",
            "label": "Mode",
            "value": "calm",
            "options": ["calm", "bright", "night"],
        },
        {
            "type": "button",
            "id": "refresh",
            "label": "Refresh",
            "action": "refresh",
        },
    ],
}


def connect(path, timeout_s=5.0):
    deadline = time.monotonic() + timeout_s
    while True:
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(path)
            return sock
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.01)


def paint(pixels):
    for y in range(FRAME_SIZE):
        for x in range(FRAME_SIZE):
            index = (y * FRAME_SIZE + x) * 4
            tx = x / (FRAME_SIZE - 1)
            ty = y / (FRAME_SIZE - 1)
            pixels[index + 0] = int(32 + 80 * tx + 60 * (1.0 - ty))
            pixels[index + 1] = int(46 + 96 * ty)
            pixels[index + 2] = int(78 + 120 * tx * ty)
            pixels[index + 3] = 255


def read_line(sock, buffer):
    while b"\n" not in buffer:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            return None, buffer
        if not chunk:
            return b"", buffer
        buffer += chunk
    line, buffer = buffer.split(b"\n", 1)
    return line, buffer


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", default=os.environ.get("BRICKDESK_PLUGIN_SOCKET"))
    args = parser.parse_args()
    if not args.socket:
        print("hello-py: no socket", file=sys.stderr)
        return 2

    sock = connect(args.socket)
    sock.settimeout(5.0)
    buffer = b""
    surface_path = ""
    while True:
        line, buffer = read_line(sock, buffer)
        if line is None:
            continue
        if not line:
            return 1
        message = json.loads(line)
        if message.get("method") == "host.hello":
            surface_path = message.get("params", {}).get("surface", "")
            send(
                sock,
                {
                    "jsonrpc": "2.0",
                    "id": message.get("id", 1),
                    "result": {
                        "pid": os.getpid(),
                        "api": API_VERSION,
                        "capabilities": ["tile", "settings"],
                    },
                },
            )
            break

    frame = None
    header = None
    if surface_path:
        fd = os.open(surface_path, os.O_RDWR)
        size = 32 + 400 * 400 * 4
        frame = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
        os.close(fd)
        header = frame
        pixels = bytearray(FRAME_SIZE * FRAME_SIZE * 4)
        paint(pixels)
        header[0:32] = struct.pack(
            "<8I",
            FRAME_MAGIC,
            FRAME_VERSION,
            FRAME_SIZE,
            FRAME_SIZE,
            FRAME_SIZE * 4,
            FRAME_FORMAT_RGBA8888,
            0,
            0,
        )
        header[32 : 32 + len(pixels)] = pixels
        # Write seq last so the host only sees completed frames.
        header[24:28] = struct.pack("<I", 1)
        notify(sock, "plugin.tile.frame", {"seq": 1})

    show_meta = True
    notify(sock, "plugin.ui.set", {"tile": tile_tree(show_meta), "settings": SETTINGS_TREE})
    sock.settimeout(0.5)

    while True:
        line, buffer = read_line(sock, buffer)
        if line is None:
            continue
        if not line:
            break
        message = json.loads(line)
        method = message.get("method")
        if method == "host.ping":
            send(
                sock,
                {
                    "jsonrpc": "2.0",
                    "id": message.get("id", 0),
                    "result": {"pong": True},
                },
            )
        elif method == "host.shutdown":
            break
        elif method == "host.event":
            params = message.get("params", {})
            if params.get("type") == "settings.changed" and params.get("key") == "show_meta":
                show_meta = bool(params.get("value", True))
                notify(sock, "plugin.ui.set", {"tile": tile_tree(show_meta)})
            elif params.get("type") == "tile.visible":
                if frame is not None:
                    header[24:28] = struct.pack("<I", 2)
                    notify(sock, "plugin.tile.frame", {"seq": 2})

    if frame is not None:
        frame.close()
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
