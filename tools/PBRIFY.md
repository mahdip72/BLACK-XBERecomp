# Original textures at 4x resolution

This pack is generated from the user's Xbox XISO using only
[4x-PBRify_UpscalerV4](https://github.com/Kim2091/Kim2091-Models/releases/tag/4x-PBRify_UpscalerV4).
It contains no HD Reimagined artwork. A 128x128 texture becomes 512x512;
4x means each dimension, not a fixed 4K size.

Use Python 3.12 with Pillow, NumPy, psutil, Spandrel 0.4.2, and a compatible
PyTorch/torchvision pair. This batch was prepared with PyTorch 2.7.1 and
torchvision 0.22.1 (CUDA 11.8) on an RTX 2070. Download the author's
`4x-PBRify_UpscalerV4.safetensors` checkpoint separately; the script verifies
its SHA-256 before inference.

```powershell
python tools/upscale_pbrify.py extract --iso path/to/Black.iso --output out/pbrify-pack
python tools/upscale_pbrify.py run --model path/to/4x-PBRify_UpscalerV4.safetensors --output out/pbrify-pack
python tools/upscale_pbrify.py validate --output out/pbrify-pack
```

Extraction audits named serialized textures in the game's BIN/DB/XBE files,
including BC1, BC2, BC3, paletted P8 and swizzled RGBA assets. Identical
textures share one content key; P8 keys include their palette. The local
manifest retains every source location.

Inference uses float32, one model worker and two CPU threads. Large images
use overlapping tiles with context. RGB passes through PBRify; original
alpha is resized with Lanczos to preserve opacity and specular values.
Texture layouts, UVs and font metrics stay unchanged. Outputs are BC3 DDS
files with complete mip chains, plus lossless PNGs for review.

Rerun the same command to resume. Completed files are checked against their
source, recipe and output hashes. The batch pauses while a `BLACK*.exe`
process runs, or while an empty `PAUSE` file exists in the output folder.
An error stops the batch without silently skipping an asset.

Put the generated `textures` directory beside the newly built executable.
The renderer replaces matching host textures and keeps the original game
files untouched. Missing or malformed replacements fall back to originals.
Restart the game after adding/removing a pack. Dynamic render targets and
FMV video frames are outside this static texture pack.

Keep originals, generated artwork, checkpoints, manifests and local records
outside Git. Visual campaign testing is still needed to assess AI artifacts,
transparent edges, tiled surfaces and atlas boundaries in motion.
