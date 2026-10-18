"""Combine original-art PBRify textures with only the two HD Reimagined fonts."""
import argparse
import json
import shutil
import struct
from pathlib import Path

from pbrify_assets import sha256
from upscale_pbrify import RECIPE


def check_dds(path, expected_hash, expected_size, pixel_format='BC3'):
    if sha256(path) != expected_hash:
        raise ValueError(f'Texture hash mismatch: {path}')
    data = path.read_bytes()
    if len(data) < 128 or data[:4] != b'DDS ':
        raise ValueError(f'Expected DDS: {path}')
    h, w = struct.unpack_from('<II', data, 12)
    levels = max(1, struct.unpack_from('<I', data, 28)[0])
    if pixel_format == 'BGRA8':
        expected = (32, 0x41, 0, 32, 0xff0000, 0xff00, 0xff, 0xff000000)
        if struct.unpack_from('<8I', data, 76) != expected or levels != 1 or struct.unpack_from('<I', data, 20)[0] != w * 4:
            raise ValueError(f'Expected tightly packed lossless BGRA8 font DDS: {path}')
        size = w * h * 4
    elif pixel_format == 'BC3':
        if data[84:88] != b'DXT5' or levels != max(w, h).bit_length():
            raise ValueError(f'Expected complete BC3 mip chain: {path}')
        size = sum(max(1, ((w >> i) + 3) // 4) * max(1, ((h >> i) + 3) // 4) * 16 for i in range(levels))
    else:
        raise ValueError(f'Unsupported texture format: {pixel_format}')
    if [w, h] != list(expected_size) or len(data) != 128 + size:
        raise ValueError(f'Invalid dimensions or mip chain: {path}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pbrify', type=Path, required=True)
    parser.add_argument('--fonts', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.resolve() in (args.pbrify.resolve(), args.fonts.resolve()):
        parser.error('The combined package must have its own output directory')
    originals = json.loads((args.pbrify / 'originals.json').read_text())
    validation = json.loads((args.pbrify / 'validation.json').read_text())
    fonts = json.loads((args.fonts / 'fonts.json').read_text())
    if originals['unsupported'] or validation['recipe'] != RECIPE or validation['textures'] != len(originals['textures']):
        raise ValueError('Expected a fully validated original PBRify pack')
    if set(fonts) != {'Big', 'Small'}:
        raise ValueError('Expected exactly the Big and Small font replacements')
    font_keys = {}
    for name, font in fonts.items():
        matches = [entry for entry in originals['textures'] if any(
            s['path'].lower() == f'language/fonts/{name.lower()}.bin' for s in entry['sources'])]
        if len(matches) != 1 or matches[0]['key'] != font['key'] or matches[0]['source_sha256'] != font['original_sha256']:
            raise ValueError(f'Font does not match the original Xbox atlas: {name}')
        entry = matches[0]
        factor = 4 if name == 'Big' else 8
        if font['size'] != [entry['width'] * factor, entry['height'] * factor]:
            raise ValueError(f'Unexpected font dimensions: {name}')
        font_keys[font['key']] = (name, font)
    destination = args.output / 'textures'
    destination.mkdir(parents=True, exist_ok=True)
    expected = {entry['key'] for entry in originals['textures']}
    if {p.stem for p in destination.iterdir()} - expected:
        raise ValueError('Unexpected files in the combined texture directory')
    ready = args.output / 'manifest.json'
    ready.unlink(missing_ok=True)
    textures = []
    for entry in originals['textures']:
        key = entry['key']
        if key in font_keys:
            name, font = font_keys[key]
            if font['format'] != 'BGRA8':
                raise ValueError(f'Expected lossless font pixels: {name}')
            source, digest, size = args.fonts, font['dds_sha256'], font['size']
            record = dict(font, name=name, source='HD Reimagined font')
        else:
            result = json.loads((args.pbrify / 'records' / f'{key}.json').read_text())
            if result['recipe'] != RECIPE or result['source_sha256'] != entry['source_sha256']:
                raise ValueError(f'Stale PBRify result: {key}')
            source, digest = args.pbrify / 'textures', result['dds_sha256']
            size = [entry['width'] * 4, entry['height'] * 4]
            record = dict(key=key, name=entry['name'], source='PBRify original Xbox', size=size, dds_sha256=digest, format='BC3')
        filename = f'{key}.dds'
        check_dds(source / filename, digest, size, record['format'])
        shutil.copyfile(source / filename, destination / filename)
        check_dds(destination / filename, digest, size, record['format'])
        textures.append(record)
    if {p.name for p in destination.iterdir()} != {f'{key}.dds' for key in expected}:
        raise ValueError('Combined texture coverage mismatch')
    report = dict(schema=1, pbrify_textures=len(textures)-2, mod_font_atlases=2,
                  recipe=RECIPE, font_sources=fonts, textures=textures,
                  checks=['source and copied DDS hashes', 'complete BC3 mip chains and lossless BGRA8 fonts', 'exact original key coverage',
                          'only Big and Small overridden with mod artwork'])
    ready.write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {len(textures)-2} PBRify textures + 2 HD Reimagined fonts; {len(textures)} DDS files')


if __name__ == '__main__':
    main()
