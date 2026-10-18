# Black - XBE Recomp
An unofficial native PC port of the original Xbox version of Black, with extended graphics options, keyboard and mouse support, and other enhancements.

This port was created using the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of the game to install or build.

## Installation
- Put your game files in the `assets` folder. This can be an ISO, or the extracted data files, of the USA or Europe release.
- Run `BLACK.exe`.
- Recomp settings can be adjusted in the in-game options menu while the game is running, or through the Settings overlay, which is opened with F10 by default.
<img width="1920" height="1080" alt="{5852D359-24A0-48C8-9319-07C3A619E355}" src="https://github.com/user-attachments/assets/e0156eb8-c1b5-49d9-ab93-3abb46bf5f47" />


## Features
### Display
- **Monitor** - Picks the monitor the game shows on, for Fullscreen and Borderless (and where Windowed opens). Lists every connected monitor by its Windows number and desktop resolution.
- **Display Mode** - Switch between Fullscreen, Borderless and Windowed.
- **Resolution** - Lists every mode your monitor reports and defaults to your desktop resolution, replacing the original 640x480 resolution.
- **Render Resolution** - Renders the game at 20% to 200% of the output resolution, allowing supersampling or downscaling independent of the window size.
- **Field of View** - Sets the horizontal field of view during gameplay from 45 to 140 degrees, with 70 being the game's default.
- **Viewmodel Field of View** - Adjusts the first-person weapon and arms field of view.
- **Max Frame Rate** - Caps the frame rate to your monitor's refresh rate or a custom value, or removes the cap entirely, replacing the Xbox's 30 FPS limiter.
- **V-Sync** - Reduces screen tearing.
- **HDR Output** - Enables HDR10 output on displays with Windows HDR turned on, detected automatically. Highlights extend up to the display's peak brightness.
- **FPS Counter** - Displays the frame rate on screen.
- **FMV Scale** - Choose between Fill, which zooms the 4:3 FMVs to fill widescreen displays, and 4:3, which keeps them at their original aspect ratio.

### Graphics
- **Multisample Antialiasing (MSAA)** - Enables 2x, 4x or 8x MSAA to smooth jagged edges while preserving a sharp image.
- **Subpixel Morphological Antialiasing (SMAA)** - Enables SMAA to smooth jagged edges across the whole frame with a softer image. SMAA also smooths alpha-tested edges such as fences and foliage, and can be combined with MSAA.
- **Anisotropic Filtering** - Forces the selected anisotropic filtering level on all textures.
- **Draw Distance** - Increases the level of detail and draw distances past the game's values: 2x, 4x or 8x.
- **Ambient Occlusion** - Adds screen space ambient occlusion, creating soft contact shadows where surfaces meet. Choose between SSAO and GTAO.
- **Reload Blur** - Off, Original, Balanced DOF, or FidelityFX DOF. FidelityFX uses AMD's official DOF component. Both modern modes keep the reload weapon and HUD sharp and share eased transitions; normal gameplay can use a separate light weapon blur. F10 opens a compact right-side tuner with live sliders and a reload preview toggle.
- **DOF Quality** - FidelityFX Standard retains the half-resolution filter. Cinematic uses native-resolution gathering, up to eight sample rings, and disables ring merging. It uses substantially more GPU time and memory. Bokeh Size changes the blur radius independently of quality; focus and blur-strength tuning are shared between modes.

### Input
- **Mouse and Keyboard** - Full keyboard and mouse support with raw input, adjustable sensitivity, inverted look and rebindable controls.
- **Controller** - Controller with SDL3 support. 

### General
- **Skip Intro** - Skips the intro logos.
- **Pause On Focus Loss** - Pauses the game and its sound while the window is in the background or minimized.

### Miscellaneous
- **Custom Menu Cursor** - Place a `cursor.png` (or `cursor.dds`) beside the exe to replace the Windows arrow in the game's menus.

## System Requirements
### Minimum
- **OS**
  - Windows 10 64-bit
- **CPU** with 2 cores and support for SSE4.2 and POPCNT
  - Intel Nehalem (Intel Core 1st Generation)
  - AMD Bulldozer (AMD FX series)
- **GPU** with support for Direct3D 12
  - NVIDIA GeForce GT 630 (Kepler)
  - AMD Radeon HD 7750
  - Intel HD Graphics 510 (Skylake)
- **Memory**
  - 4 GB
- **Storage**
  - 2 GB for the game files and saves

## Building
### 1. Clone the Repository
Clone this repository using [Git](https://git-scm.com/).
```
git clone https://github.com/TGP482/BLACK-XBERecomp.git
```

### 2. Add the Required Game Files
Place your extracted game data files in the `assets` folder. `default.xbe` from the USA release must be there, with the rest of the game's files beside it.


### 3. Install Dependencies
Install [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) and select the following **Workloads** and **Individual components**:
- Desktop development with C++
- C++ Clang tools for Windows
- C++ CMake tools for Windows

> [!TIP]
> `build.bat` offers to install the Clang and CMake components for you if they're missing.

### 4. Build the Project
- Prepare the pinned DOF dependency once: `powershell -NoProfile -ExecutionPolicy Bypass -File tools\cmake\prepare_fidelityfx_sdk.ps1`. Only the official FidelityFX DOF component and DX12 backend are built. To build without them, configure with `-DBLACK_FIDELITYFX_DOF=OFF`.
- Run `build.bat`. The first build recompiles the entire game and can take 30 to 90 minutes on weaker systems.
- When it finishes, run `out\build\release\BLACK.exe`.

### Depth-of-field checks
- Run `powershell -NoProfile -ExecutionPolicy Bypass -File tools\tests\run_dof_tests.ps1` from the repository root. The focused checks compile the production shader, render synthetic fixtures with the CPU-only D3D11 WARP device, and test the reload hooks. They do not start the game.
- Results and rendered synthetic fixtures are saved under `out\tests\dof-tests`. The checks cover depth separation, continuous sloped surfaces, thin geometry, fractional-radius continuity, foreground edges, alpha, viewport bounds, and 2x/4x/8x MSAA. They do not validate the D3D12 device, gameplay appearance, or frame rate.
- Run `tools\tests\run_viewmodel_dof.cmd` for separate CPU-only WARP checks of weapon coverage, depth falloff, silhouette protection, and alpha at 1x/2x/4x/8x MSAA. Its executable goes under `out\tests\dof-tests`.
- For visual checking, use F10's Reload Blur selector to compare Original, Balanced DOF, and FidelityFX DOF. Check fade-in/out, interrupted and multipart reloads, weapon/HUD sharpness, and nearby silhouettes with MSAA off and on. General settings and keyboard bindings remain accessible from the tuner.
- Turn **Preview reload DOF** on to fade the world effect in over 0.8 s and hold it without starting a reload, with a sharp weapon and HUD. Turn it off to check the 0.9 s fade back to normal play. Reversing mid-fade continues from the current value. Closing the panel leaves preview active; selecting Original or Off clears it. Preview starts off each launch and is not saved.
- Modern DOF also focuses on the weapon during the authored inspection animation after prolonged idle time. It follows the animation graph's actual inspection section, shares the preview's smooth focus fade, and leaves ordinary idle sway unchanged. Reloads retain priority.
- Five controls update live and save on release: **Focus distance** (1–30 world units; default 5), **Near blur** (0–150%; default 75%), **Distant blur** (0–150%; default 25%), **Bokeh size** (4–30 px at nominal 1080p; default 12), and **Gameplay weapon blur** (0–100%; default 25%). **Reset DOF tuning** restores these five values and Cinematic quality without changing other settings.
- Start with **DISTANT BLUR** to reduce distant softness without changing the near blur. **FOCUS DISTANCE** moves the sharp depth band, while **BOKEH SIZE** changes the maximum circle size. World controls affect the reload effect; **GAMEPLAY WEAPON BLUR** affects normal play. Set the latter to zero to disable its work. Off and Original disable both modern effects. Both world strengths at zero bypass the reload filter while suppressing the original blur.
- **Weapon sharp depth** (default 1.00) sets where normal-play weapon blur reaches zero. **Weapon focus transition** (default 0.75) sets the depth interval leading up to that point; the defaults give full blur through 0.25, then gradually sharpen through 1.00. These are camera view-depth units, not verified metres; both controls update live and reset with DOF tuning. The transition cannot exceed the sharp depth. Tune with preview off. Preview fades weapon blur out as world blur fades in, and reverses this when disabled; actual reloads keep the weapon and hands fully sharp.
- FidelityFX uses the pinned AMD SDK v1.1.4 (`c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`) and its five compute stages. It requires Shader Model 6.2 and wave operations; unsupported devices or SDK failures fall back to Balanced with a log message. `tools\tests\run_newffx_dof_warp.cmd` builds and runs its offscreen D3D12 WARP checks from `out\build\fidelityfx-dof` (pass another build directory as its first argument).
- Filter inputs extend the viewport edge to a 64-pixel boundary for the SDK's workgroups and mip chain; only the original viewport is written back, without resizing the image or blur radius. The D3D12 checks cover this odd-size path, foreground spreading, focus preservation, viewport/alpha preservation, and mixed MSAA input. Gameplay appearance and hardware GPU timing remain separate checks.
- Balanced retains its half-resolution 64+32-tap filter for comparison. FidelityFX accepts one depth value per pixel, so resolved MSAA color with nearest depth has different silhouette coverage from Balanced. Neither path restores missing source texture detail or hidden background color.
- Modern DOF filters verified late native alpha billboards at their own depth before blending, using the same focus, near/far strength, bokeh radius and eased transition as the world effect. A 64-tap aperture expands their raster support, gathers premultiplied color/alpha and checks opaque depth at source taps. Earlier particles already included in world DOF keep their native path. Unsupported materials, atlas rectangles or geometry also keep native rendering; additive particles are not covered. Native reduced-resolution particle layers and their composites remain, so their original resolution still limits quality.
- `tools\tests\run_new_smoke_dof_warp.cmd` builds the headless particle checks against production geometry and translated native shaders. It uses the software WARP adapter and does not start the game.
- Modern reloads use at least 0.8 s to fade in and 0.9 s to fade out, with eased endpoints. The original game timer still handles holds and cancellation. Normal gameplay weapon blur uses the depth samples changed by the first-person draw, with a small radius that decreases with distance. It preserves the silhouette and leaves parts without depth writes sharp. Focus calibration, highlights, transparency, and actual GPU performance still need gameplay review.
