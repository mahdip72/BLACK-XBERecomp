# PBRify textures with HD Reimagined fonts

The combined pack uses 2,019 original Xbox textures upscaled with
[4x-PBRify_UpscalerV4](../tools/PBRIFY.md) and two adapted font atlases from
[BLACK HD Reimagined](https://www.moddb.com/mods/black-hd-reimagined/addons/black-hd-reimagined-texture-pack)
by HDREIMAGINED. Only the fonts use mod artwork. Menu maps, loading graphics,
BLACK logos, world textures, weapons and effects use the upscaled Xbox artwork.

The shared Big and Small fonts cover HUD and menu text while retaining original
glyph positions, spacing and character tables. Controller symbols retain Xbox
artwork. Text baked into other images follows the PBRify texture replacement.
Big is 1024x1024 and Small is 1024x2048. These are detailed bitmap fonts suitable
for high display resolutions. PBRify enlarges each original dimension four times.

## Prepare one pack

First generate and validate the complete original-only pack using the
[PBRify instructions](../tools/PBRIFY.md). Then run these commands with Python,
Pillow and NumPy, your own US Xbox XISO, and the extracted mod's
`SLUS-21376/replacements` directory:

```powershell
python tools/import_hd_fonts.py --iso D:/Black.iso --mod D:/mod/SLUS-21376/replacements --output out/hd-fonts
python tools/package_textures.py --pbrify out/pbrify-pack --fonts out/hd-fonts --output out/texture-pack
```

The font importer repacks only the Big and Small glyphs and writes lossless
BGRA8 DDS files with one mip, matching the original font sampling. The packager
copies the PBRify textures and replaces
exactly those two entries with the mod fonts. It verifies source and copied
hashes, dimensions, file layouts and complete original texture-key coverage.
`out/texture-pack/manifest.json` records the source and hash of every output.
The original-only PBRify pack remains unchanged.

Put the combined pack's `textures` directory beside the newly built executable.
No separate `fonts` directory or font resource hook is needed. Restart after
changing the pack. Missing or malformed DDS files use the original texture.
Rename `textures` with the game closed to disable the pack. Normal game
installation requirements still apply. Dynamic render targets and FMV frames
are outside this static texture pack.

The repository contains only code and font mapping metadata. Game artwork,
generated textures and model weights stay local. The mod is proprietary and its
artwork is supplied separately. The reviewed archive has MD5
`f7384ede03583784f68de43da83742c5`.
