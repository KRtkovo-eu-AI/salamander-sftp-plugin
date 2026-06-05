#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Testovaci SSH server emulujici SCP rezim (shell prikazy + scp protokol) v Pythonu.
Pouziva se k overeni SCP vetve SFTP pluginu na Windows (kde neni realny unix server).
Posloucha 127.0.0.1:2223, user test / heslo test, root = D:\\weby\\sftp_testroot.
"""
import os, sys, socket, threading, shlex, stat as statmod, time, paramiko

ROOT = r"D:\weby\sftp_testroot"
HOST, PORT = "127.0.0.1", 2223
USER, PASSWORD = "test", "test"
HOSTKEY_PATH = r"D:\weby\sftp_hostkey"
os.makedirs(ROOT, exist_ok=True)


def get_hostkey():
    if os.path.exists(HOSTKEY_PATH):
        return paramiko.RSAKey(filename=HOSTKEY_PATH)
    k = paramiko.RSAKey.generate(2048)
    k.write_private_key_file(HOSTKEY_PATH)
    return k


def realpath(p):
    # p je absolutni posix cesta "/..."; mapuj do ROOT
    p = p.replace("\\", "/")
    parts = [x for x in p.split("/") if x not in ("", ".")]
    safe = []
    for x in parts:
        if x == "..":
            if safe:
                safe.pop()
        else:
            safe.append(x)
    return os.path.join(ROOT, *safe)


def rwx(mode):
    s = ["d" if statmod.S_ISDIR(mode) else ("l" if statmod.S_ISLNK(mode) else "-")]
    for who in (6, 3, 0):
        s.append("r" if mode & (0o4 << who) else "-")
        s.append("w" if mode & (0o2 << who) else "-")
        s.append("x" if mode & (0o1 << who) else "-")
    return "".join(s)


def emulate_ls(path):
    rp = realpath(path)
    out = []
    try:
        names = ["."] + sorted(os.listdir(rp))
    except OSError as e:
        return None, 1
    out.append("total 0")
    for name in names:
        full = rp if name == "." else os.path.join(rp, name)
        try:
            st = os.stat(full)
        except OSError:
            continue
        # na Windows nejsou unix prava ani vlastnik -> nasimuluj rozumne
        mode = st.st_mode
        if os.path.isdir(full):
            mode = statmod.S_IFDIR | 0o755
        else:
            mode = statmod.S_IFREG | 0o644
        out.append("%s %d %s %s %d %d %s" % (
            rwx(mode), 1, "test", "test", st.st_size, int(st.st_mtime), name))
    return "\n".join(out) + "\n", 0


def emulate_stat(fmt, path):
    rp = realpath(path)
    try:
        st = os.stat(rp)
    except OSError:
        return None, 1
    mode = 0o755 if os.path.isdir(rp) else 0o644
    vals = {"%a": "%o" % mode, "%s": str(st.st_size), "%u": "0", "%g": "0", "%Y": str(int(st.st_mtime))}
    res = fmt
    for k, v in vals.items():
        res = res.replace(k, v)
    return res + "\n", 0


def scp_source(channel, path):
    # scp -f: server posila soubor klientovi
    rp = realpath(path)
    a = channel.recv(1)  # \0 od klienta = start
    sys.stderr.write(f"scp_source: start byte={a!r} path={rp}\n")
    st = os.stat(rp)
    size = st.st_size
    name = os.path.basename(rp)
    # T-radek s casy (libssh2 posila scp -pf -> ocekava preserve casy)
    tline = "T%d 0 %d 0\n" % (int(st.st_mtime), int(st.st_atime))
    channel.sendall(tline.encode())
    channel.recv(1)  # ack T
    cline = "C0644 %d %s\n" % (size, name)
    channel.sendall(cline.encode())
    ack = channel.recv(1)  # ack
    sys.stderr.write(f"scp_source: cline={cline!r} ack={ack!r} size={size}\n")
    with open(rp, "rb") as f:
        while True:
            b = f.read(32768)
            if not b:
                break
            channel.sendall(b)
    channel.sendall(b"\x00")
    fin = channel.recv(1)  # ack
    sys.stderr.write(f"scp_source: done fin={fin!r}\n")


def scp_sink(channel, path):
    # scp -t: server prijima soubor od klienta
    rp = realpath(path)
    if os.path.isdir(rp):
        pass
    channel.sendall(b"\x00")  # ready
    line = b""
    while not line.endswith(b"\n"):
        c = channel.recv(1)
        if not c:
            return
        line += c
    # C<mode> <size> <name>
    parts = line.decode().strip().split(" ", 2)
    size = int(parts[1])
    channel.sendall(b"\x00")  # ack C-line
    target = rp
    if os.path.isdir(rp):
        target = os.path.join(rp, parts[2])
    got = 0
    with open(target, "wb") as f:
        while got < size:
            b = channel.recv(min(32768, size - got))
            if not b:
                break
            f.write(b)
            got += len(b)
    channel.recv(1)  # trailing \0
    channel.sendall(b"\x00")  # ack


def run_exec(channel, cmd):
    try:
        rc = 0
        text = ""
        s = cmd.strip()
        sys.stderr.write(f"EXEC: {s!r}\n")
        if s.startswith("scp "):
            args = shlex.split(s)
            flags = "".join(a for a in args if a.startswith("-"))
            if "f" in flags:  # zdroj (download), vc. -pf
                scp_source(channel, args[-1]); channel.send_exit_status(0); channel.close(); return
            if "t" in flags:  # cil (upload), vc. -pt
                scp_sink(channel, args[-1]); channel.send_exit_status(0); channel.close(); return
        if s.startswith("if [ -d"):
            # if [ -d 'P' ]; then echo D; elif [ -e 'P' ]; then echo F; else echo N; fi
            q = shlex.split(s)
            p = q[3]  # 'P'
            rp = realpath(p)
            text = ("D" if os.path.isdir(rp) else ("F" if os.path.exists(rp) else "N")) + "\n"
        else:
            args = shlex.split(s)
            cmdname = args[0]
            rest = [a for a in args[1:] if a != "--"]
            if cmdname == "ls":
                path = rest[-1] if rest else "/"
                text, rc = emulate_ls(path)
                if text is None:
                    text, rc = "", 1
            elif cmdname == "stat":
                # stat -c FMT -- PATH  (FMT je rest[1] po -c)
                ci = args.index("-c")
                fmt = args[ci + 1]
                path = args[-1]
                text, rc = emulate_stat(fmt, path)
                if text is None:
                    text, rc = "", 1
            elif cmdname == "mkdir":
                try: os.mkdir(realpath(rest[-1]))
                except OSError: rc = 1
            elif cmdname == "rmdir":
                try: os.rmdir(realpath(rest[-1]))
                except OSError: rc = 1
            elif cmdname == "rm":
                try: os.remove(realpath(rest[-1]))
                except OSError: rc = 1
            elif cmdname == "mv":
                try: os.replace(realpath(rest[-2]), realpath(rest[-1]))
                except OSError: rc = 1
            elif cmdname == "chmod":
                rc = 0  # Windows neumi unix prava -> predstirej uspech
            elif cmdname == "echo":
                text = " ".join(rest) + "\n"
            else:
                text = ""; rc = 0
        if text:
            channel.sendall(text.encode())
        channel.send_exit_status(rc)
    except Exception as e:
        try: channel.sendall_stderr(str(e).encode())
        except Exception: pass
        try: channel.send_exit_status(1)
        except Exception: pass
    try: channel.close()
    except Exception: pass


class Server(paramiko.ServerInterface):
    def check_auth_password(self, u, p):
        return paramiko.AUTH_SUCCESSFUL if (u == USER and p == PASSWORD) else paramiko.AUTH_FAILED
    def check_channel_request(self, kind, cid):
        return paramiko.OPEN_SUCCEEDED
    def get_allowed_auths(self, u):
        return "password"
    def check_channel_exec_request(self, channel, command):
        cmd = command.decode("utf-8", "replace") if isinstance(command, bytes) else command
        threading.Thread(target=run_exec, args=(channel, cmd), daemon=True).start()
        return True


def handle(client):
    try:
        t = paramiko.Transport(client)
        t.add_server_key(get_hostkey())
        t.start_server(server=Server())
        while t.is_active():
            t.join(1)
    except Exception as e:
        sys.stderr.write(f"conn err: {e}\n")


def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((HOST, PORT)); s.listen(10)
    sys.stderr.write(f"SCP test server bezi na {HOST}:{PORT} user={USER} pass={PASSWORD} root={ROOT}\n")
    while True:
        c, a = s.accept()
        threading.Thread(target=handle, args=(c,), daemon=True).start()


if __name__ == "__main__":
    main()
