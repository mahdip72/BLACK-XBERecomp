# AMD FidelityFX SDK

BLACK links the official FidelityFX Depth of Field host component and DirectX 12 backend from AMD's FidelityFX SDK `v1.1.4` tag at commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`.

The project uses the SDK's DoF component, shared host sources, DX12 backend, PIX headers/library, shader compiler driver, and DoF HLSL sources. CMake enables only `FFX_DOF`; all other effects and `FFX_ALL` are disabled. The official compiler emits the SDK's four wave/precision shader permutations per pass as generated headers in the build tree. The SDK source itself stays in ignored `out/deps/FidelityFX-SDK`.

Nine pinned patches are applied in the order 1–6, then 9, 12, and 14. Patches 1–3 fix SDK build and tile-height issues. Patches 4–6 preserve the virtual 2× gather and reconstruction while using native input storage, averaging four individually rounded RGBA16F outputs into RGBA32F, and resetting the global radius once. High defaults to virtual inputs and fused output (`BLACK_DOF_WORLD_MODE=2`); `0` selects materialized storage and `1` selects virtual inputs alone.

High defaults to the exact three-entry world cache (`BLACK_DOF_WORLD_REUSE=4`). It compares prepared color/depth bits and effective constants, restores a matching stored result, and otherwise runs the original five SDK passes. Changed inputs never accept an approximate match. `0` disables reuse; `3` selects the previous-world cache. Experimental group comparison (`1`) and guarded group reuse (`2`) remain available for diagnostics. If cache creation fails, preparation retries the same FidelityFX quality without caching. Medium retains its original path. Multi-entry caching adds GPU memory and helps recurring views; a continuously changing view still needs the full blur.

- [0001-fix-shader-output-list.patch](patches/0001-fix-shader-output-list.patch), SHA-256 `C3CD383225C5E5FF2C0819A26388EA584B99A6039CAACF72F73424BFB0922250`
- [0002-include-bit-for-popcount.patch](patches/0002-include-bit-for-popcount.patch), SHA-256 `C027744FC54453C31D0802AB5E9C669D302AB529EC883D061C296E5456395597`
- [0003-fix-dof-tile-height.patch](patches/0003-fix-dof-tile-height.patch), SHA-256 `AE28E56C63E34402855382B149A5EC2DF62CDE70EDBD5A102FB44863BFEB1861`
- [0004-dof-virtual-input.patch](patches/0004-dof-virtual-input.patch), SHA-256 `65979927648B8949D5705F0A22794B2ACF280052CB02CE537DB8FD7C62B67CA8`
- [0005-dof-fused-output.patch](patches/0005-dof-fused-output.patch), SHA-256 `C60CE19CE963D12076D6BACEE38673DE75EC1CC210A90238992B753433DB61B9`
- [0006-dof-single-global-reset.patch](patches/0006-dof-single-global-reset.patch), SHA-256 `7877C9FB6EC53C4AF83A88B66C31DD49EA76CA27A4626BFE18DF3B2FCF215463`
- [0009-dof-exact-reuse-candidate.patch](patches/0009-dof-exact-reuse-candidate.patch), SHA-256 `25AD52424DA9A223B6850C27EBC7888EE1E6EF16E254FBBA0B66D0091231C1DA`
- [0012-dof-exact-world-cache.patch](patches/0012-dof-exact-world-cache.patch), SHA-256 `FE81D9A7BD265A51AB543ED20523F13E0F6268E21161CEA7FCA52384F3C263BF`
- [0014-dof-exact-world-multi-cache.patch](patches/0014-dof-exact-world-multi-cache.patch), SHA-256 `6B20BAC15A002E789019171461CE8785E0A735D486ECE915925428FEDF8FF727`

Prepare the sparse pinned checkout from the repository root with:

```powershell
powershell -ExecutionPolicy Bypass -File tools/cmake/prepare_fidelityfx_sdk.ps1
```

Then configure the ordinary Black build. FidelityFX is enabled by default; set `-DBLACK_FIDELITYFX_DOF=OFF` only for a build that intentionally uses the adapter's unavailable-component fallback.

The SDK is distributed under the MIT License. Its full upstream license notice is copied to [LICENSE.txt](LICENSE.txt). Source and tag: <https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4>.
