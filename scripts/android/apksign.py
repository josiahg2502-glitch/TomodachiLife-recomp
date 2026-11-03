#!/usr/bin/env python3
"""APK Signature Scheme v2 in Python (pycryptodome, which the installer already has): the Android app is signed on a
player's PC without Java (apksigner). Android 7+ checks v2 only; the app's minimum is Android 10.

    python scripts/android/apksign.py <aligned.apk> <signed.apk> <key folder>

The key (RSA 2048, key.pem) and its self-signed certificate (cert.der) are made in <key folder> once and kept: an update
of the app must carry the same certificate, or Android refuses to install it over the old one.
"""
import hashlib
import os
import struct
import sys

from Crypto.Hash import SHA256
from Crypto.PublicKey import RSA
from Crypto.Signature import pkcs1_15

V2_ID, RSA_PKCS1_SHA256 = 0x7109871A, 0x0103
CHUNK = 1 << 20


def der(tag, body):
    n = len(body)
    head = bytes([n]) if n < 0x80 else (lambda b: bytes([0x80 | len(b)]) + b)(n.to_bytes((n.bit_length() + 7) // 8, 'big'))
    return bytes([tag]) + head + body


def certificate(key):
    """A self-signed X.509 v3 certificate of the key (CN "Tomodachi Life recomp"), DER."""
    sha256_rsa = der(0x30, der(0x06, bytes.fromhex('2a864886f70d01010b')) + der(0x05, b''))
    name = der(0x30, der(0x31, der(0x30, der(0x06, bytes.fromhex('550403')) + der(0x0C, b'Tomodachi Life recomp'))))
    serial = int.from_bytes(hashlib.sha256(key.publickey().export_key('DER')).digest()[:8], 'big') | 1
    tbs = der(0x30, der(0xA0, der(0x02, b'\x02')) + der(0x02, serial.to_bytes(serial.bit_length() // 8 + 1, 'big')) + sha256_rsa + name +
              der(0x30, der(0x17, b'260101000000Z') + der(0x18, b'20991231235959Z')) + name + key.publickey().export_key('DER'))
    sig = pkcs1_15.new(key).sign(SHA256.new(tbs))
    return der(0x30, tbs + sha256_rsa + der(0x03, b'\x00' + sig))


def load_key(folder):
    kp, cp = os.path.join(folder, 'key.pem'), os.path.join(folder, 'cert.der')
    if not os.path.isfile(kp):
        os.makedirs(folder, exist_ok=True)
        key = RSA.generate(2048)
        with open(kp, 'wb') as f:
            f.write(key.export_key('PEM', pkcs=8))
        with open(cp, 'wb') as f:
            f.write(certificate(key))
    with open(kp, 'rb') as f:
        key = RSA.import_key(f.read())
    with open(cp, 'rb') as f:
        return key, f.read()


def lp(b):
    return struct.pack('<I', len(b)) + b


def chunked_sha256(sections):
    """v2's digest: every section in 1 MB chunks, each hashed with its length, then the chunk digests hashed together."""
    digests = []
    for s in sections:
        for i in range(0, len(s), CHUNK):
            c = s[i:i + CHUNK]
            digests.append(hashlib.sha256(b'\xa5' + struct.pack('<I', len(c)) + c).digest())
    return hashlib.sha256(b'\x5a' + struct.pack('<I', len(digests)) + b''.join(digests)).digest()


def sign(src, dst, keydir):
    data = open(src, 'rb').read()
    eocd = data.rfind(b'PK\x05\x06', max(0, len(data) - 65557))
    if eocd < 0:
        raise SystemExit('%s: not a zip' % src)
    cd_size, cd_off = struct.unpack('<II', data[eocd + 12:eocd + 20])
    if data[cd_off - 16:cd_off] == b'APK Sig Block 42':
        raise SystemExit('%s is signed already' % src)
    key, cert = load_key(keydir)
    digest = chunked_sha256([data[:cd_off], data[cd_off:eocd], data[eocd:]])
    signed = lp(lp(struct.pack('<I', RSA_PKCS1_SHA256) + lp(digest))) + lp(lp(cert)) + lp(b'')
    sig = pkcs1_15.new(key).sign(SHA256.new(signed))
    signer = lp(signed) + lp(lp(struct.pack('<I', RSA_PKCS1_SHA256) + lp(sig))) + lp(key.publickey().export_key('DER'))
    value = lp(lp(signer))
    pair = struct.pack('<QI', 4 + len(value), V2_ID) + value
    size = len(pair) + 8 + 16
    block = struct.pack('<Q', size) + pair + struct.pack('<Q', size) + b'APK Sig Block 42'
    end = bytearray(data[eocd:])
    struct.pack_into('<I', end, 16, cd_off + len(block))
    with open(dst, 'wb') as f:
        f.write(data[:cd_off] + block + data[cd_off:eocd] + bytes(end))


if __name__ == '__main__':
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    sign(sys.argv[1], sys.argv[2], sys.argv[3])
