#!/usr/bin/env python3
"""Pull one partition out of a ChromeOS disk image arriving on stdin.

The recovery image is a couple of gigabytes inside a deflated zip, so it can
only be read forwards, once. That is enough: the GPT sits at the front, and the
partition it points at can be written out as the stream reaches it. Everything
before and after is read and dropped, so only the partition lands on disk.
"""
import argparse
import struct
import sys

SECTOR = 512


def read_exact(stream, size):
    data = stream.read(size)
    if data is None or len(data) < size:
        raise SystemExit(f"image ended after {len(data or b'')} of {size} bytes")
    return data


def skip(stream, size, chunk=4 << 20):
    while size:
        size -= len(read_exact(stream, min(size, chunk)))


def find_partition(stream, wanted):
    """Read the GPT off the front of the stream, leaving it just past the table."""
    read_exact(stream, SECTOR)                        # protective MBR
    header = read_exact(stream, SECTOR)
    if header[:8] != b"EFI PART":
        raise SystemExit("not a GPT image")
    table_lba, count, entry_size = struct.unpack_from("<QII", header, 72)

    position = 2
    skip(stream, (table_lba - position) * SECTOR)
    position = table_lba

    table = read_exact(stream, count * entry_size)
    position += (count * entry_size + SECTOR - 1) // SECTOR

    for i in range(count):
        entry = table[i * entry_size:(i + 1) * entry_size]
        first, last = struct.unpack_from("<QQ", entry, 32)
        name = entry[56:128].decode("utf-16-le").rstrip("\0")
        if name == wanted:
            return position, first, (last - first + 1)
    raise SystemExit(f"no partition named {wanted!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output")
    parser.add_argument("--partition", default="ROOT-A")
    args = parser.parse_args()

    stream = sys.stdin.buffer
    position, first, sectors = find_partition(stream, args.partition)
    print(f"{args.partition}: {sectors * SECTOR / 1e9:.1f} GB at sector {first}",
          file=sys.stderr)

    skip(stream, (first - position) * SECTOR)
    remaining = sectors * SECTOR
    written = 0
    with open(args.output, "wb") as out:
        while remaining:
            chunk = read_exact(stream, min(remaining, 4 << 20))
            out.write(chunk)
            remaining -= len(chunk)
            written += len(chunk)
            if written % (256 << 20) < (4 << 20):
                print(f"  {written / 1e9:.1f} GB", file=sys.stderr)
    print(f"wrote {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
