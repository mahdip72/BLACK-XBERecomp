"""Extract original Xbox image assets and encode host-only DDS replacements.

No game artwork, model weights, or generated files belong in the repository.
"""
import hashlib
import io
import json
import struct
from collections import Counter
from pathlib import Path

import numpy as np
from PIL import Image


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def fingerprint(width, height, fmt, pixels, palette=b''):
    value = 14695981039346656037
    for byte in struct.pack('<III', width, height, fmt) + pixels + palette:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f'{value:016x}'


class Disc:
    def __init__(self, path):
        self.file = Path(path).open('rb')
        volume = self.read(32 * 2048, 2048)
        if volume[:20] != b'MICROSOFT*XBOX*MEDIA':
            raise ValueError('Expected an XISO filesystem image')
        self.root = struct.unpack_from('<II', volume, 20)

    def read(self, offset, size):
        self.file.seek(offset)
        data = self.file.read(size)
        if len(data) != size:
            raise ValueError('Truncated game image')
        return data

    def entries(self, sector, size):
        table = self.read(sector * 2048, size)
        todo, seen = [0], set()
        while todo:
            at = todo.pop()
            if at in seen or at + 14 > size:
                continue
            seen.add(at)
            left, right, sector, length, flags, count = struct.unpack_from('<HHIIBB', table, at)
            if left == 65535:
                continue
            todo.extend(x * 4 for x in (left, right) if x)
            yield table[at + 14:at + 14 + count].decode('ascii'), sector, length, flags

    def files(self, root=None, prefix=''):
        for name, sector, size, flags in self.entries(*(root or self.root)):
            path = prefix + name
            if flags & 16:
                yield from self.files((sector, size), path + '/')
            else:
                yield path, sector, size

    def find(self, path):
        name, sector, size = next(entry for entry in self.files() if entry[0].lower() == path.lower())
        return sector * 2048, size


def morton(width, height):
    xs, ys = np.arange(width, dtype=np.uint32), np.arange(height, dtype=np.uint32)
    xx, yy, bit = np.zeros(width, dtype=np.uint32), np.zeros(height, dtype=np.uint32), 0
    for i in range(max(width, height).bit_length() - 1):
        if 1 << i < width:
            xx |= ((xs >> i) & 1) << bit
            bit += 1
        if 1 << i < height:
            yy |= ((ys >> i) & 1) << bit
            bit += 1
    return yy[:, None] | xx[None, :]


def decode(width, height, code, pixels, palette):
    if code == 11:
        indices = np.frombuffer(pixels, dtype=np.uint8)[morton(width, height)]
        colours = np.frombuffer(palette, dtype=np.uint8).reshape(256, 4)[:, [2, 1, 0, 3]]
        return Image.fromarray(colours[indices])
    if code == 58:
        colours = np.frombuffer(pixels, dtype=np.uint8).reshape(-1, 4)
        return Image.fromarray(colours[morton(width, height)])
    header = bytearray(128)
    header[:4] = b'DDS '
    struct.pack_into('<7I', header, 4, 124, 0x81007, height, width, len(pixels), 0, 0)
    struct.pack_into('<II4s', header, 76, 32, 4, {12: b'DXT1', 14: b'DXT3', 15: b'DXT5'}[code])
    struct.pack_into('<I', header, 108, 0x1000)
    return Image.open(io.BytesIO(header + pixels)).convert('RGBA')


def extract(iso, destination):
    """Audit serialized 2D textures in BIN/DB/XBE files, deduplicating by GPU key."""
    destination = Path(destination)
    originals = destination / 'originals'
    originals.mkdir(parents=True, exist_ok=True)
    disc, items, occurrences, unsupported = Disc(iso), {}, 0, []
    for path, sector, size in sorted(disc.files()):
        if Path(path).suffix.lower() not in ('.bin', '.db', '.xbe'):
            continue
        data = disc.read(sector * 2048, size)
        cursor = 0
        while (cursor := data.find(b'\x01\x00\x04\x00', cursor)) >= 0:
            at, cursor = cursor, cursor + 4
            if at % 4 or at + 128 > size:
                continue
            common, address, lock, fmt, extent = struct.unpack_from('<5I', data, at)
            name = data[at + 68:at + 100].split(b'\0')[0]
            if address != 128 or lock or not name or any(c < 32 or c > 126 for c in name):
                continue
            width, height = 1 << ((fmt >> 20) & 15), 1 << ((fmt >> 24) & 15)
            code, levels = (fmt >> 8) & 255, (fmt >> 16) & 15
            source = {'path': path, 'offset': at, 'name': name.decode('ascii')}
            if fmt & 255 != 0x29 or extent or code not in (11, 12, 14, 15, 58) or not levels:
                unsupported.append({**source, 'format': fmt, 'size': extent})
                continue
            count = (width * height * (1 if code == 11 else 4) if code in (11, 58)
                     else max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * (8 if code == 12 else 16))
            if at + address + count > size:
                raise ValueError(f'Truncated pixels: {source}')
            pixels, palette = data[at + address:at + address + count], b''
            if code == 11:
                relative = struct.unpack_from('<I', data, at + 20)[0]
                if not relative or at + relative + 8 > size:
                    raise ValueError(f'Missing palette: {source}')
                pc, pd = struct.unpack_from('<II', data, at + relative)
                colours = 256 >> (pc >> 30)
                if at + pd + colours * 4 > size:
                    raise ValueError(f'Truncated palette: {source}')
                palette = data[at + pd:at + pd + colours * 4] + bytes(1024 - colours * 4)
            key = fingerprint(width, height, code, pixels, palette)
            occurrences += 1
            if key in items:
                items[key]['sources'].append(source)
                continue
            image = decode(width, height, code, pixels, palette)
            output = originals / f'{key}.png'
            image.save(output)
            items[key] = dict(key=key, name=source['name'], width=width, height=height,
                              format=code, levels=levels, source_sha256=hashlib.sha256(pixels + palette).hexdigest(),
                              png_sha256=sha256(output), sources=[source])
        print(f'extract {path}: {len(items)} unique', flush=True)
    manifest = dict(schema=1, iso_bytes=Path(iso).stat().st_size, occurrences=occurrences,
                    unsupported=unsupported, formats=dict(Counter(str(x['format']) for x in items.values())),
                    textures=sorted(items.values(), key=lambda x: x['key']))
    (destination / 'originals.json').write_text(json.dumps(manifest, indent=2) + '\n')
    if unsupported:
        raise ValueError(f'{len(unsupported)} unsupported texture headers; see originals.json')
    return manifest


def write_dds(image, destination):
    """BC3 with complete mips; RGB and alpha resize independently (alpha may be specular)."""
    rgb, alpha = image.convert('RGB'), image.getchannel('A')
    width, height, chunks, header = *image.size, [], None
    while True:
        level = rgb.resize((width, height), Image.Resampling.LANCZOS).convert('RGBA')
        level.putalpha(alpha.resize((width, height), Image.Resampling.LANCZOS))
        stream = io.BytesIO()
        level.save(stream, format='DDS', pixel_format='DXT5')
        block = stream.getvalue()
        if header is None:
            header = bytearray(block[:128])
        chunks.append(block[128:])
        if width == height == 1:
            break
        width, height = max(1, width // 2), max(1, height // 2)
    struct.pack_into('<I', header, 8, 0xa1007)
    struct.pack_into('<I', header, 20, len(chunks[0]))
    struct.pack_into('<I', header, 28, len(chunks))
    struct.pack_into('<I', header, 108, 0x401008)
    destination.write_bytes(header + b''.join(chunks))
