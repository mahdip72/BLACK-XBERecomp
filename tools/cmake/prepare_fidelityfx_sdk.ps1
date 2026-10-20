param(
    [string]$Destination = (Join-Path $PSScriptRoot '..\..\out\deps\FidelityFX-SDK')
)

$ErrorActionPreference = 'Stop'
$Repository = 'https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git'
$Tag = 'v1.1.4'
$Commit = 'c6efa6bf7f2027b3ec94f28578bb5965eabb9e55'
$PatchDirectory = Join-Path $PSScriptRoot '..\..\third_party\FidelityFX\patches'
$Patches = @(
    @{ Name = '0001-fix-shader-output-list.patch'; Sha256 = 'C3CD383225C5E5FF2C0819A26388EA584B99A6039CAACF72F73424BFB0922250' },
    @{ Name = '0002-include-bit-for-popcount.patch'; Sha256 = 'C027744FC54453C31D0802AB5E9C669D302AB529EC883D061C296E5456395597' },
    @{ Name = '0003-fix-dof-tile-height.patch'; Sha256 = 'AE28E56C63E34402855382B149A5EC2DF62CDE70EDBD5A102FB44863BFEB1861' },
    @{ Name = '0004-dof-virtual-input.patch'; Sha256 = '65979927648B8949D5705F0A22794B2ACF280052CB02CE537DB8FD7C62B67CA8' },
    @{ Name = '0005-dof-fused-output.patch'; Sha256 = 'C60CE19CE963D12076D6BACEE38673DE75EC1CC210A90238992B753433DB61B9' },
    @{ Name = '0006-dof-single-global-reset.patch'; Sha256 = '7877C9FB6EC53C4AF83A88B66C31DD49EA76CA27A4626BFE18DF3B2FCF215463' },
    @{ Name = '0009-dof-exact-reuse-candidate.patch'; Sha256 = '25AD52424DA9A223B6850C27EBC7888EE1E6EF16E254FBBA0B66D0091231C1DA' },
    @{ Name = '0012-dof-exact-world-cache.patch'; Sha256 = 'FE81D9A7BD265A51AB543ED20523F13E0F6268E21161CEA7FCA52384F3C263BF' },
    @{ Name = '0014-dof-exact-world-multi-cache.patch'; Sha256 = '6B20BAC15A002E789019171461CE8785E0A735D486ECE915925428FEDF8FF727' }
)
$Destination = [System.IO.Path]::GetFullPath($Destination)
$NewCheckout = $false

function Normalize-Text([string]$Text) {
    return $Text.Replace("`r`n", "`n").Replace("`r", "`n")
}

function Read-PinnedText([string]$GitDirectory, [string]$Tree, [string]$Path) {
    $Blob = (git --git-dir=$GitDirectory rev-parse "${Tree}:$Path").Trim()
    if ($LASTEXITCODE -ne 0 -or $Blob -notmatch '^[0-9a-f]{40,64}$') {
        throw "Could not resolve pinned SDK file ${Tree}:$Path."
    }
    # Preserve the final newline when reading native output in PowerShell.
    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = (Get-Command git -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    $StartInfo.Arguments = '--git-dir="' + $GitDirectory + '" cat-file blob ' + $Blob
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $Process = [System.Diagnostics.Process]::new()
    $Process.StartInfo = $StartInfo
    try {
        if (-not $Process.Start()) { throw 'Could not start Git to read a pinned SDK file.' }
        $Text = $Process.StandardOutput.ReadToEnd()
        $Process.WaitForExit()
        if ($Process.ExitCode -ne 0) { throw "Could not read pinned SDK file $Path." }
        return $Text
    } finally {
        $Process.Dispose()
    }
}

foreach ($Patch in $Patches) {
    $Patch.Path = [System.IO.Path]::GetFullPath((Join-Path $PatchDirectory $Patch.Name))
    if ((Get-FileHash -LiteralPath $Patch.Path -Algorithm SHA256).Hash -ne $Patch.Sha256) {
        throw "Unexpected FidelityFX SDK patch hash: $($Patch.Name)."
    }
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
$ActualHead = (git -C $Destination rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $ActualHead -ne $Commit) {
    throw "Expected an existing checkout at $Commit, got $ActualHead. Prepare a separate fresh dependency directory."
}

if (-not $NewCheckout) {
    $StagedFiles = @(git -C $Destination diff --cached --name-only)
    if ($LASTEXITCODE -ne 0) { throw 'Could not inspect staged FidelityFX changes.' }
    $UntrackedFiles = @(git -C $Destination ls-files --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) { throw 'Could not inspect untracked FidelityFX files.' }
    if ($StagedFiles.Count -gt 0) {
        throw "Refusing to change a FidelityFX checkout with staged files at $Destination."
    }
}

# Build expected states in an isolated object store and index. The dependency's
# objects are read-only alternates; synthesis never writes its index or files.
$ScratchRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$Scratch = Join-Path $ScratchRoot ('black-fidelityfx-sdk-' + [guid]::NewGuid().ToString('N'))
try {
    git init --bare --quiet $Scratch
    if ($LASTEXITCODE -ne 0) { throw 'Could not initialize temporary SDK patch index.' }
    # A new filtered clone may lack blobs. Fetch them into scratch, never into
    # the existing dependency while deciding whether its changes are trusted.
    git --git-dir=$Scratch config remote.origin.url $Repository
    if ($LASTEXITCODE -ne 0) { throw 'Could not configure temporary SDK origin.' }
    git --git-dir=$Scratch config remote.origin.promisor true
    if ($LASTEXITCODE -ne 0) { throw 'Could not configure temporary SDK blob fetching.' }
    git --git-dir=$Scratch config remote.origin.partialclonefilter blob:none
    if ($LASTEXITCODE -ne 0) { throw 'Could not configure temporary SDK blob filter.' }
    $Objects = (git -C $Destination rev-parse --path-format=absolute --git-path objects).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Could not locate pinned SDK objects.' }
    [System.IO.File]::WriteAllText((Join-Path $Scratch 'objects/info/alternates'),
        $Objects.Replace('\', '/') + "`n", [System.Text.UTF8Encoding]::new($false))
    git --git-dir=$Scratch read-tree $Commit
    if ($LASTEXITCODE -ne 0) { throw 'Could not read pinned SDK tree.' }
    $KnownTrees = @($Commit)
    for ($Index = 0; $Index -lt $Patches.Count; ++$Index) {
        git --git-dir=$Scratch -c core.autocrlf=false apply --cached --unidiff-zero --whitespace=nowarn $Patches[$Index].Path
        if ($LASTEXITCODE -ne 0) { throw "Could not synthesize SDK patch $($Patches[$Index].Name)." }
        if ($Index -eq 2 -or $Index -eq 5 -or $Index -eq 6 -or $Index -eq 7 -or $Index -eq 8) {
            $Tree = (git --git-dir=$Scratch write-tree --missing-ok).Trim()
            if ($LASTEXITCODE -ne 0) { throw 'Could not write expected SDK tree.' }
            $KnownTrees += $Tree
        }
    }
    $FinalTree = $KnownTrees[-1]
    $PatchPaths = @(git --git-dir=$Scratch diff-tree --no-commit-id --name-only -r $Commit $FinalTree)
    if ($LASTEXITCODE -ne 0) { throw 'Could not enumerate patched SDK files.' }
    $FinalText = @{}
    foreach ($Path in $PatchPaths) {
        $FinalText[$Path] = Read-PinnedText $Scratch $FinalTree $Path
    }

    if (-not $NewCheckout) {
        $ChangedFiles = @(git -C $Destination diff --name-only --no-renames)
        if ($LASTEXITCODE -ne 0) { throw 'Could not inspect FidelityFX worktree changes.' }
        # New patch-owned files remain untracked in the SDK checkout. Accept
        # only an exact known state's paths and full contents, including them.
        $ChangedFiles = @(@($ChangedFiles) + @($UntrackedFiles) | Sort-Object -Unique -CaseSensitive)
        $KnownState = $false
        foreach ($Tree in $KnownTrees) {
            $ExpectedPaths = @(git --git-dir=$Scratch diff-tree --no-commit-id --name-only -r $Commit $Tree)
            if ($LASTEXITCODE -ne 0) { throw 'Could not inspect an expected SDK tree.' }
            $ExpectedPaths = @($ExpectedPaths | Sort-Object -Unique -CaseSensitive)
            if (($ChangedFiles -join "`n") -cne ($ExpectedPaths -join "`n")) { continue }
            $TreePaths = @(git --git-dir=$Scratch ls-tree -r --name-only $Tree -- $PatchPaths)
            if ($LASTEXITCODE -ne 0) { throw 'Could not inspect expected SDK file presence.' }
            $Matches = $true
            foreach ($Path in $PatchPaths) {
                $File = Join-Path $Destination $Path
                if ($TreePaths -cnotcontains $Path) {
                    if (Test-Path -LiteralPath $File) { $Matches = $false; break }
                    continue
                }
                if (-not (Test-Path -LiteralPath $File -PathType Leaf) -or
                    (Normalize-Text ([System.IO.File]::ReadAllText($File))) -cne
                    (Read-PinnedText $Scratch $Tree $Path)) {
                    $Matches = $false
                    break
                }
            }
            if ($Matches) {
                $KnownState = $true
                break
            }
        }
        if (-not $KnownState) {
            throw "Refusing to change a modified FidelityFX checkout at $Destination."
        }
    }
} finally {
    # Delete only the uniquely named directory created above, within OS temp.
    $ResolvedScratch = [System.IO.Path]::GetFullPath($Scratch)
    if (-not $ResolvedScratch.StartsWith($ScratchRoot, [System.StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $ResolvedScratch) -notmatch '^black-fidelityfx-sdk-[0-9a-f]{32}$') {
        throw 'Refusing to remove an unexpected SDK scratch path.'
    }
    if (Test-Path -LiteralPath $ResolvedScratch) {
        Remove-Item -LiteralPath $ResolvedScratch -Recurse -Force
    }
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

# All existing changes were verified before checkout or sparse configuration.
foreach ($Path in $PatchPaths) {
    [System.IO.File]::WriteAllText((Join-Path $Destination $Path), $FinalText[$Path],
        [System.Text.UTF8Encoding]::new($false))
}

Write-Output "FidelityFX SDK $Tag prepared at $Destination ($ActualCommit; patches 0001-0006, 0009, 0012, 0014)."
