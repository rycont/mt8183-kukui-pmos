"""Build a tiny GPT image with a ROOT-A partition to check the extractor."""
import struct, subprocess, sys
S = 512
first, sectors = 40, 8            # ROOT-A payload
entries, esize = 4, 128
table_lba = 2

img = bytearray(S * (first + sectors))
img[0:S] = b"\x00" * S            # protective MBR

hdr = bytearray(S)
hdr[0:8] = b"EFI PART"
struct.pack_into("<QII", hdr, 72, table_lba, entries, esize)
img[S:2 * S] = hdr

table = bytearray(entries * esize)
for i, (name, lba, n) in enumerate([("STATE", 32, 8), ("ROOT-A", first, sectors)]):
    e = bytearray(esize)
    e[0:16] = b"\x01" * 16
    struct.pack_into("<QQ", e, 32, lba, lba + n - 1)
    e[56:56 + len(name) * 2] = name.encode("utf-16-le")
    table[i * esize:(i + 1) * esize] = e
img[table_lba * S:table_lba * S + len(table)] = table

payload = bytes(range(256)) * (sectors * S // 256)
img[first * S:first * S + len(payload)] = payload
open("/tmp/gpt.img", "wb").write(img)

subprocess.run([sys.executable, "tools/mali-extract-root.py", "/tmp/out.bin"],
               stdin=open("/tmp/gpt.img", "rb"), check=True)
got = open("/tmp/out.bin", "rb").read()
assert got == payload, f"got {len(got)} bytes, expected {len(payload)}"
print("OK: ROOT-A extracted byte-for-byte")
