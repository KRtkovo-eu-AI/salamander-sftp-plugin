#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate an unencrypted PuTTY .ppk v2 file from the OpenSSH RSA key sftp_testkey.
Used to test the plugin .ppk parser. The MAC is a dummy value (the parser ignores it)."""
import struct, base64, paramiko

KEY = r"D:\weby\sftp_testkey"
OUT = r"D:\weby\sftp_testkey.ppk"

k = paramiko.RSAKey(filename=KEY)
nums = k.key.private_numbers()
pub = nums.public_numbers

def mpint(x):
    b = x.to_bytes((x.bit_length() + 8) // 8, "big") if x else b"\x00"
    return struct.pack(">I", len(b)) + b

def sstr(b):
    return struct.pack(">I", len(b)) + b

pubblob = sstr(b"ssh-rsa") + mpint(pub.e) + mpint(pub.n)
privblob = mpint(nums.d) + mpint(nums.p) + mpint(nums.q) + mpint(nums.iqmp)

def lines(blob):
    s = base64.b64encode(blob).decode()
    return [s[i:i+64] for i in range(0, len(s), 64)]

pl = lines(pubblob)
ql = lines(privblob)
out = []
out.append("PuTTY-User-Key-File-2: ssh-rsa")
out.append("Encryption: none")
out.append("Comment: sftp-test")
out.append("Public-Lines: %d" % len(pl)); out += pl
out.append("Private-Lines: %d" % len(ql)); out += ql
out.append("Private-MAC: " + "00" * 20)  # dummy, parser ho ignoruje
open(OUT, "w", newline="\r\n").write("\n".join(out) + "\n")
print("Zapsano:", OUT)
