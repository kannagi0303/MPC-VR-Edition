$ErrorActionPreference = 'Stop'

$ProgressPreference = 'SilentlyContinue'

# Windows PowerShell 5.1 and recent PowerShell versions.

[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12



function Get-Sha256Hex {

    param([Parameter(Mandatory = $true)][string]$FilePath)

    $sha = [System.Security.Cryptography.SHA256]::Create()

    $stream = $null

    try {

        $stream = [System.IO.File]::OpenRead($FilePath)

        return [System.BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '')

    } finally {

        if ($null -ne $stream) { $stream.Dispose() }

        $sha.Dispose()

    }

}



$root = (Get-Item -LiteralPath (Split-Path -Parent $PSScriptRoot)).FullName

$compiled = Join-Path $root '_bin\Filter_x64'

$ax = Join-Path $compiled 'MPCVRMFG64.ax'

$shim = Join-Path $compiled 'dlss\mpcvrmfg_nvngx.dll'

$install = Join-Path $PSScriptRoot 'Install_MPCVR_64.cmd'

$uninstall64 = Join-Path $PSScriptRoot 'Uninstall_MPCVR_64.cmd'

$gplLicense = Join-Path $root '.github\LICENSE.md'

$nvidiaLicense = Join-Path $root 'vendor\nvidia\LICENSE.txt'



foreach ($required in @($ax,$shim,$install,$uninstall64,$gplLicense,$nvidiaLicense)) {

    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {

        throw ('Cannot package: required file not found: ' + $required)

    }

}



function Get-NvidiaRuntime {
    param(
        [string]$FileName,
        [string]$Version,
        [string]$Revision,
        [string]$ExpectedHash
    )

    $cacheFolder = Join-Path $root 'runtime\dlss'
    $cacheFile = Join-Path $cacheFolder $FileName
    foreach ($candidate in @($cacheFile, (Join-Path $compiled ('dlss\' + $FileName)))) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            if ((Get-Sha256Hex -FilePath $candidate) -eq $ExpectedHash) {
                Write-Host ('[INFO] Using verified local NVIDIA runtime: ' + $candidate)
                return $candidate
            }
            Write-Warning ('Ignoring a different NVIDIA runtime: ' + $candidate)
        }
    }

    $url = "https://raw.githubusercontent.com/NVIDIA/DLSS/$Revision/lib/Windows_x86_64/rel/$FileName"
    $null = New-Item -ItemType Directory -Path $cacheFolder -Force
    Write-Host "[INFO] Downloading NVIDIA $FileName $Version..."
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        $temporary = $cacheFile + '.download-' + [Guid]::NewGuid().ToString('N')
        try {
            Invoke-WebRequest -Uri $url -OutFile $temporary -UseBasicParsing -TimeoutSec 120
            if ((Get-Sha256Hex -FilePath $temporary) -ne $ExpectedHash) {
                throw "SHA-256 mismatch for $FileName."
            }
            Move-Item -LiteralPath $temporary -Destination $cacheFile -Force
            return $cacheFile
        } catch {
            if ($attempt -ge 3) {
                throw "Could not obtain NVIDIA $FileName $Version. Build output remains in _bin. Put the matching DLL in runtime\dlss. Last error: $($_.Exception.Message)"
            }
            Write-Warning "Download attempt $attempt failed for $FileName; retrying..."
            Start-Sleep -Seconds (2 * $attempt)
        } finally {
            if (Test-Path -LiteralPath $temporary) {
                Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
            }
        }
    }
}

# MFG requires precisely the binary reviewed by Source/Mfg/Runtime.cpp.
$runtimes = @(
    @{ FileName = 'nvngx_dlssg.dll'; Version = '310.9.1'; Revision = 'v310.9.1'
       ExpectedHash = 'FF6E90EB78B827927DFF5B4ECC6B1C870C2E9BCA29ED9F48C7D348CC9E170B82' },
    @{ FileName = 'nvngx_dlss.dll'; Version = '310.9.16'; Revision = '9981c6350dc58c0ce21f024fd65e31e851c31689'
       ExpectedHash = 'D00B5B0AB29C3CA101B70C3A5ECB3A01B7EF33766A3383A28A18623083E6123E' }
)
foreach ($runtime in $runtimes) {
    $runtimeSource = Get-NvidiaRuntime @runtime
    $runtime.Source = $runtimeSource
}

# A fresh staging directory avoids leaving a partly populated dist/ on failure.

$dist = Join-Path $root 'dist'

$stage = Join-Path $root ('dist.stage-' + [Guid]::NewGuid().ToString('N'))

$old = Join-Path $root ('dist.previous-' + [Guid]::NewGuid().ToString('N'))

foreach ($path in @($dist, $stage, $old)) {
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($path)) -ne $root) {
        throw ('Distribution path is outside the repository: ' + $path)
    }
    if ((Test-Path -LiteralPath $path) -and
        ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw ('Distribution path is a junction or symlink: ' + $path)
    }
}
$previousMoved = $false

$published = $false

try {

    $dlss = Join-Path $stage 'dlss'

    $null = New-Item -ItemType Directory -Path $dlss -Force

    Copy-Item -LiteralPath $ax -Destination (Join-Path $stage 'MPCVRMFG64.ax') -ErrorAction Stop

    Copy-Item -LiteralPath $shim -Destination (Join-Path $dlss 'mpcvrmfg_nvngx.dll') -ErrorAction Stop

    foreach ($runtime in $runtimes) {
        $target = Join-Path $dlss $runtime.FileName
        Copy-Item -LiteralPath $runtime.Source -Destination $target
        if ((Get-Sha256Hex -FilePath $target) -ne $runtime.ExpectedHash) {
            throw ('Staged NVIDIA runtime has the wrong SHA-256: ' + $runtime.FileName)
        }
    }

    Copy-Item -LiteralPath $install -Destination (Join-Path $stage 'Install_MPCVR_64.cmd') -ErrorAction Stop

    Copy-Item -LiteralPath $uninstall64 -Destination (Join-Path $stage 'Uninstall_MPCVR_64.cmd') -ErrorAction Stop

    Copy-Item -LiteralPath $gplLicense -Destination (Join-Path $stage 'LICENSE.md') -ErrorAction Stop

    Copy-Item -LiteralPath $nvidiaLicense -Destination (Join-Path $dlss 'LICENSE-NVIDIA.txt') -ErrorAction Stop

    if (Test-Path -LiteralPath $dist) {

        Move-Item -LiteralPath $dist -Destination $old -ErrorAction Stop

        $previousMoved = $true

    }

    Move-Item -LiteralPath $stage -Destination $dist -ErrorAction Stop

    $published = $true

    Write-Host ('[OK] Clean distribution ready: ' + $dist)

    Write-Host '[INFO] Contents: MPCVRMFG64.ax, x64 install/uninstall, dlss DLLs and licenses.'

    # One-time cleanup of an obsolete build-support template from earlier ZIPs.

    # Never touch the actual x64 installer or uninstaller.

    $obsoleteX86Stub = Join-Path $PSScriptRoot 'Uninstall_MPCVR_32.cmd'

    if (Test-Path -LiteralPath $obsoleteX86Stub -PathType Leaf) {

        try {

            Remove-Item -LiteralPath $obsoleteX86Stub -Force -ErrorAction Stop

            Write-Host '[INFO] Removed obsolete x86 uninstall stub from build-support.'

        } catch {

            Write-Warning ('Obsolete x86 stub could not be removed: ' + $_.Exception.Message)

        }

    }

} finally {

    if (-not $published -and $previousMoved -and -not (Test-Path -LiteralPath $dist)) {

        Move-Item -LiteralPath $old -Destination $dist -ErrorAction SilentlyContinue

    }

    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue }

    if ($published -and $previousMoved -and (Test-Path -LiteralPath $old)) {

        Remove-Item -LiteralPath $old -Recurse -Force -ErrorAction SilentlyContinue

    }

}

