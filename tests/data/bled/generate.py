#!/usr/bin/env python3
"""Regenerate the bled unit test fixtures (requires Python 3.14+ for zstd).

The payload is produced by the same generator as payload() in test_bled.c, so
the test can check the decompressed output without storing it.
"""

import bz2
import gzip
import lzma
import os
import zipfile
from compression import zstd

PAYLOAD_SIZE = 600 * 1024  # larger than bled's 256 KB buffer


def payload(size: int) -> bytes:
    out = bytearray()
    i = 0
    while len(out) < size:
        out += b"Rufus bled test line %08d: the quick brown fox jumps over the lazy dog\n" % i
        i += 1
    return bytes(out[:size])


def main() -> None:
    here = os.path.dirname(os.path.abspath(__file__))
    data = payload(PAYLOAD_SIZE)

    def write(name: str, blob: bytes) -> None:
        with open(os.path.join(here, name), "wb") as f:
            f.write(blob)

    write("payload.gz", gzip.compress(data, mtime=0))
    write("payload.xz", lzma.compress(data, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32))
    write("payload.lzma", lzma.compress(data, format=lzma.FORMAT_ALONE))
    write("payload.bz2", bz2.compress(data))
    write("payload.zst", zstd.compress(data))

    def zinfo(name: str) -> zipfile.ZipInfo:
        info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        return info

    with zipfile.ZipFile(os.path.join(here, "archive.zip"), "w") as z:
        z.writestr(zinfo("payload.txt"), data)
        z.writestr(zinfo("dir/sub/hello.txt"), b"hello from a subdirectory\n")

    # Entry trying to escape the extraction directory ("zip slip")
    with zipfile.ZipFile(os.path.join(here, "zipslip.zip"), "w") as z:
        z.writestr(zinfo("../escaped.txt"), b"this file must not be created\n")


if __name__ == "__main__":
    main()
