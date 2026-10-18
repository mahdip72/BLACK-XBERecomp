# AMD FidelityFX SDK

BLACK links the official FidelityFX Depth of Field host component and DirectX 12 backend from AMD's FidelityFX SDK `v1.1.4` tag at commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`.

The project uses the SDK's DoF component, shared host sources, DX12 backend, PIX headers/library, shader compiler driver, and the five DoF HLSL sources. CMake enables only `FFX_DOF`; all other effects and `FFX_ALL` are disabled. The official compiler emits the SDK's four wave/precision shader permutations per pass as generated headers in the build tree. The SDK source itself stays in ignored `out/deps/FidelityFX-SDK`.

Three small upstream compatibility/correctness fixes are applied to the isolated sparse checkout before configuration. Patch 1 corrects the shader helper's output-variable expansion; patch 2 adds the missing standard `<bit>` include required by its own `std::popcount` use; patch 3 corrects the DoF host component's tile-height rounding so the allocated tile grid covers the dispatch extent. The third patch changes allocation sizing only; it does not modify a FidelityFX kernel.

- [0001-fix-shader-output-list.patch](patches/0001-fix-shader-output-list.patch), SHA-256 `C3CD383225C5E5FF2C0819A26388EA584B99A6039CAACF72F73424BFB0922250`
- [0002-include-bit-for-popcount.patch](patches/0002-include-bit-for-popcount.patch), SHA-256 `C027744FC54453C31D0802AB5E9C669D302AB529EC883D061C296E5456395597`
- [0003-fix-dof-tile-height.patch](patches/0003-fix-dof-tile-height.patch), SHA-256 `AE28E56C63E34402855382B149A5EC2DF62CDE70EDBD5A102FB44863BFEB1861`

Prepare the sparse pinned checkout from the repository root with:

```powershell
powershell -ExecutionPolicy Bypass -File tools/cmake/prepare_fidelityfx_sdk.ps1
```

Then configure the ordinary Black build. FidelityFX is enabled by default; set `-DBLACK_FIDELITYFX_DOF=OFF` only for a build that intentionally uses the adapter's unavailable-component fallback.

The SDK is distributed under the MIT License. Its full upstream license notice is copied to [LICENSE.txt](LICENSE.txt). Source and tag: <https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4>.
