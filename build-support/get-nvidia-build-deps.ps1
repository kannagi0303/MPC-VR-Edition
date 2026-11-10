$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# Windows PowerShell 5.1 can still default to outdated TLS versions.
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

# Use the built-in .NET SHA-256 implementation instead of Get-FileHash.
# Some restricted/custom Windows PowerShell environments do not expose that cmdlet.
# The SHA-256 check remains mandatory for both cached and downloaded SDK files.
function Get-Sha256Hex {
    param([Parameter(Mandatory = $true)][string]$FilePath)
    $stream = $null
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $stream = [System.IO.File]::Open(
            $FilePath,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read,
            [System.IO.FileShare]::Read
        )
        $digest = $sha256.ComputeHash($stream)
        return [System.BitConverter]::ToString($digest).Replace('-', '')
    }
    finally {
        if ($null -ne $stream) { $stream.Dispose() }
        $sha256.Dispose()
    }
}

$root = Split-Path $PSScriptRoot -Parent
$manifestPath = Join-Path $PSScriptRoot 'nvidia-build-dependencies.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$vendorRoot = [IO.Path]::GetFullPath((Join-Path $root 'vendor/nvidia')).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
$trustedPrefix = 'https://raw.githubusercontent.com/NVIDIA/DLSS/' + $manifest.commit + '/'

Write-Host '[INFO] NVIDIA build-only SDK: checking pinned files and SHA-256 hashes.'
Write-Host ('[INFO] NVIDIA SDK license: ' + $manifest.licenseUrl)

foreach ($item in $manifest.files) {
    $target = [IO.Path]::GetFullPath((Join-Path $root $item.destination))
    if (-not $target.StartsWith($vendorRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw ('Invalid NVIDIA destination path: ' + $item.destination)
    }
    if (-not ([string]$item.url).StartsWith($trustedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw ('Unexpected NVIDIA source URL: ' + $item.url)
    }
    if ([string]$item.sha256 -notmatch '^[0-9a-fA-F]{64}$') {
        throw ('Invalid dependency SHA-256 in manifest: ' + $item.destination)
    }
    $expectedHash = ([string]$item.sha256).ToUpperInvariant()
    if (Test-Path -LiteralPath $target -PathType Leaf) {
        $presentHash = (Get-Sha256Hex -FilePath $target).ToUpperInvariant()
        if ($presentHash -eq $expectedHash) { continue }
        Write-Host ('[WARN] Incorrect checksum; fetching the pinned version: ' + $item.destination)
    } else {
        Write-Host ('[INFO] Downloading: ' + $item.destination)
    }

    $null = New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force
    $succeeded = $false
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        $temporary = $target + '.download-' + [Guid]::NewGuid().ToString('N')
        try {
            Invoke-WebRequest -Uri $item.url -OutFile $temporary -UseBasicParsing -TimeoutSec 120 -ErrorAction Stop
            $actualHash = (Get-Sha256Hex -FilePath $temporary).ToUpperInvariant()
            if ($actualHash -ne $expectedHash) {
                throw ('SHA-256 mismatch for ' + $item.destination)
            }
            if (Test-Path -LiteralPath $target) {
                $backup = $target + '.unverified-' + [Guid]::NewGuid().ToString('N') + '.bak'
                Move-Item -LiteralPath $target -Destination $backup -ErrorAction Stop
                Write-Host ('[WARN] Preserved mismatched original at: ' + $backup)
            }
            Move-Item -LiteralPath $temporary -Destination $target -ErrorAction Stop
            $succeeded = $true
            break
        } catch {
            if (Test-Path -LiteralPath $temporary) {
                Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
            }
            if ($attempt -eq 3) {
                throw ('Could not retrieve verified NVIDIA dependency ' + $item.destination + ': ' + $_.Exception.Message)
            }
            Write-Host ('[WARN] Download attempt ' + $attempt + ' failed; retrying...')
            Start-Sleep -Seconds (2 * $attempt)
        }
    }
    if (-not $succeeded) { throw ('Unable to obtain ' + $item.destination) }
}
Write-Host ('[OK] NVIDIA build dependencies verified (commit ' + $manifest.commit + ').')
