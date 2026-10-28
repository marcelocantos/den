#!/usr/bin/env python3
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
"""Regenerate the archive-format fixtures used by tests/test_archive_formats.cpp.

The fixtures are written with Python's own compressors (not libarchive), so
the test checks that den's libarchive reads archives produced by an
independent implementation. Requires Python 3.14+ (compression.zstd).

    python3 tests/fixtures/archives/make_fixtures.py
"""

import io
import os
import tarfile
import zipfile
from compression import zstd

HERE = os.path.dirname(os.path.abspath(__file__))
MTIME = 1700000000
FILES = [
    ("hello/README.txt", b"hello from den\n", 0o644),
    ("hello/bin/tool", b"#!/bin/sh\necho hi\n", 0o755),
]


def tar_bytes() -> bytes:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.PAX_FORMAT) as tf:
        for d in ("hello", "hello/bin"):
            ti = tarfile.TarInfo(d)
            ti.type, ti.mode, ti.mtime = tarfile.DIRTYPE, 0o755, MTIME
            tf.addfile(ti)
        for name, data, mode in FILES:
            ti = tarfile.TarInfo(name)
            ti.size, ti.mode, ti.mtime = len(data), mode, MTIME
            tf.addfile(ti, io.BytesIO(data))
    return buf.getvalue()


def main() -> None:
    raw = tar_bytes()
    import bz2, gzip, lzma

    outputs = {
        "hello.tar.gz": gzip.compress(raw, mtime=0),
        "hello.tar.bz2": bz2.compress(raw),
        "hello.tar.xz": lzma.compress(raw, format=lzma.FORMAT_XZ),
        "hello.tar.zst": zstd.compress(raw),
    }
    for name, data in outputs.items():
        with open(os.path.join(HERE, name), "wb") as f:
            f.write(data)

    zpath = os.path.join(HERE, "hello.zip")
    with zipfile.ZipFile(zpath, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for d in ("hello/", "hello/bin/"):
            zi = zipfile.ZipInfo(d, date_time=(2023, 11, 14, 22, 13, 20))
            zi.external_attr = (0o40755 << 16) | 0x10
            zi.create_system = 3
            zf.writestr(zi, b"")
        for name, data, mode in FILES:
            zi = zipfile.ZipInfo(name, date_time=(2023, 11, 14, 22, 13, 20))
            zi.external_attr = (0o100000 | mode) << 16
            zi.create_system = 3
            zi.compress_type = zipfile.ZIP_DEFLATED
            zf.writestr(zi, data)


if __name__ == "__main__":
    main()
