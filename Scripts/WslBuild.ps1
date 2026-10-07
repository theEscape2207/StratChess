<#
.SYNOPSIS
    GCC Release builds of a ref in WSL Ubuntu-26.04, the strength lab's toolchain. Dot-sourced
    by Measure-CpuProfile.ps1 and Compare-BenchLinux.ps1.

.NOTES
    No param() block: this is a library. Compare-BenchLinux.ps1 -SelfTest covers it; the link is
    its $SelfTestCoverers entry in Validate-PrePR.ps1.

    A ref is exported as a tar (git archive; a dirty worktree through a temporary index) and
    built on WSL's ext4: FetchContent fails over /mnt/c, and a worktree's .git file holds a
    Windows path WSL cannot follow. WSL is driven through a generated .sh with `wsl --exec`,
    never shell text, whose quoting mangles backslashes.
#>

Set-StrictMode -Version Latest

$WslDistro = 'Ubuntu-26.04'

# Builds one ref on ext4 and copies the exe to <bin dir>; the build tree is deleted on exit.
# Written with LF endings, so bash never sees a CR. An empty <cxx flags> configures exactly as
# strength.yml does.
$WslBuildScript = @'
#!/usr/bin/env bash
# Usage: build.sh <source.tar> <bin dir> <work dir> <FetchContent deps dir> [cxx flags]
set -euo pipefail
tar_path=$1; bin_dir=$2; work=$3; deps=$4; flags=${5:-}
rm -rf "$work"
mkdir -p "$work/src" "$bin_dir"
trap 'rm -rf "$work"' EXIT
tar -xf "$tar_path" -C "$work/src"
cmake -S "$work/src" -B "$work/build" -G Ninja --log-level=WARNING -DCMAKE_BUILD_TYPE=Release \
    ${flags:+"-DCMAKE_CXX_FLAGS=$flags"} "-DFETCHCONTENT_BASE_DIR=$deps"
cmake --build "$work/build" --target StratChessEvolved --parallel
cp "$work/build/StratChessEvolved" "$bin_dir/"
grep -E '^CMAKE_(CXX_COMPILER|CXX_FLAGS|BUILD_TYPE):' "$work/build/CMakeCache.txt"
"$(grep -E '^CMAKE_CXX_COMPILER:' "$work/build/CMakeCache.txt" | cut -d= -f2)" --version | head -1
'@ -replace "`r`n", "`n"

function Invoke-Wsl {
    <# A command in $WslDistro. --exec passes arguments verbatim, with no shell to mangle them. #>
    # An empty element is real: build.sh takes an empty flags argument.
    param([Parameter(Mandatory)][AllowEmptyString()][string[]]$Argument)
    & wsl.exe -d $WslDistro --exec @Argument
}

function ConvertTo-WslPath {
    param([Parameter(Mandatory)][string]$Path)
    $wslPath = Invoke-Wsl -Argument 'wslpath', '-a', $Path
    if ($LASTEXITCODE -ne 0 -or -not $wslPath) { throw "wslpath could not translate $Path." }
    return [string]$wslPath
}

function Get-WslDepsDir {
    <# The FetchContent cache on ext4, shared by every WSL build so dependencies download once. #>
    return "$(Invoke-Wsl -Argument 'printenv', 'HOME')/strat-wsl-deps"
}

function Resolve-BuildRef {
    <# A worktree path, built with its uncommitted changes, or a commit of $Repo. #>
    param([Parameter(Mandatory)][string]$Ref, [Parameter(Mandatory)][string]$Arm, [Parameter(Mandatory)][string]$Repo)

    if (Test-Path -LiteralPath $Ref -PathType Container) {
        $root = (Resolve-Path -LiteralPath $Ref).Path
        if (-not (Test-Path -LiteralPath (Join-Path $root 'CMakePresets.json'))) { throw "-$Arm '$Ref' is a directory but not a StratChess checkout." }
        $commit = (& git -C $root rev-parse HEAD)
        if ($LASTEXITCODE -ne 0) { throw "-$Arm '$Ref' is not a git worktree." }
        # Untracked files count: CMake globs sources, so a new .cpp is built.
        $dirty = @(& git -C $root status --porcelain).Count -gt 0
        return [pscustomobject]@{ Arm = $Arm; Ref = $Ref; Tree = $root; Commit = [string]$commit; Dirty = $dirty }
    }
    $commit = (& git -C $Repo rev-parse --verify --quiet "$Ref^{commit}")
    if ($LASTEXITCODE -ne 0 -or -not $commit) { throw "-$Arm '$Ref' is neither a directory nor a commit of $Repo." }
    return [pscustomobject]@{ Arm = $Arm; Ref = $Ref; Tree = $null; Commit = [string]$commit; Dirty = $false }
}

function Export-RefArchive {
    <#
        The ref's source as a tar. A dirty worktree is snapshotted through a temporary index, so
        uncommitted and untracked (not ignored) files are built, as a Windows build would.
    #>
    param([Parameter(Mandatory)][object]$Ref, [Parameter(Mandatory)][string]$Tar, [Parameter(Mandatory)][string]$Repo)

    $source = if ($Ref.Tree) { $Ref.Tree } else { $Repo }
    $treeish = $Ref.Commit
    if ($Ref.Dirty) {
        $callerIndex = $env:GIT_INDEX_FILE
        $env:GIT_INDEX_FILE = "$Tar.index"
        try {
            & git -C $source read-tree HEAD | Out-Host
            & git -C $source add -A | Out-Host
            $treeish = & git -C $source write-tree
            if ($LASTEXITCODE -ne 0) { throw "Could not snapshot $source." }
        }
        finally {
            $env:GIT_INDEX_FILE = $callerIndex
            Remove-Item -LiteralPath "$Tar.index" -ErrorAction SilentlyContinue
        }
    }
    & git -C $source archive --format=tar -o $Tar $treeish | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "git archive failed for $treeish." }
}

function Test-WslBuildFlags {
    <# True when a build log shows CMAKE_CXX_FLAGS configured as exactly $Flags. #>
    param([Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Log, [Parameter(Mandatory)][AllowEmptyString()][string]$Flags)
    return @($Log | Where-Object { $_ -match ('^CMAKE_CXX_FLAGS:\w+=' + [regex]::Escape($Flags) + '$') }).Count -gt 0
}

function Build-WslVariant {
    <#
        Builds $Ref with GCC Release plus $CxxFlags and copies its exe into $WslBinDir (a WSL
        path). $StageDir (Windows) keeps build.sh and build.log; the log's last line is the
        compiler version. Returns the exe's WSL path.
    #>
    param(
        [Parameter(Mandatory)][object]$Ref,
        [Parameter(Mandatory)][string]$Repo,
        [Parameter(Mandatory)][string]$StageDir,
        [Parameter(Mandatory)][string]$WslBinDir,
        [Parameter(Mandatory)][string]$WslWorkDir,
        [Parameter(Mandatory)][string]$WslDepsDir,
        [Parameter(Mandatory)][AllowEmptyString()][string]$CxxFlags
    )

    New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
    $tar = Join-Path $StageDir 'source.tar'
    $buildScript = Join-Path $StageDir 'build.sh'
    $log = Join-Path $StageDir 'build.log'
    [System.IO.File]::WriteAllText($buildScript, $WslBuildScript)
    Export-RefArchive -Ref $Ref -Tar $tar -Repo $Repo
    try {
        Invoke-Wsl -Argument 'bash', (ConvertTo-WslPath $buildScript), (ConvertTo-WslPath $tar), $WslBinDir, $WslWorkDir, $WslDepsDir, $CxxFlags *>> $log
        if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath $log -Tail 20 | Out-Host; throw "WSL build failed; full log: $log" }
    }
    finally { Remove-Item -LiteralPath $tar -ErrorAction SilentlyContinue }

    if (-not (Test-WslBuildFlags -Log @(Get-Content -LiteralPath $log) -Flags $CxxFlags)) { throw "The WSL build was not configured with CMAKE_CXX_FLAGS='$CxxFlags'; see $log" }
    $exe = "$WslBinDir/StratChessEvolved"
    Invoke-Wsl -Argument 'test', '-x', $exe | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "The WSL build wrote no $exe." }
    return $exe
}
