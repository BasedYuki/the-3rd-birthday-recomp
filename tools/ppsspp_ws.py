"""Minimal client for PPSSPP's WebSocket debugger (stdlib only).

PPSSPP is the reference oracle: drive it to the same point as the port and compare
thread states, memory and HLE call logs.

Enable with RemoteDebuggerOnStartup = True and a fixed RemoteISOPort in ppsspp.ini.

    from ppsspp_ws import Dbg
    d = Dbg(45679)
    d.request("cpu.status")
    d.press("cross")
    for msg in d.drain(2.0): ...   # broadcasts (log lines etc.)
"""

import base64
import json
import os
import socket
import struct
import time


class Dbg:
    def __init__(self, port, host="127.0.0.1", timeout=10.0):
        self.s = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (f"GET /debugger HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
               f"Sec-WebSocket-Protocol: debugger.ppsspp.org\r\nSec-WebSocket-Version: 13\r\n\r\n")
        self.s.sendall(req.encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            resp += self.s.recv(4096)
        head, self.buf = resp.split(b"\r\n\r\n", 1)
        if b" 101 " not in head.split(b"\r\n")[0]:
            raise RuntimeError(head.decode(errors="replace"))
        self.ticket = 0
        self.pending = []

    # ---- framing ----
    def _send(self, text):
        data = text.encode()
        hdr = bytearray([0x81])
        n = len(data)
        if n < 126:
            hdr.append(0x80 | n)
        elif n < 65536:
            hdr.append(0x80 | 126); hdr += struct.pack(">H", n)
        else:
            hdr.append(0x80 | 127); hdr += struct.pack(">Q", n)
        mask = os.urandom(4)
        hdr += mask
        self.s.sendall(bytes(hdr) + bytes(b ^ mask[i & 3] for i, b in enumerate(data)))

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(65536)
            if not chunk:
                raise ConnectionError("closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _recv(self):
        msg = b""
        while True:
            b0, b1 = self._read(2)
            n = b1 & 0x7F
            if n == 126: n = struct.unpack(">H", self._read(2))[0]
            elif n == 127: n = struct.unpack(">Q", self._read(8))[0]
            payload = self._read(n)
            op = b0 & 0x0F
            if op == 9:                     # ping -> pong
                continue
            msg += payload
            if b0 & 0x80:
                return json.loads(msg.decode(errors="replace")) if op in (0, 1) else None

    # ---- API ----
    def request(self, event, timeout=10.0, expect=None, **params):
        """Send a request and wait for its reply. `expect` also accepts a broadcast of that
        event name as the answer (cpu.stepping/cpu.resume reply that way in v1.20)."""
        self.ticket += 1
        t = str(self.ticket)
        self._send(json.dumps(dict(event=event, ticket=t, **params)))
        end = time.time() + timeout
        self.s.settimeout(timeout)
        while time.time() < end:
            m = self._recv()
            if m is None:
                continue
            if m.get("ticket") == t or (expect and m.get("event") == expect):
                if m.get("event") == "error":
                    raise RuntimeError(m.get("message"))
                return m
            self.pending.append(m)
        raise TimeoutError(event)

    def drain(self, seconds):
        """Collect broadcast messages for `seconds`."""
        out, self.pending = self.pending, []
        end = time.time() + seconds
        while True:
            left = end - time.time()
            if left <= 0:
                break
            self.s.settimeout(left)
            try:
                m = self._recv()
            except (socket.timeout, TimeoutError):
                break
            if m is not None:
                out.append(m)
        return out

    def press(self, button, duration=6):   # duration in frames
        return self.request("input.buttons.press", button=button, duration=duration)
