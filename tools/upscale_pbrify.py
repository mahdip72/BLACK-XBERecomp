"""Resumable, original-art-only 4x-PBRify_UpscalerV4 texture pack builder."""
import argparse
import json
import time
from pathlib import Path

import numpy as np
from PIL import Image

from pbrify_assets import extract, sha256, write_dds

MODEL_SHA256 = 'a00fa50116895a9b766aeb012edad0a14d66f3afda65539efc0f6fc28c991ba2'
RECIPE = dict(model='4x-PBRify_UpscalerV4', model_sha256=MODEL_SHA256, scale=4,
              precision='float32', alpha='original Lanczos', tile=256, context=32,
              overlap=32, format='BC3 full mip chain', revision=1)


def atomic_json(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def wait_for_game(root):
    import psutil
    waiting = False
    while True:
        games = [p.info['name'] for p in psutil.process_iter(['name'])
                 if (p.info['name'] or '').lower().startswith('black')
                 and (p.info['name'] or '').lower().endswith('.exe')]
        if not games and not (root / 'PAUSE').exists():
            if waiting:
                print('Resuming: game closed and PAUSE absent', flush=True)
            return
        if not waiting:
            print(f'Waiting for BLACK to close or PAUSE to be removed: {games}', flush=True)
            # Return the allocator's unused VRAM while another game is active.
            import sys
            torch = sys.modules.get('torch')
            if torch is not None and torch.cuda.is_available():
                torch.cuda.empty_cache()
            waiting = True
        time.sleep(5)


def upscale(model, image, device, gate=lambda: None):
    """Full inference up to 256px; larger images use context and feathered overlaps."""
    import torch
    width, height = image.size
    source = np.asarray(image.convert('RGB'), dtype=np.float32) / 255
    output = np.zeros((height * 4, width * 4, 3), dtype=np.float32)
    weights = np.zeros((height * 4, width * 4, 1), dtype=np.float32)

    def starts(length, core):
        if length <= core:
            return [0]
        return list(range(0, max(1, length - core + 1), core - 32)) + ([length - core] if length > core and (length - core) % (core - 32) else [])

    # Most assets fit a single pass. Large textures retain 32 source pixels of
    # context on each tile and blend overlapping results to suppress seams.
    cw, ch = (size if size <= 256 else 192 for size in (width, height))
    for y in starts(height, ch):
        for x in starts(width, cw):
            gate()
            x0, y0 = max(0, x - 32), max(0, y - 32)
            x1, y1 = min(width, x + cw + 32), min(height, y + ch + 32)
            patch = source[y0:y1, x0:x1]
            ph, pw = patch.shape[:2]
            if min(ph, pw) < 16:
                patch = np.pad(patch, ((0, max(0, 16 - ph)), (0, max(0, 16 - pw)), (0, 0)), mode='reflect')
            tensor = torch.from_numpy(np.ascontiguousarray(patch.transpose(2, 0, 1)[None])).to(device)
            with torch.inference_mode():
                prediction = model(tensor)
                tile = prediction[0, :, (y-y0)*4:(y-y0+ch)*4, (x-x0)*4:(x-x0+cw)*4].float().cpu().numpy().transpose(1, 2, 0)
            if tile.shape != (ch * 4, cw * 4, 3) or not np.isfinite(tile).all():
                raise ValueError('Invalid model output')
            wx, wy = np.ones(cw * 4, np.float32), np.ones(ch * 4, np.float32)
            fade = 32 * 4
            if x:
                wx[:fade] = np.linspace(1 / fade, 1, fade)
            if x + cw < width:
                wx[-fade:] = np.linspace(1, 1 / fade, fade)
            if y:
                wy[:fade] = np.linspace(1 / fade, 1, fade)
            if y + ch < height:
                wy[-fade:] = np.linspace(1, 1 / fade, fade)
            weight = wy[:, None, None] * wx[None, :, None]
            output[y*4:(y+ch)*4, x*4:(x+cw)*4] += tile * weight
            weights[y*4:(y+ch)*4, x*4:(x+cw)*4] += weight
            del prediction, tensor
    if not (weights > 0).all():
        raise ValueError('Incomplete tile coverage')
    rgb = np.rint(np.clip(output / weights, 0, 1) * 255).astype(np.uint8)
    result = Image.fromarray(rgb).convert('RGBA')
    result.putalpha(image.getchannel('A').resize((width * 4, height * 4), Image.Resampling.LANCZOS))
    return result


def complete(root, entry):
    key = entry['key']
    record = root / 'records' / f'{key}.json'
    if not record.exists():
        return False
    data = json.loads(record.read_text())
    return (data['recipe'] == RECIPE and data['source_sha256'] == entry['source_sha256']
            and all((root / folder / f'{key}.{ext}').exists()
                    and sha256(root / folder / f'{key}.{ext}') == data[f'{ext}_sha256']
                    for folder, ext in (('upscaled', 'png'), ('textures', 'dds'))))


def run(args, manifest):
    import torch
    from spandrel import ImageModelDescriptor, ModelLoader
    if sha256(args.model) != MODEL_SHA256:
        raise ValueError('Model is not the selected author checkpoint')
    if args.device == 'cuda' and not torch.cuda.is_available():
        raise RuntimeError('CUDA unavailable; use a CUDA-enabled PyTorch installation')
    if args.device == 'cuda':
        # Leave room for the desktop; the benchmark and batch use the same cap.
        torch.cuda.set_per_process_memory_fraction(0.55)
    torch.set_num_threads(2)
    torch.set_num_interop_threads(1)
    for name in ('textures', 'upscaled', 'records'):
        (args.output / name).mkdir(parents=True, exist_ok=True)
    remaining = [e for e in manifest['textures'] if not complete(args.output, e)]
    print(f'{len(manifest["textures"])-len(remaining)} complete, {len(remaining)} remaining', flush=True)
    if not remaining:
        return
    wait_for_game(args.output)
    model = ModelLoader().load_from_file(args.model)
    if not isinstance(model, ImageModelDescriptor) or model.scale != 4:
        raise ValueError('Expected a 4x image model')
    # DAT explicitly does not support float16 in Spandrel. Keep RGB inference FP32.
    model.to(device=args.device, dtype=torch.float32).eval()
    print(f'Loaded {model.architecture.name}, {args.device}, float32', flush=True)
    atomic_json(args.output / 'recipe.json', RECIPE)
    count, total = len(manifest['textures']) - len(remaining), len(manifest['textures'])
    for entry in sorted(remaining, key=lambda e: (e['width'] * e['height'], e['key'])):
        wait_for_game(args.output)
        started = time.perf_counter()
        key = entry['key']
        original = args.output / 'originals' / f'{key}.png'
        if sha256(original) != entry['png_sha256']:
            raise ValueError(f'Original PNG changed: {key}')
        image = Image.open(original).convert('RGBA')
        result = upscale(model, image, args.device, lambda: wait_for_game(args.output))
        png = args.output / 'upscaled' / f'{key}.png'
        dds = args.output / 'textures' / f'{key}.dds'
        temporary = png.with_suffix('.tmp')
        result.save(temporary, format='PNG')
        temporary.replace(png)
        temporary = dds.with_suffix('.tmp')
        write_dds(result, temporary)
        temporary.replace(dds)
        record = dict(key=key, recipe=RECIPE, source_sha256=entry['source_sha256'],
                      png_sha256=sha256(png), dds_sha256=sha256(dds),
                      device=args.device, torch_version=torch.__version__,
                      seconds=round(time.perf_counter() - started, 3),
                      output_size=list(result.size))
        atomic_json(args.output / 'records' / f'{key}.json', record)
        count += 1
        atomic_json(args.output / 'progress.json', dict(complete=count, total=total, last=record, state='running'))
        print(f'{count}/{total} {key} {entry["name"]} {image.size} -> {result.size} {record["seconds"]}s', flush=True)
    atomic_json(args.output / 'progress.json', dict(complete=count, total=total, state='complete'))


def validate(root, manifest):
    import struct
    for entry in manifest['textures']:
        if not complete(root, entry):
            raise ValueError(f'Missing/stale output: {entry["key"]}')
        key = entry['key']
        image = Image.open(root / 'upscaled' / f'{key}.png').convert('RGBA')
        expected = (entry['width'] * 4, entry['height'] * 4)
        assert image.size == expected, key
        source = root / 'originals' / f'{key}.png'
        if sha256(source) != entry['png_sha256']:
            raise ValueError(f'Original PNG changed: {key}')
        original = Image.open(source).convert('RGBA')
        assert image.getchannel('A').tobytes() == original.getchannel('A').resize(expected, Image.Resampling.LANCZOS).tobytes(), key
        dds = (root / 'textures' / f'{key}.dds').read_bytes()
        assert dds[:4] == b'DDS ' and dds[84:88] == b'DXT5', key
        h, w = struct.unpack_from('<II', dds, 12)
        levels = struct.unpack_from('<I', dds, 28)[0]
        assert (w, h) == expected and levels == max(w, h).bit_length(), key
        length = sum(max(1, ((w >> i) + 3) // 4) * max(1, ((h >> i) + 3) // 4) * 16 for i in range(levels))
        assert len(dds) == 128 + length, key
    expected_keys = {e['key'] for e in manifest['textures']}
    if {p.stem for p in (root / 'textures').glob('*.dds')} != expected_keys:
        raise ValueError('Unexpected/missing DDS files in pack')
    report = dict(textures=len(manifest['textures']), occurrences=manifest['occurrences'],
                  formats=manifest['formats'], unsupported=manifest['unsupported'], recipe=RECIPE,
                  checks=['source and output hashes', 'exact 4x dimensions', 'original alpha Lanczos',
                          'BC3 header, complete mip chain and byte count'])
    atomic_json(root / 'validation.json', report)
    print(json.dumps(report, indent=2), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['extract', 'run', 'validate'])
    parser.add_argument('--iso', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--model', type=Path)
    parser.add_argument('--device', choices=['cuda', 'cpu'], default='cuda')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.action == 'extract':
        if not args.iso:
            parser.error('extract requires --iso')
        print(json.dumps({k: v for k, v in extract(args.iso, args.output).items() if k != 'textures'}, indent=2))
        return
    manifest = json.loads((args.output / 'originals.json').read_text())
    if args.action == 'run':
        if not args.model:
            parser.error('run requires --model')
        run(args, manifest)
    else:
        validate(args.output, manifest)


if __name__ == '__main__':
    main()
