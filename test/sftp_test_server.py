#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Minimalni lokalni SFTP server pro testovani Salamander SFTP pluginu.
Posloucha na 127.0.0.1:2222, uzivatel test / heslo test, serveruje ROOT slozku.
Bez adminu, bez systemovych zmen. Zaloha na paramiko demo StubSFTPServer.
"""
import os, sys, socket, threading, paramiko
paramiko.util.log_to_file(r"D:\weby\sftp_paramiko.log", level="DEBUG")

ROOT = r"D:\weby\sftp_testroot"
HOST, PORT = "127.0.0.1", int(os.environ.get("SFTP_PORT", "2222"))
USER, PASSWORD = "test", "test"
HOSTKEY_PATH = r"D:\weby\sftp_hostkey"
ED25519_HOSTKEY_PATH = r"D:\weby\sftp_hostkey_ed25519"
MODERN_ONLY = os.environ.get("SFTP_MODERN_ONLY", "0") == "1"

os.makedirs(ROOT, exist_ok=True)

def get_hostkey():
    if os.path.exists(HOSTKEY_PATH):
        return paramiko.RSAKey(filename=HOSTKEY_PATH)
    key = paramiko.RSAKey.generate(2048)
    key.write_private_key_file(HOSTKEY_PATH)
    return key

def get_ed25519_hostkey():
    if os.path.exists(ED25519_HOSTKEY_PATH):
        return paramiko.Ed25519Key(filename=ED25519_HOSTKEY_PATH)
    # paramiko neumí generovat Ed25519 do souboru -> použij cryptography
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    from cryptography.hazmat.primitives import serialization
    pk = Ed25519PrivateKey.generate()
    data = pk.private_bytes(serialization.Encoding.PEM,
                            serialization.PrivateFormat.OpenSSH,
                            serialization.NoEncryption())
    with open(ED25519_HOSTKEY_PATH, "wb") as f:
        f.write(data)
    return paramiko.Ed25519Key(filename=ED25519_HOSTKEY_PATH)

def _load_authorized_key():
    try:
        with open(r"D:\weby\sftp_testkey.pub") as f:
            parts = f.read().split()
            return paramiko.RSAKey(data=__import__("base64").b64decode(parts[1]))
    except Exception:
        return None
AUTHORIZED_KEY = _load_authorized_key()

KBI_ONLY = os.environ.get("SFTP_KBI_ONLY", "0") == "1"  # jen keyboard-interactive

class Server(paramiko.ServerInterface):
    def check_auth_password(self, username, password):
        if KBI_ONLY:
            return paramiko.AUTH_FAILED  # vynutí keyboard-interactive
        if username == USER and password == PASSWORD:
            return paramiko.AUTH_SUCCESSFUL
        return paramiko.AUTH_FAILED
    def check_auth_publickey(self, username, key):
        if username == USER and AUTHORIZED_KEY is not None and key == AUTHORIZED_KEY:
            return paramiko.AUTH_SUCCESSFUL
        return paramiko.AUTH_FAILED
    def check_auth_interactive(self, username, submethods):
        if username == USER:
            return paramiko.InteractiveQuery("Autentizace", "Zadejte heslo", ("Heslo: ", False))
        return paramiko.AUTH_FAILED
    def check_auth_interactive_response(self, responses):
        if responses and responses[0] == PASSWORD:
            return paramiko.AUTH_SUCCESSFUL
        return paramiko.AUTH_FAILED
    def check_channel_request(self, kind, chanid):
        return paramiko.OPEN_SUCCEEDED
    def get_allowed_auths(self, username):
        return "keyboard-interactive" if KBI_ONLY else "password,publickey,keyboard-interactive"
    def check_channel_exec_request(self, channel, command):
        cmd = command.decode("utf-8", "replace") if isinstance(command, bytes) else command
        threading.Thread(target=_run_exec, args=(channel, cmd), daemon=True).start()
        return True

def _run_exec(channel, cmd):
    import subprocess
    try:
        r = subprocess.run(cmd, shell=True, capture_output=True, timeout=20)
        if r.stdout:
            channel.sendall(r.stdout)
        if r.stderr:
            channel.sendall_stderr(r.stderr)
        channel.send_exit_status(r.returncode)
    except Exception as e:
        try:
            channel.sendall_stderr(str(e).encode())
        except Exception:
            pass
    try:
        channel.close()
    except Exception:
        pass

class StubSFTPHandle(paramiko.SFTPHandle):
    def stat(self):
        try:
            return paramiko.SFTPAttributes.from_stat(os.fstat(self.readfile.fileno()))
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)
    def chattr(self, attr):
        return paramiko.SFTP_OK

class StubSFTPServer(paramiko.SFTPServerInterface):
    ROOT = ROOT
    PERMS = {}  # simulace unix práv (Windows je neumí) path->mode
    def _realpath(self, path):
        return self.ROOT + self.canonicalize(path)
    def list_folder(self, path):
        p = self._realpath(path)
        try:
            out = []
            for fname in os.listdir(p):
                attr = paramiko.SFTPAttributes.from_stat(os.stat(os.path.join(p, fname)))
                attr.filename = fname
                out.append(attr)
            return out
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)
    def stat(self, path):
        try:
            a = paramiko.SFTPAttributes.from_stat(os.stat(self._realpath(path)))
            rp = self._realpath(path)
            if rp in StubSFTPServer.PERMS:  # simulace unix práv
                a.st_mode = (a.st_mode & ~0o777) | StubSFTPServer.PERMS[rp]
            return a
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)
    def lstat(self, path):
        try:
            return paramiko.SFTPAttributes.from_stat(os.lstat(self._realpath(path)))
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)
    def open(self, path, flags, attr):
        p = self._realpath(path)
        try:
            binary_flag = getattr(os, "O_BINARY", 0)
            flags |= binary_flag
            mode = getattr(attr, "st_mode", None) or 0o666
            fd = os.open(p, flags, mode)
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)
        if flags & os.O_CREAT and attr is not None:
            attr._flags &= ~attr.FLAG_PERMISSIONS
            paramiko.SFTPServer.set_file_attr(p, attr)
        if flags & os.O_WRONLY:
            fstr = "ab" if (flags & os.O_APPEND) else "wb"
        elif flags & os.O_RDWR:
            fstr = "a+b" if (flags & os.O_APPEND) else "r+b"
        else:
            fstr = "rb"
        try:
            f = os.fdopen(fd, fstr)
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)
        fobj = StubSFTPHandle(flags)
        fobj.filename = p
        fobj.readfile = f
        fobj.writefile = f
        return fobj
    def remove(self, path):
        try:
            os.remove(self._realpath(path))
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)
        return paramiko.SFTP_OK
    def rename(self, oldpath, newpath):
        try:
            os.rename(self._realpath(oldpath), self._realpath(newpath))
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)
        return paramiko.SFTP_OK
    def mkdir(self, path, attr):
        try:
            os.mkdir(self._realpath(path))
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)
        return paramiko.SFTP_OK
    def rmdir(self, path):
        try:
            os.rmdir(self._realpath(path))
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)
        return paramiko.SFTP_OK
    def chattr(self, path, attr):
        rp = self._realpath(path)
        if attr._flags & attr.FLAG_PERMISSIONS:
            StubSFTPServer.PERMS[rp] = attr.st_mode & 0o777  # simulace chmod
        try:
            paramiko.SFTPServer.set_file_attr(rp, attr)
        except OSError:
            pass
        return paramiko.SFTP_OK
    def canonicalize(self, path):
        if not path or path[0] != "/":
            path = "/" + path
        parts = []
        for seg in path.split("/"):
            if seg in ("", "."):
                continue
            if seg == "..":
                if parts: parts.pop()
            else:
                parts.append(seg)
        return "/" + "/".join(parts)

def handle(client):
    try:
        t = paramiko.Transport(client)
        # omez algoritmy na ty, ktere libssh2/WinCNG umi (RSA + DH group14)
        so = t.get_security_options()
        if MODERN_ONLY:
            # jen moderní algoritmy, které starý WinCNG backend NEUMĚL
            try:
                so.kex = ("curve25519-sha256", "curve25519-sha256@libssh.org")
                so.ciphers = ("chacha20-poly1305@openssh.com", "aes256-gcm@openssh.com")
                so.key_types = ("ssh-ed25519",)
            except Exception as e:
                sys.stderr.write(f"secopt(modern): {e}\n")
            t.add_server_key(get_ed25519_hostkey())
        else:
            try:
                so.kex = ("diffie-hellman-group14-sha256", "diffie-hellman-group14-sha1", "diffie-hellman-group1-sha1")
                so.key_types = ("rsa-sha2-512", "rsa-sha2-256", "ssh-rsa")
            except Exception as e:
                sys.stderr.write(f"secopt: {e}\n")
            t.add_server_key(get_hostkey())
        t.set_subsystem_handler("sftp", paramiko.SFTPServer, StubSFTPServer)
        t.start_server(server=Server())
        chan = t.accept(30)
        if chan is None:
            t.close(); return
        while t.is_active():
            t.join(1)
    except Exception as e:
        sys.stderr.write(f"conn error: {e}\n")

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((HOST, PORT))
    sock.listen(10)
    sys.stderr.write(f"SFTP test server bezi na {HOST}:{PORT}  user={USER} pass={PASSWORD}  root={ROOT}\n")
    while True:
        client, addr = sock.accept()
        threading.Thread(target=handle, args=(client,), daemon=True).start()

if __name__ == "__main__":
    main()
