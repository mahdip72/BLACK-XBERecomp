"""Build optional Xbox menu texture replacements from a user's ISO and extracted mod pack.
Requires Pillow. Game and mod artwork are read locally and are never added to Git.
"""
import argparse
import io
import json
import struct
from pathlib import Path
from PIL import Image


def fingerprint(width, height, fmt, pixels, palette=b''):
    value = 14695981039346656037
    for byte in struct.pack('<III', width, height, fmt) + pixels + palette:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f'{value:016x}'


class Disc:
    def __init__(self, path):
        self.file = open(path, 'rb')
        volume = self.read(32 * 2048, 2048)
        if volume[:20] != b'MICROSOFT*XBOX*MEDIA':
            raise ValueError('Expected an extracted XISO filesystem image')
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

    def find(self, path):
        sector, length = self.root
        for part in path.split('/'):
            _, sector, length, _ = next(x for x in self.entries(sector, length) if x[0].lower() == part.lower())
        return sector * 2048, length


def unswizzle(pixels, width, height, channels):
    result = bytearray(width * height * channels)
    def offsets(size, other):
        out = []
        for pos in range(size):
            address = bit = 0
            for i in range(max(size, other).bit_length() - 1):
                if (1 << i) < size:
                    address |= ((pos >> i) & 1) << bit
                    bit += 1
                if (1 << i) < other:
                    bit += 1
            out.append(address)
        return out
    xs = offsets(width, height)
    # Y follows X at each bit, rather than preceding it.
    ys = []
    for y in range(height):
        address = bit = 0
        for i in range(max(width, height).bit_length() - 1):
            if (1 << i) < width:
                bit += 1
            if (1 << i) < height:
                address |= ((y >> i) & 1) << bit
                bit += 1
        ys.append(address)
    for y in range(height):
        for x in range(width):
            src = (xs[x] | ys[y]) * channels
            dst = (y * width + x) * channels
            result[dst:dst + channels] = pixels[src:src + channels]
    return bytes(result)


def native_texture(disc, entry):
    start, length = disc.find(entry['source']['path'])
    offset = entry['source']['offset']
    if offset + 128 > length:
        raise ValueError('Texture outside game file')
    header = disc.read(start + offset, 128)
    fmt = struct.unpack_from('<I', header, 12)[0]
    code = (fmt >> 8) & 255
    w, h = 1 << ((fmt >> 20) & 15), 1 << ((fmt >> 24) & 15)
    if (w, h, code) != (entry['width'], entry['height'], entry['format']):
        raise ValueError('Original texture layout does not match this import map')
    size = w * h * (1 if code == 11 else 4) if code in (11, 58) else max(1, (w+3)//4) * max(1, (h+3)//4) * (8 if code == 12 else 16)
    if offset + 128 + size > length:
        raise ValueError('Texture pixels outside game file')
    pixels = disc.read(start + offset + 128, size)
    palette = b''
    if code == 11:
        pal = struct.unpack_from('<I', header, 20)[0]
        common, data = struct.unpack('<II', disc.read(start + offset + pal, 8))
        count = 256 >> (common >> 30)
        palette = disc.read(start + offset + data, count * 4) + bytes(1024 - count * 4)
    if fingerprint(w, h, code, pixels, palette) != entry['key']:
        raise ValueError('Original texture contents differ from the reviewed Xbox texture')
    if code == 11:
        indices = unswizzle(pixels, w, h, 1)
        return Image.frombytes('RGBA', (w, h), b''.join(palette[i*4:i*4+4] for i in indices), 'raw', 'BGRA')
    if code == 58:
        return Image.frombytes('RGBA', (w, h), unswizzle(pixels, w, h, 4))
    dds = bytearray(128)
    dds[:4] = b'DDS '
    struct.pack_into('<7I', dds, 4, 124, 0x81007, h, w, size, 0, 0)
    struct.pack_into('<II4s', dds, 76, 32, 4, {12:b'DXT1', 14:b'DXT3', 15:b'DXT5'}[code])
    struct.pack_into('<I', dds, 108, 0x1000)
    return Image.open(io.BytesIO(dds + pixels)).convert('RGBA')


def write_dds(image, alpha, destination):
    """Encode BC3 with complete mips; opacity/specular values retain Xbox semantics."""
    w, h = image.size
    chunks = []
    header = None
    rgb = image.convert('RGB')
    while True:
        level = rgb.resize((w, h), Image.Resampling.LANCZOS).convert('RGBA')
        level.putalpha(alpha.resize((w, h), Image.Resampling.LANCZOS))
        data = io.BytesIO()
        level.save(data, format='DDS', pixel_format='DXT5')
        block = data.getvalue()
        if header is None:
            header = bytearray(block[:128])
        chunks.append(block[128:])
        if w == h == 1:
            break
        w, h = max(1, w//2), max(1, h//2)
    struct.pack_into('<I', header, 8, 0xa1007)  # DDSD_MIPMAPCOUNT
    struct.pack_into('<I', header, 20, len(chunks[0]))
    struct.pack_into('<I', header, 28, len(chunks))
    struct.pack_into('<I', header, 108, 0x401008)  # texture, complex, mipmap
    destination.write_bytes(header + b''.join(chunks))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--mod', type=Path, required=True, help='Extracted SLUS-21376/replacements directory')
    parser.add_argument('--output', type=Path, required=True, help='textures directory beside BLACK.exe')
    parser.add_argument('--mapping', type=Path, default=Path(__file__).parent/'hd_textures'/'mapping.json')
    parser.add_argument('--limit', type=int, default=0, help='Optional bounded trial import')
    args = parser.parse_args()
    entries = json.loads(args.mapping.read_text())['entries']
    if args.limit:
        entries = entries[:args.limit]
    args.output.mkdir(parents=True, exist_ok=True)
    disc = Disc(args.iso)
    imported, failed = [], []
    for i, entry in enumerate(entries):
        destination = args.output / (entry['key'] + '.dds')
        try:
            original = native_texture(disc, entry)
            mod_path = args.mod / entry['mod']
            if mod_path.parent.resolve() != args.mod.resolve():
                raise ValueError('Replacement path outside the mod directory')
            with Image.open(mod_path) as replacement:
                replacement = replacement.convert('RGBA')
                write_dds(replacement, original.getchannel('A'), destination)
            imported.append({k:entry[k] for k in ('key', 'name', 'mod', 'alpha') if k in entry})
        except Exception as error:
            failed.append({'key':entry['key'], 'name':entry['name'], 'error':str(error)})
        if i % 25 == 0 or i+1 == len(entries):
            print(f'{i+1}/{len(entries)} processed; {len(imported)} imported; {len(failed)} failed', flush=True)
    report = {'imported':imported, 'failed':failed, 'source_assets_used':len(set(x['mod'] for x in imported))}
    (args.output/'import-report.json').write_text(json.dumps(report, indent=2)+'\n')
    if failed:
        raise SystemExit(f'{len(failed)} imports failed; see import-report.json')


if __name__ == '__main__':
    main()
