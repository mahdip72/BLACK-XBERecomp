# Optional HD textures

This branch builds on `codex/fix-weapon-depth-order` at `914edcb` and includes the approved HD HUD fonts. It adds optional texture replacement throughout the renderer.

Game files and replacement artwork stay outside Git. The source contains the loader, import tools and mapping metadata. Obtain [BLACK HD Reimagined](https://www.moddb.com/mods/black-hd-reimagined/addons/black-hd-reimagined-texture-pack) separately from HDREIMAGINED and use your own Xbox game files. The mod is a proprietary PS2/PCSX2 pack; its artwork is not licensed by this repository.

## Prepare a pack

Build the game normally. The import tools require Python, Pillow and NumPy (NumPy is only needed for fonts). Extract the mod archive and locate `SLUS-21376/replacements`. The reviewed archive has MD5 `f7384ede03583784f68de43da83742c5`.

Run these commands from the repository, replacing the example paths. Use a fresh output directory when changing import maps; old DDS files are still active if left in `textures`.

```powershell
python tools/import_hd_textures.py --iso "D:/Black.iso" --mod "D:/mod/SLUS-21376/replacements" --output "D:/Black-HD/textures"
python tools/import_hd_fonts.py --iso "D:/Black.iso" --mod "D:/mod/SLUS-21376/replacements" --output "D:/Black-HD/fonts"
```

Put those `textures` and `fonts` directories beside the executable, then start the game. The importer reads the original US Xbox XISO and checks every texture's dimensions, format, palette and content hash before converting. Unsupported original textures retain the game's artwork. Normal game installation requirements still apply.

To disable replacements, close the game and rename the two directories. Saves and the game image are not patched.

## Coverage and limits

The initial map contains **1,056 Xbox texture variants**, using **863 mod files**, plus the **two font files**. It covers image-matched scenery, props, character and weapon atlases, selected effects and UI images. The fonts retain the Xbox glyph layout, metrics and controller symbols.

**459 of the mod's 1,324 texture files remain unmapped.** Some images are alternatives or use different PS2 atlas layouts; other identities remain uncertain. There is no verified original PS2 texture dump available for matching. `tools/hd_textures/coverage.json` lists every mod file and its current status. These are image-matched candidates with manual corrections, not a complete campaign playtest or a complete port of the mod.

The pack does not change models, geometry, videos or level data. Texture sizes follow the supplied artwork; they are not all 4096 pixels. The mod's PCSX2 FOV patch is not used.

## How it works

The renderer fingerprints original texture contents, including palette colours for indexed textures, and looks for `textures/<fingerprint>.dds`. Dimensions or game addresses alone cannot select a replacement. Converted files are BC3 DDS with full mip chains. Most assets retain the Xbox alpha channel, which can encode material properties; reviewed foliage and effects use the mod's opacity. Font atlases use the separate font callback.

Missing or malformed replacements fall back to the original. If allocation of an HD texture fails, the renderer attempts the original resource. Unused HD resources can be released during normal cache sweeps. Large texture packs still need more graphics memory and can add loading stalls.

## Checks performed

- Windows Release build with the finalized FOV changes.
- Native loader checks for BC1, BC2, BC3, indexed P8 and swizzled RGBA originals, including palette identity, every imported mip and malformed-file fallback.
- DDS header and mip-byte checks for every generated replacement.
- Font generation reproduced the previously approved PNGs byte for byte.
- The isolated 3840x2160 runtime opened its menus and logged 95 HD texture uploads during the Tivliz Asylum mission load. The test instance was closed when a separate glow-tuning game session started; gameplay visuals were not fully checked.

Visual testing across all missions remains open. A successful import verifies conversion and source identity; it does not prove that every image match or material looks correct in gameplay.
