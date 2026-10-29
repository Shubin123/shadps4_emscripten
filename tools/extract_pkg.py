#!/usr/bin/env python3
"""Extract a fake-signed (fPKG) PS4 .pkg into a shadPS4-style game folder.

Upstream shadPS4 removed its PKG installer in #2702 (it now expects an already-extracted game
folder), so this re-implements what that code did - same key derivation, same PFS/PFSC layout -
but walks the inner PFS's directory tree properly instead of the old pattern-matching scan.

No key material lives in this repo: the fake keysets are read out of shadPS4's own git history
(src/core/crypto/keys.h, deleted in be22674f), so a shadPS4 checkout is required.

    tools/extract_pkg.py GAME.pkg OUT_DIR [--shadps4-src ~/shadps4_src] [--list]

Requires the `cryptography` package (AES-XTS/CBC, RSA). Only fPKGs are supported; retail PKGs
are encrypted with console-unique keys.
"""

import argparse
import hashlib
import hmac
import os
import re
import struct
import subprocess
import sys
import zlib

from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

KEYS_COMMIT = "be22674f^"
KEYS_PATH = "src/core/crypto/keys.h"
PKG_MAGIC = 0x7F434E54
PFSC_MAGIC = 0x43534650
XTS_SECTOR = 0x1000
DIRENT_FILE, DIRENT_DIR = 2, 3


def load_keysets(shadps4_src):
    try:
        text = subprocess.run(["git", "-C", shadps4_src, "show", f"{KEYS_COMMIT}:{KEYS_PATH}"],
                              check=True, capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        sys.exit(f"could not read {KEYS_PATH} from {shadps4_src}'s history: {e}")
    keysets = {}
    for cls, body in re.findall(r"class (\w+) \{(.*?)\n\};", text, re.S):
        fields = {}
        for name, data in re.findall(r"byte (\w+)\[\] = \{(.*?)\};", body, re.S):
            fields[name] = int.from_bytes(bytes(int(b, 16) for b in re.findall(r"0x[0-9A-Fa-f]+|\b\d+\b", data)), "big")
        keysets[cls] = rsa.RSAPrivateNumbers(
            p=fields["Prime1"], q=fields["Prime2"], d=fields["PrivateExponent"],
            dmp1=fields["Exponent1"], dmq1=fields["Exponent2"], iqmp=fields["Coefficient"],
            public_numbers=rsa.RSAPublicNumbers(fields["PublicExponent"], fields["Modulus"]),
        ).private_key()
    return keysets


def rsa_decrypt32(key, ciphertext):
    return key.decrypt(ciphertext, padding.PKCS1v15())[:32]


def aes_cbc_decrypt(ivkey, data):
    d = Cipher(algorithms.AES(ivkey[16:32]), modes.CBC(ivkey[:16])).decryptor()
    return d.update(data) + d.finalize()


class Pkg:
    def __init__(self, path, keysets):
        self.f = open(path, "rb")
        hdr = self.f.read(0x1000)
        if struct.unpack_from(">I", hdr, 0)[0] != PKG_MAGIC:
            sys.exit("not a PS4 PKG")
        self.content_id = hdr[0x40:0x64].decode()
        n_entries, = struct.unpack_from(">I", hdr, 0x10)
        table_off, = struct.unpack_from(">I", hdr, 0x18)
        self.pfs_offset, self.pfs_size = struct.unpack_from(">QQ", hdr, 0x410)
        self.pfs_cache_size, = struct.unpack_from(">I", hdr, 0x43C)

        self.f.seek(table_off)
        table = self.f.read(n_entries * 32)
        self.entries = []
        for i in range(n_entries):
            raw = table[i * 32:(i + 1) * 32]
            eid, name_off, flags1, flags2, off, size = struct.unpack_from(">IIIIII", raw)
            self.entries.append((eid, off, size, raw))
        self._derive_keys(keysets)

    def read(self, off, size):
        self.f.seek(off)
        return self.f.read(size)

    def entry(self, eid):
        return next(e for e in self.entries if e[0] == eid)

    def _derive_keys(self, keysets):
        # ENTRY_KEYS (0x10): seed[32], digests[7][32], keys[7][256]; keys[3] is DK3.
        _, off, _, _ = self.entry(0x10)
        dk3 = rsa_decrypt32(keysets["PkgDerivedKey3Keyset"], self.read(off + 32 + 7 * 32 + 3 * 256, 256))
        # IMAGE_KEY (0x20): AES-CBC under sha256(entry_row || dk3), then RSA under the fake keyset.
        _, off, _, raw = self.entry(0x20)
        ivkey = hashlib.sha256(raw + dk3).digest()
        ekpfs = rsa_decrypt32(keysets["FakeKeyset"], aes_cbc_decrypt(ivkey, self.read(off, 256)))
        seed = self.read(self.pfs_offset + 0x370, 16)
        dt = hmac.new(ekpfs, struct.pack("<I", 1) + seed, hashlib.sha256).digest()
        self.tweak_key, self.data_key = dt[:16], dt[16:32]

    def read_pfs(self, off, size):
        """Read and XTS-decrypt [off, off+size) of the outer PFS image."""
        start = off & ~(XTS_SECTOR - 1)
        end = (off + size + XTS_SECTOR - 1) & ~(XTS_SECTOR - 1)
        enc = self.read(self.pfs_offset + start, end - start)
        out = bytearray()
        for i in range(0, len(enc), XTS_SECTOR):
            sector = (start + i) // XTS_SECTOR
            d = Cipher(algorithms.AES(self.data_key + self.tweak_key),
                       modes.XTS(sector.to_bytes(16, "little"))).decryptor()
            out += d.update(enc[i:i + XTS_SECTOR])
        return bytes(out[off - start:off - start + size])


class InnerPfs:
    """The PFSC-compressed image (pfs_image.dat) inside the outer encrypted PFS."""

    def __init__(self, pkg):
        self.pkg = pkg
        scan = pkg.read_pfs(0, pkg.pfs_cache_size * 2)
        self.pfsc = next((i for i in range(0x20000, len(scan) - 4, 0x10000)
                          if struct.unpack_from("<I", scan, i)[0] == PFSC_MAGIC), None)
        if self.pfsc is None:
            sys.exit("PFSC image not found - not an fPKG, or a layout this tool doesn't know")
        hdr = scan[self.pfsc:self.pfsc + 0x30]
        _, _, _, _, self.block_size, map_off, _, data_len = struct.unpack_from("<iiiiqqQq", hdr)
        n = data_len // self.block_size
        raw = pkg.read_pfs(self.pfsc + map_off, (n + 1) * 8)
        self.block_map = struct.unpack_from(f"<{n + 1}Q", raw)

        sb = self.block(0)
        self.inner_block_size, = struct.unpack_from("<i", sb, 0x20)
        self.dinode_count, = struct.unpack_from("<q", sb, 0x30)
        self.superroot, = struct.unpack_from("<q", sb, 0x48)
        assert self.inner_block_size == self.block_size, "inner/outer block size mismatch"
        self.inodes = self._read_inodes()

    def block(self, i):
        a, b = self.block_map[i], self.block_map[i + 1]
        if b == a:
            return bytes(self.block_size)
        data = self.pkg.read_pfs(self.pfsc + a, b - a)
        if b - a == self.block_size:
            return data
        return zlib.decompress(data).ljust(self.block_size, b"\0")

    def _read_inodes(self):
        inodes, blk = [], 1
        per_block = self.block_size // 0xA8
        while len(inodes) < self.dinode_count:
            data = self.block(blk)
            for p in range(per_block):
                if len(inodes) == self.dinode_count:
                    break
                mode, _, flags, size = struct.unpack_from("<HHIq", data, p * 0xA8)
                nblocks, loc = struct.unpack_from("<II", data, p * 0xA8 + 0x60)
                inodes.append((mode, size, nblocks, loc))
            blk += 1
        return inodes

    def listdir(self, ino):
        _, _, nblocks, loc = self.inodes[ino]
        for b in range(nblocks):
            data, p = self.block(loc + b), 0
            while p + 16 <= len(data):
                d_ino, d_type, namelen, entsize = struct.unpack_from("<iiii", data, p)
                if d_ino == 0 or entsize == 0:
                    break
                if d_type in (DIRENT_FILE, DIRENT_DIR):
                    yield data[p + 16:p + 16 + namelen].decode(), d_ino, d_type
                p += entsize

    def read_file(self, ino, out):
        _, size, nblocks, loc = self.inodes[ino]
        for b in range(nblocks):
            out.write(self.block(loc + b)[:min(self.block_size, size - b * self.block_size)])


def walk(pfs, ino, rel=""):
    for name, child, typ in pfs.listdir(ino):
        path = f"{rel}/{name}" if rel else name
        yield path, child, typ
        if typ == DIRENT_DIR:
            yield from walk(pfs, child, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pkg")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--shadps4-src", default=os.path.expanduser("~/shadps4_src"))
    ap.add_argument("--list", action="store_true", help="list files instead of extracting")
    args = ap.parse_args()

    pkg = Pkg(args.pkg, load_keysets(args.shadps4_src))
    pfs = InnerPfs(pkg)
    uroot = next(c for n, c, t in pfs.listdir(pfs.superroot) if n == "uroot")
    print(f"{pkg.content_id}: {pfs.dinode_count} inodes")

    if args.list or not args.out:
        for path, ino, typ in walk(pfs, uroot):
            print(f"{pfs.inodes[ino][1]:>12}  {path}{'/' if typ == DIRENT_DIR else ''}")
        return

    os.makedirs(os.path.join(args.out, "sce_sys"), exist_ok=True)
    # sce_sys metadata entries (param.sfo, icons, ...) live in the PKG's entry table, not the PFS.
    entry_names = {0x0402: "nptitle.dat", 0x0403: "npbind.dat", 0x1000: "param.sfo",
                   0x1001: "playgo-chunk.dat", 0x1002: "playgo-chunk.sha",
                   0x1003: "playgo-manifest.xml", 0x1004: "pronunciation.xml",
                   0x1005: "pronunciation.sig", 0x1006: "pic1.png", 0x100B: "shareparam.json",
                   0x100D: "save_data.png", 0x1200: "icon0.png", 0x1220: "pic0.png",
                   0x1240: "snd0.at9", 0x1260: "changeinfo/changeinfo.xml", 0x1280: "icon0.dds",
                   0x12C0: "pic1.dds", 0x1400: "trophy/trophy00.trp"}
    for eid, off, size, _ in pkg.entries:
        if eid in entry_names:
            dst = os.path.join(args.out, "sce_sys", entry_names[eid])
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as o:
                o.write(pkg.read(off, size))

    for path, ino, typ in walk(pfs, uroot):
        dst = os.path.join(args.out, path)
        if typ == DIRENT_DIR:
            os.makedirs(dst, exist_ok=True)
            continue
        size = pfs.inodes[ino][1]
        if os.path.exists(dst) and os.path.getsize(dst) == size:
            continue
        print(f"{size:>12}  {path}", flush=True)
        with open(dst + ".part", "wb") as o:
            pfs.read_file(ino, o)
        os.replace(dst + ".part", dst)


if __name__ == "__main__":
    main()
