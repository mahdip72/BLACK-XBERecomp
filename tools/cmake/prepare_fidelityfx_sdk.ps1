param(
    [string]$Destination = (Join-Path $PSScriptRoot '..\..\out\deps\FidelityFX-SDK')
)

$ErrorActionPreference = 'Stop'
$Repository = 'https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git'
$Tag = 'v1.1.4'
$Commit = 'c6efa6bf7f2027b3ec94f28578bb5965eabb9e55'
$PatchDirectory = Join-Path $PSScriptRoot '..\..\third_party\FidelityFX\patches'
$PatchCmake = Join-Path $PatchDirectory '0001-fix-shader-output-list.patch'
$PatchBit = Join-Path $PatchDirectory '0002-include-bit-for-popcount.patch'
$PatchDofTiles = Join-Path $PatchDirectory '0003-fix-dof-tile-height.patch'
$ExpectedPatchCmakeSha256 = 'C3CD383225C5E5FF2C0819A26388EA584B99A6039CAACF72F73424BFB0922250'
$ExpectedPatchBitSha256 = 'C027744FC54453C31D0802AB5E9C669D302AB529EC883D061C296E5456395597'
$ExpectedPatchDofTilesSha256 = 'AE28E56C63E34402855382B149A5EC2DF62CDE70EDBD5A102FB44863BFEB1861'
$Destination = [System.IO.Path]::GetFullPath($Destination)
$NewCheckout = $false
$KnownPatchApplied = $false

function Normalize-Text([string]$Text) {
    return $Text.Replace("`r`n", "`n").Replace("`r", "`n")
}

function Get-PinnedFileText([string]$Path) {
    $GitSpec = "${Commit}:$Path"
    $Blob = (git -C $Destination rev-parse $GitSpec).Trim()
    if ($LASTEXITCODE -ne 0 -or $Blob -notmatch '^[0-9a-f]{40,64}$') {
        throw "Could not resolve pinned SDK file $GitSpec."
    }

    # Read the blob as one stream so PowerShell does not drop or invent a final
    # newline while converting native command output into an array of lines.
    $GitCommand = (Get-Command git -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = $GitCommand
    $StartInfo.Arguments = '-C "' + $Destination + '" cat-file blob ' + $Blob
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $Process = [System.Diagnostics.Process]::new()
    $Process.StartInfo = $StartInfo
    if (-not $Process.Start()) { throw "Could not start Git to read $GitSpec." }
    $PinnedText = $Process.StandardOutput.ReadToEnd()
    $Process.WaitForExit()
    if ($Process.ExitCode -ne 0) { throw "Could not read pinned SDK file $GitSpec." }
    $Process.Dispose()
    return $PinnedText
}

$ActualPatchCmakeSha256 = (Get-FileHash -LiteralPath $PatchCmake -Algorithm SHA256).Hash
$ActualPatchBitSha256 = (Get-FileHash -LiteralPath $PatchBit -Algorithm SHA256).Hash
$ActualPatchDofTilesSha256 = (Get-FileHash -LiteralPath $PatchDofTiles -Algorithm SHA256).Hash
if ($ActualPatchCmakeSha256 -ne $ExpectedPatchCmakeSha256 -or
    $ActualPatchBitSha256 -ne $ExpectedPatchBitSha256 -or
    $ActualPatchDofTilesSha256 -ne $ExpectedPatchDofTilesSha256) {
    throw 'Unexpected FidelityFX SDK build patch hash.'
}

if (-not (Test-Path (Join-Path $Destination '.git'))) {
    if (Test-Path $Destination) {
        throw "Destination exists but is not a Git checkout: $Destination"
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
    git clone --filter=blob:none --depth 1 --branch $Tag --no-checkout $Repository $Destination
    if ($LASTEXITCODE -ne 0) { throw 'FidelityFX SDK clone failed.' }
    $NewCheckout = $true
}

$ActualOrigin = (git -C $Destination remote get-url origin).Trim()
if ($LASTEXITCODE -ne 0 -or $ActualOrigin -ne $Repository) {
    throw "Unexpected FidelityFX remote at $Destination : $ActualOrigin"
}

if (-not $NewCheckout) {
    $Status = @(git -C $Destination status --porcelain=v1)
    if ($LASTEXITCODE -ne 0) { throw "Could not inspect the FidelityFX checkout at $Destination." }
    if ($Status.Count -gt 0) {
        $ChangedFiles = @(git -C $Destination diff --name-only)
        if ($LASTEXITCODE -ne 0) { throw "Could not inspect FidelityFX worktree changes at $Destination." }
        $StagedFiles = @(git -C $Destination diff --cached --name-only)
        if ($LASTEXITCODE -ne 0) { throw "Could not inspect staged FidelityFX changes at $Destination." }
        $UntrackedFiles = @(git -C $Destination ls-files --others --exclude-standard)
        if ($LASTEXITCODE -ne 0) { throw "Could not inspect staged FidelityFX changes at $Destination." }
        $ShaderCmake = Join-Path $Destination 'sdk\include\FidelityFX\gpu\CMakeCompileShaders.txt'
        $ShaderText = [System.IO.File]::ReadAllText($ShaderCmake)
        $UtilHeader = Join-Path $Destination 'sdk\include\FidelityFX\host\ffx_util.h'
        $UtilText = [System.IO.File]::ReadAllText($UtilHeader)
        $ShaderPath = 'sdk/include/FidelityFX/gpu/CMakeCompileShaders.txt'
        $UtilPath = 'sdk/include/FidelityFX/host/ffx_util.h'
        $DofPath = 'sdk/src/components/dof/ffx_dof.cpp'
        $PinnedShader = Get-PinnedFileText $ShaderPath
        $PinnedUtil = Get-PinnedFileText $UtilPath
        $PinnedDof = Get-PinnedFileText $DofPath
        $DofSource = [System.IO.File]::ReadAllText((Join-Path $Destination 'sdk\src\components\dof\ffx_dof.cpp'))
        $OriginalLine = 'set(${PERMUTATION_OUTPUTS} ${PERMUTATION_OUTPUTS} PARENT_SCOPE)'
        $ExpectedLine = 'set(${PERMUTATION_OUTPUTS} ${${PERMUTATION_OUTPUTS}} PARENT_SCOPE)'
        $UtilInclude = '#include <FidelityFX/host/ffx_types.h>'
        if ([regex]::Matches($PinnedShader, [regex]::Escape($OriginalLine)).Count -eq 1 -and
            [regex]::Matches($PinnedUtil, [regex]::Escape($UtilInclude)).Count -eq 1 -and
            -not $PinnedUtil.Contains('#include <bit>') -and
            [regex]::Matches($PinnedDof, [regex]::Escape('uint32_t tileHeight = (halfHeight + 4) / 4;')).Count -eq 1) {
            $ExpectedShader = $PinnedShader.Replace($OriginalLine, $ExpectedLine)
            $ExpectedUtil = $PinnedUtil.Replace($UtilInclude, '#include <bit>' + "`n" + $UtilInclude)
            $ExpectedDof = $PinnedDof.Replace('uint32_t tileHeight = (halfHeight + 4) / 4;', 'uint32_t tileHeight = (halfHeight + 3) / 4;')
        } else {
            throw 'Pinned FidelityFX SDK files do not match the expected patch anchors.'
        }
        if ($ChangedFiles.Count -eq 3 -and
            $ChangedFiles -contains $ShaderPath -and
            $ChangedFiles -contains $UtilPath -and
            $ChangedFiles -contains $DofPath -and
            $StagedFiles.Count -eq 0 -and
            $UntrackedFiles.Count -eq 0 -and
            (Normalize-Text $ShaderText) -ceq $ExpectedShader -and
            (Normalize-Text $UtilText) -ceq $ExpectedUtil -and
            (Normalize-Text $DofSource) -ceq $ExpectedDof) {
            $KnownPatchApplied = $true
        } else {
            throw "Refusing to change a modified FidelityFX checkout at $Destination."
        }
    }
}

$ActualHead = (git -C $Destination rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $ActualHead -ne $Commit) {
    throw "Expected an existing checkout at $Commit, got $ActualHead. Prepare a separate fresh dependency directory."
}

git -C $Destination sparse-checkout init --no-cone
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize FidelityFX sparse checkout.' }
git -C $Destination sparse-checkout set --no-cone @(
    '/LICENSE.txt',
    '/sdk/CMakeLists.txt',
    '/sdk/common.cmake',
    '/sdk/toolchain.cmake',
    '/sdk/include/FidelityFX/host/**',
    '/sdk/include/FidelityFX/gpu/*.h',
    '/sdk/include/FidelityFX/gpu/CMakeCompileShaders.txt',
    '/sdk/include/FidelityFX/gpu/dof/**',
    '/sdk/include/FidelityFX/gpu/spd/**',
    '/sdk/src/shared/**',
    '/sdk/src/components/*/CMakeLists.txt',
    '/sdk/src/components/dof/**',
    '/sdk/src/backends/dx12/CMakeLists.txt',
    '/sdk/src/backends/dx12/CMakeShadersDOF.txt',
    '/sdk/src/backends/dx12/*.h',
    '/sdk/src/backends/dx12/*.cpp',
    '/sdk/src/backends/dx12/shaders/dof/**',
    '/sdk/src/backends/shared/**',
    '/sdk/libs/pix/**',
    '/sdk/tools/binary_store/FidelityFX_SC.exe',
    '/sdk/tools/binary_store/dxcompiler.dll',
    '/sdk/tools/binary_store/dxil.dll'
)
if ($LASTEXITCODE -ne 0) { throw 'Could not configure the FidelityFX sparse paths.' }

git -C $Destination checkout --detach $Commit
if ($LASTEXITCODE -ne 0) { throw "Could not check out FidelityFX $Commit." }

$ActualCommit = (git -C $Destination rev-parse HEAD).Trim()
if ($ActualCommit -ne $Commit) { throw "Expected $Commit, got $ActualCommit." }
if (-not (Test-Path (Join-Path $Destination 'sdk\src\backends\dx12\CMakeLists.txt'))) {
    throw 'The sparse SDK checkout is missing the DX12 backend.'
}
if (-not (Test-Path (Join-Path $Destination 'sdk\src\backends\dx12\shaders\dof\ffx_dof_blur_pass.hlsl'))) {
    throw 'The sparse SDK checkout is missing DoF shaders.'
}

if (-not $KnownPatchApplied) {
    $ShaderCmake = Join-Path $Destination 'sdk\include\FidelityFX\gpu\CMakeCompileShaders.txt'
    $ShaderText = [System.IO.File]::ReadAllText($ShaderCmake)
    $OriginalLine = 'set(${PERMUTATION_OUTPUTS} ${PERMUTATION_OUTPUTS} PARENT_SCOPE)'
    $ExpectedLine = 'set(${PERMUTATION_OUTPUTS} ${${PERMUTATION_OUTPUTS}} PARENT_SCOPE)'
    if (-not $ShaderText.Contains($OriginalLine)) { throw 'Unexpected upstream shader output CMake function.' }
    [System.IO.File]::WriteAllText($ShaderCmake, $ShaderText.Replace($OriginalLine, $ExpectedLine),
        [System.Text.UTF8Encoding]::new($false))

    $UtilHeader = Join-Path $Destination 'sdk\include\FidelityFX\host\ffx_util.h'
    $UtilText = [System.IO.File]::ReadAllText($UtilHeader)
    $UtilInclude = '#include <FidelityFX/host/ffx_types.h>'
    if (-not $UtilText.Contains($UtilInclude)) { throw 'Unexpected FidelityFX utility header.' }
    [System.IO.File]::WriteAllText($UtilHeader,
        $UtilText.Replace($UtilInclude, '#include <bit>' + "`r`n" + $UtilInclude),
        [System.Text.UTF8Encoding]::new($false))

    $DofSourcePath = Join-Path $Destination 'sdk\src\components\dof\ffx_dof.cpp'
    $DofSource = [System.IO.File]::ReadAllText($DofSourcePath)
    $OriginalTileHeight = 'uint32_t tileHeight = (halfHeight + 4) / 4;'
    $ExpectedTileHeight = 'uint32_t tileHeight = (halfHeight + 3) / 4;'
    if ([regex]::Matches($DofSource, [regex]::Escape($OriginalTileHeight)).Count -ne 1) {
        throw 'Unexpected FidelityFX DoF tile-height expression.'
    }
    [System.IO.File]::WriteAllText($DofSourcePath, $DofSource.Replace($OriginalTileHeight, $ExpectedTileHeight),
        [System.Text.UTF8Encoding]::new($false))
}

Write-Output "FidelityFX SDK $Tag prepared at $Destination ($ActualCommit)."
