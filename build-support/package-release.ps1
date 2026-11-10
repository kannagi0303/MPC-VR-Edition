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



$root = Split-Path -Parent $PSScriptRoot

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



# Runtime::Open requires precisely this reviewed binary, not just a same-name DLL.

$expected = 'FF6E90EB78B827927DFF5B4ECC6B1C870C2E9BCA29ED9F48C7D348CC9E170B82'

$runtimeCandidates = @(

    (Join-Path $root 'runtime\dlss\nvngx_dlssg.dll'),

    (Join-Path $compiled 'dlss\nvngx_dlssg.dll')

)

$runtime = $null

foreach ($candidate in $runtimeCandidates) {

    if (Test-Path -LiteralPath $candidate -PathType Leaf) {

        if ((Get-Sha256Hex -FilePath $candidate).ToUpperInvariant() -eq $expected) {

            $runtime = $candidate

            Write-Host ('[INFO] Using verified local NVIDIA MFG runtime: ' + $candidate)

            break

        }

        Write-Host ('[WARN] Ignoring a different NVIDIA MFG DLL: ' + $candidate)

    }

}



if ($null -eq $runtime) {

    # Pin to NVIDIA's official 310.9.1 release, matching Source/Mfg/Runtime.cpp.

    # Never obtain a different-version DLL or silently skip the checksum.

    $url = 'https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.1/lib/Windows_x86_64/rel/nvngx_dlssg.dll'

    $cacheFolder = Join-Path $root 'runtime\dlss'

    $null = New-Item -ItemType Directory -Path $cacheFolder -Force

    $cacheFile = Join-Path $cacheFolder 'nvngx_dlssg.dll'

    Write-Host '[INFO] Downloading pinned NVIDIA DLSS-G 310.9.1 runtime from NVIDIA/DLSS...'

    for ($attempt = 1; $attempt -le 3; $attempt++) {

        $temporary = $cacheFile + '.download-' + [Guid]::NewGuid().ToString('N')

        try {

            Invoke-WebRequest -Uri $url -OutFile $temporary -UseBasicParsing -TimeoutSec 120 -ErrorAction Stop

            if ((Get-Sha256Hex -FilePath $temporary).ToUpperInvariant() -ne $expected) {

                throw 'Downloaded runtime SHA-256 mismatch (required by MFG).'

            }

            if (Test-Path -LiteralPath $cacheFile) {

                # A mismatched old cache must not accidentally be reused.

                Remove-Item -LiteralPath $cacheFile -Force

            }

            Move-Item -LiteralPath $temporary -Destination $cacheFile

            $runtime = $cacheFile

            break

        } catch {

            if (Test-Path -LiteralPath $temporary) {

                Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue

            }

            if ($attempt -ge 3) {

                throw ('Could not obtain the exact NVIDIA runtime from the official source. ' +

                    'Build output remains in _bin. Put the matching licensed 310.9.1 DLL in runtime\dlss. ' +

                    'Last error: ' + $_.Exception.Message)

            }

            Write-Host ('[WARN] Runtime download attempt ' + $attempt + ' failed; retrying...')

            Start-Sleep -Seconds (2 * $attempt)

        }

    }

}



if ((Get-Sha256Hex -FilePath $runtime).ToUpperInvariant() -ne $expected) {

    throw 'Runtime verification failed before staging the release.'

}



# A fresh staging directory avoids leaving a partly populated dist/ on failure.

$dist = Join-Path $root 'dist'

$stage = Join-Path $root ('dist.stage-' + [Guid]::NewGuid().ToString('N'))

$old = Join-Path $root ('dist.previous-' + [Guid]::NewGuid().ToString('N'))

$previousMoved = $false

$published = $false

try {

    $dlss = Join-Path $stage 'dlss'

    $null = New-Item -ItemType Directory -Path $dlss -Force

    Copy-Item -LiteralPath $ax -Destination (Join-Path $stage 'MPCVRMFG64.ax') -ErrorAction Stop

    Copy-Item -LiteralPath $shim -Destination (Join-Path $dlss 'mpcvrmfg_nvngx.dll') -ErrorAction Stop

    Copy-Item -LiteralPath $runtime -Destination (Join-Path $dlss 'nvngx_dlssg.dll') -ErrorAction Stop

    Copy-Item -LiteralPath $install -Destination (Join-Path $stage 'Install_MPCVR_64.cmd') -ErrorAction Stop

    Copy-Item -LiteralPath $uninstall64 -Destination (Join-Path $stage 'Uninstall_MPCVR_64.cmd') -ErrorAction Stop

    Copy-Item -LiteralPath $gplLicense -Destination (Join-Path $stage 'LICENSE.md') -ErrorAction Stop

    Copy-Item -LiteralPath $nvidiaLicense -Destination (Join-Path $dlss 'LICENSE-NVIDIA.txt') -ErrorAction Stop

    if ((Get-Sha256Hex -FilePath (Join-Path $dlss 'nvngx_dlssg.dll')).ToUpperInvariant() -ne $expected) {

        throw 'Staged NVIDIA runtime has the wrong SHA-256.'

    }

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

