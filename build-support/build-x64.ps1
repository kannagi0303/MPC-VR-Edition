# SPDX-License-Identifier: GPL-3.0-or-later
$ErrorActionPreference = 'Stop'
$support = $PSScriptRoot
$root = Split-Path -Parent $support
$bin = Join-Path $root '_bin'
$logs = Join-Path $bin 'logs'
$null = New-Item -ItemType Directory -Path $logs -Force
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw 'Visual Studio Installer (vswhere) was not found.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or !$vs) { throw 'Visual Studio with the MSVC x64 tools was not found.' }
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'
if (!(Test-Path -LiteralPath $msbuild -PathType Leaf)) {
    $msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
}
if (!(Test-Path -LiteralPath $msbuild -PathType Leaf)) { throw 'MSBuild was not found.' }
$solution = Join-Path $root 'MpcVideoRenderer.sln'
$converter = Join-Path $bin 'depth-converter\depth-convert.exe'
$prepareDirectML = Join-Path $support 'prepare-directml.ps1'

function Invoke-BuildTarget([string]$Target, [string]$LogName) {
    $log = Join-Path $logs $LogName
    & $msbuild $solution "/t:$Target" /m:2 /nr:false /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal /fl "/flp:logfile=$log;verbosity=normal;encoding=UTF-8"
    if ($LASTEXITCODE -ne 0) { throw "MSBuild target $Target failed. See $log." }
}

try {
    $lock = [IO.File]::Open((Join-Path $bin 'build-x64.lock'), 'OpenOrCreate', 'Write', 'None')
} catch [IO.IOException] {
    throw "Cannot lock the x64 build output in $bin. Check whether another build is using it. $($_.Exception.Message)"
}
$stage = Join-Path $bin ('depth-input-' + [Guid]::NewGuid().ToString('N'))
try {
    $null = New-Item -ItemType Directory -Path $stage
    $step = [Diagnostics.Stopwatch]::StartNew()
    & (Join-Path $support 'get-nvidia-build-deps.ps1')
    & $prepareDirectML
    Write-Host ("[INFO] Build dependencies: {0:F1} s" -f $step.Elapsed.TotalSeconds)

    $step.Restart()
    $ensureConverter = {
        & $prepareDirectML -WithConverter
        Invoke-BuildTarget 'DepthConvert' 'depth-converter.log'
    }
    & (Join-Path $support 'depth\stage-depth-model.ps1') -Converter $converter -EnsureConverter $ensureConverter -Edition $stage -PreviousEdition (Join-Path $root 'dist\edition')
    Write-Host ("[INFO] Depth resources: {0:F1} s" -f $step.Elapsed.TotalSeconds)

    $step.Restart()
    # Incremental ZIP overlays can preserve older file timestamps. Rebuild the
    # renderer to prevent mixing translation units with different class layouts.
    Write-Host '[INFO] Renderer: full rebuild (Clean + Build).'
    Invoke-BuildTarget 'MpcVideoRenderer:Rebuild' 'x64-release.log'
    Write-Host ("[INFO] Renderer build: {0:F1} s" -f $step.Elapsed.TotalSeconds)

    $step.Restart()
    & (Join-Path $support 'package-release.ps1') -PreparedDepth $stage
    Write-Host ("[OK] Local x64 dist ready ({0:F1} s packaging)." -f $step.Elapsed.TotalSeconds)
} finally {
    try {
        # This unique stage was created here and accepts only the two model files.
        if (Test-Path -LiteralPath $stage -PathType Container) {
            Get-ChildItem -LiteralPath $stage -File | Remove-Item -Force
            [IO.Directory]::Delete($stage)
        }
    } finally { $lock.Dispose() }
}
