# SPDX-License-Identifier: GPL-3.0-or-later
# A successful converter receipt is tied to the pack bytes, not future tool builds.
# Use the same .NET approach as the existing DirectML build scripts.
function Get-DepthSha256Hex([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [IO.File]::OpenRead($Path)
        return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '')
    } finally {
        if ($null -ne $stream) { $stream.Dispose() }
        $sha.Dispose()
    }
}
function Test-ValidatedDepthPack([string]$Pack) {
    $receiptPath = $Pack + '.json'
    if (!(Test-Path -LiteralPath $Pack -PathType Leaf) -or
        !(Test-Path -LiteralPath $receiptPath -PathType Leaf)) { return $false }
    try {
        $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json -ErrorAction Stop
        if ($receipt.pack -notmatch '^[0-9a-fA-F]{64}$') { return $false }
        return $receipt.pack -eq (Get-DepthSha256Hex $Pack)
    } catch { return $false }
}

$depthStem = 'dp2-s-w4-352x192-fp16'
$depthGraphName = "$depthStem.dmlgraph"
$depthWeightName = 'depthpro2-s-fp16.dmlweights'

# Use the public revision already used to validate the embedded C++ recipe.
function Get-PinnedDepthCheckpoint([string]$WorkDirectory) {
    $ProgressPreference = 'SilentlyContinue'
    $revision = '903a1c1ade918831d138adcc4aedbcc8f4da0078'
    $resources = [ordered]@{
        'config.json' = '42E0A8202B5260A1ACBF4E8924745EDF96FEB0EBFF75455C1E291E10505E8C04'
        'model.safetensors' = '583C4D630779FD231F969C738733624963AD34BD3C29F9C64088257E3436F97C'
    }
    $hubCache = if ($env:HF_HUB_CACHE) { $env:HF_HUB_CACHE }
        elseif ($env:HF_HOME) { Join-Path $env:HF_HOME 'hub' }
        else { Join-Path $env:USERPROFILE '.cache\huggingface\hub' }
    $snapshot = Join-Path $hubCache "models--apple--DepthPro2-S\snapshots\$revision"
    $uris = "https://huggingface.co/apple/DepthPro2-S/resolve/$revision"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $result = @{}
    foreach ($file in $resources.Keys) {
        $destination = Join-Path $WorkDirectory ("DepthPro2-S-" + $file)
        $cached = Join-Path $snapshot $file
        $expected = $resources[$file]
        if ((Test-Path -LiteralPath $destination -PathType Leaf) -and (Get-DepthSha256Hex $destination) -eq $expected) {
            $result[$file] = $destination
            continue
        }
        if ((Test-Path -LiteralPath $cached -PathType Leaf) -and (Get-DepthSha256Hex $cached) -eq $expected) {
            $result[$file] = $cached
            continue
        }
        $temp = $destination + '.partial-' + [guid]::NewGuid().ToString('N')
        try {
            Write-Host "[INFO] Downloading pinned Depth Pro 2-S $file."
            Invoke-WebRequest -Uri "$uris/$file" -UseBasicParsing -TimeoutSec 300 -OutFile $temp
            if ((Get-DepthSha256Hex $temp) -ne $expected) { throw "Depth checkpoint SHA-256 mismatch: $file" }
            if (Test-Path -LiteralPath $destination -PathType Leaf) { [IO.File]::Replace($temp,$destination,[NullString]::Value) }
            else { [IO.File]::Move($temp,$destination) }
        } finally {
            if (Test-Path -LiteralPath $temp -PathType Leaf) { Remove-Item -LiteralPath $temp -Force }
        }
        $result[$file] = $destination
    }
    return $result
}

function Test-ValidatedDepthPair([string]$Folder) {
    try {
        $receipt = Get-Content -LiteralPath (Join-Path $Folder 'split-352x192.json') -Raw -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop
        return $receipt.format -eq 1 -and
            $receipt.graph -eq (Get-DepthSha256Hex (Join-Path $Folder $depthGraphName)) -and
            $receipt.weights -eq (Get-DepthSha256Hex (Join-Path $Folder $depthWeightName))
    } catch { return $false }
}
