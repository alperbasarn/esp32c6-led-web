#!/usr/bin/env python3
"""Compress a build asset to a byte-reproducible gzip stream.

Reproducibility is the only reason this is a script and not a `gzip -9` in the
CMake command. The stock gzip program writes the source modification time and
name into the header, so two builds of an unchanged tree produce different
bytes -- which would show up as a phantom image-size change in CI's flash
headroom gate and defeat the point of tracking it. Writing the member with
mtime=0 and no filename field makes the output a pure function of the input.

Deliberately stdlib-only: zopfli would save roughly another kilobyte, but a
build-time dependency for one asset is a poor trade when the asset is already
paying for itself several times over.
"""

import gzip
import io
import sys


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(f"usage: {argv[0]} <source> <dest.gz>", file=sys.stderr)
        return 2

    source, dest = argv[1], argv[2]
    with open(source, "rb") as handle:
        raw = handle.read()

    buffer = io.BytesIO()
    # filename="" and mtime=0 keep the 10-byte header constant.
    with gzip.GzipFile(filename="", fileobj=buffer, mode="wb",
                       compresslevel=9, mtime=0) as out:
        out.write(raw)
    payload = buffer.getvalue()

    # A corrupt asset would be served to every client and is cheap to rule out
    # here, where it fails the build instead of the device.
    if gzip.decompress(payload) != raw:
        print(f"{source}: gzip round-trip did not reproduce the input",
              file=sys.stderr)
        return 1

    with open(dest, "wb") as handle:
        handle.write(payload)

    saved = len(raw) - len(payload)
    print(f"{source}: {len(raw):,} -> {len(payload):,} bytes "
          f"(reclaimed {saved:,})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
