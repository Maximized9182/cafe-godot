"""Background client for MochaPayload's TCP shell.

Broadcasts the UDP beacon on 4445, accepts the Wii U's connection on TCP
7965 (reconnecting whenever it drops), appends everything the console sends
to LOG, and forwards each line written to the FIFO CMD as a shell command.

Usage: python3 mocha_logger.py LOG CMD
Send a command: echo "iosu reboot" > CMD
"""
import os
import socket
import sys
import threading
import time

TCP_PORT = 7965
UDP_BEACON_PORT = 4445
BEACON = b"WIIU_TCP_SYSLOG_BEACON_V1"

log_path, fifo_path = sys.argv[1], sys.argv[2]
if not os.path.exists(fifo_path):
    os.mkfifo(fifo_path)

log = open(log_path, "a", buffering=1, encoding="utf-8")
lock = threading.Lock()
client = None
# Commands sent but not yet echoed back. The console echoes each command
# before its output; that echo is dropped so the command shows up only once.
pending_echoes = []


def note(msg):
    log.write(f"[mocha_logger {time.strftime('%H:%M:%S')}] {msg}\n")


def beacon():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    while True:
        if client is None:
            try:
                s.sendto(BEACON, ("<broadcast>", UDP_BEACON_PORT))
            except OSError:
                pass
        time.sleep(0.5)


def heartbeat(sock):
    # A NUL byte every second keeps the console's watchdog from dropping us.
    while True:
        time.sleep(1.0)
        with lock:
            if client is not sock:
                return
        try:
            sock.sendall(b"\x00")
        except OSError:
            return


def commands():
    while True:
        with open(fifo_path, "r") as fifo:
            for line in fifo:
                cmd = line.strip()
                if not cmd:
                    continue
                with lock:
                    sock = client
                if sock is None:
                    note(f"not connected, dropped command: {cmd}")
                    continue
                try:
                    with lock:
                        pending_echoes.append(cmd)
                    sock.sendall((cmd + "\r\n").encode())
                    note(f"sent: {cmd}")
                except OSError as e:
                    note(f"send failed ({e}): {cmd}")


def serve():
    global client
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", TCP_PORT))
    server.listen(1)
    note(f"listening on {TCP_PORT}")
    while True:
        sock, addr = server.accept()
        with lock:
            client = sock
            pending_echoes.clear()
        note(f"connected: {addr[0]}")
        # A console that was power-cycled never closes its side. Drop the
        # connection once sent data (the heartbeat) stays unacknowledged for 5 s,
        # so the logger can accept the console's next connection.
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_USER_TIMEOUT, 5000)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        threading.Thread(target=heartbeat, args=(sock,), daemon=True).start()
        sock.settimeout(1.0)
        buf = ""
        while True:
            try:
                data = sock.recv(4096)
            except socket.timeout:
                continue
            except OSError as e:
                note(f"connection error: {e}")
                break
            if not data:
                note("console closed the connection")
                break
            buf += data.decode("utf-8", "replace").replace("\x00", "")
            while "\r" in buf:
                line, buf = buf.split("\r", 1)
                line = line.lstrip("\n")
                # The shell's prompt ("# ") is glued to the next line.
                if line.startswith("# "):
                    line = line[2:]
                with lock:
                    if pending_echoes and line.strip() == pending_echoes[0]:
                        pending_echoes.pop(0)
                        continue
                log.write(line + "\n")
        if buf:
            log.write(buf + "\n")
        with lock:
            client = None
        try:
            sock.close()
        except OSError:
            pass


threading.Thread(target=beacon, daemon=True).start()
threading.Thread(target=commands, daemon=True).start()
serve()
