# Optional HD fonts and menus

This branch builds on the finalized FOV/weapon fix at `914edcb`. Replacements are limited to the approved fonts and menu map/loading graphics. The BLACK logos retain their original artwork. World, weapon, character and effect textures retain their original artwork.

The repository contains source and mapping metadata. Artwork is imported locally from [BLACK HD Reimagined](https://www.moddb.com/mods/black-hd-reimagined/addons/black-hd-reimagined-texture-pack) by HDREIMAGINED. The mod is proprietary; its artwork is supplied separately and is not licensed by this repository.

## Prepare the optional assets

The import tools require Python, Pillow and NumPy (NumPy is only needed for fonts), your own US Xbox XISO, and the extracted mod's `SLUS-21376/replacements` directory. The reviewed mod archive has MD5 `f7384ede03583784f68de43da83742c5`.

```powershell
python tools/import_hd_textures.py --iso "D:/Black.iso" --mod "D:/mod/SLUS-21376/replacements" --output "D:/Black-HD/textures"
python tools/import_hd_fonts.py --iso "D:/Black.iso" --mod "D:/mod/SLUS-21376/replacements" --output "D:/Black-HD/fonts"
```

Put `textures` and `fonts` beside the executable. Normal game installation requirements still apply. The tools read the game image without patching it. Rename these directories with the game closed to disable replacements.

## Scope

- Two font atlases retain the approved glyphs, Xbox text metrics and controller symbols. HUD and menu text use these shared fonts.
- Eleven menu pages cover ten mission-map tiles and the loading bullet artwork.
- All BLACK logo atlases retain the original game artwork.
- Other menu icons and Xbox controller diagrams keep their original artwork where no compatible replacement was verified. Videos are unchanged.

The renderer also checks a fixed list of menu texture fingerprints. Older world-texture packs cannot enable world replacements in this build. Missing or malformed DDS files keep the original textures. The importer checks original content and produces BC3 DDS with mipmaps. `tools/hd_textures/coverage.json` lists the selected assets.

## Validation

The Windows Release build passed against the latest fork main. The narrowed loader passed offline checks for all 11 menu pages, every mip, malformed DDS fallback and rejection of world and logo textures even with valid replacement files present. The approved font PNGs remain unchanged. Fonts and menu graphics were accepted in visual testing. Logo replacements were removed after that review.
