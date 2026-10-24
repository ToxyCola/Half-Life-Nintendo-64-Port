"""Append a staged folder to HL64Builder.exe as its payload (see builder/hl64builder.c).

    python3 builder/pack.py <stub.exe> <staging dir> <out.exe>

payload: "HL64PAK1", uint32 count, then per file: uint16 name length, name (/ separated),
         uint64 size, uint64 packed size, zlib data
footer:  uint64 payload offset, "HL64PAY1"
"""
import os, sys, struct, zlib


def main():
    stub, stage, out = sys.argv[1:4]
    files = []
    for root, _, names in os.walk(stage):
        for n in sorted(names):
            p = os.path.join(root, n)
            files.append((os.path.relpath(p, stage).replace(os.sep, '/'), p))
    files.sort()
    data = open(stub, 'rb').read()
    with open(out, 'wb') as f:
        f.write(data)
        off = f.tell()
        f.write(b'HL64PAK1' + struct.pack('<I', len(files)))
        raw = packed = 0
        for name, p in files:
            b = open(p, 'rb').read()
            z = zlib.compress(b, 9)
            nb = name.encode('utf-8')
            f.write(struct.pack('<H', len(nb)) + nb + struct.pack('<QQ', len(b), len(z)))
            f.write(z)
            raw += len(b)
            packed += len(z)
        f.write(struct.pack('<Q', off) + b'HL64PAY1')
    print('%s: %d files, %.1f MB -> %.1f MB' % (out, len(files), raw / 1048576, os.path.getsize(out) / 1048576))


if __name__ == '__main__':
    main()
