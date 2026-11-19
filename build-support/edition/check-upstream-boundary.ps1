[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ReferenceRoot,
    [string]$ReportPath,
    [ValidateSet('All', 'Source', 'Integration')][string]$Scope = 'All'
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$referenceDirectory = (Resolve-Path -LiteralPath $ReferenceRoot).Path
$baseline = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'upstream-baseline.json') -Raw | ConvertFrom-Json
$nativeFiles = @()
$approvedFiles = @()
$manifests = if ($Scope -eq 'Source') { @('') } elseif ($Scope -eq 'Integration') { @('integration-') } else { @('', 'integration-') }
foreach ($prefix in $manifests) {
    $fileName = if ($prefix) { 'upstream-integration-files.json' } else { 'upstream-files.json' }
    $contractName = if ($prefix) { 'upstream-integration-boundary.json' } else { 'upstream-boundary.json' }
    $files = Get-Content -LiteralPath (Join-Path $PSScriptRoot $fileName) -Raw | ConvertFrom-Json
    $contract = Get-Content -LiteralPath (Join-Path $PSScriptRoot $contractName) -Raw | ConvertFrom-Json
    if ($files.commit -ne $baseline.commit -or $contract.commit -ne $baseline.commit) {
        throw 'The upstream reference and boundary manifests refer to different commits.'
    }
    $nativeFiles += @($files.files)
    $approvedFiles += @($contract.files)
}
if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) { throw 'Git is required for the boundary audit.' }

function Get-GitBlobHash([byte[]]$Data) {
    $prefix = [Text.Encoding]::UTF8.GetBytes(('blob ' + $Data.Length + [char]0))
    $hash = [Security.Cryptography.SHA1]::Create()
    try {
        $null = $hash.TransformBlock($prefix, 0, $prefix.Length, $prefix, 0)
        $null = $hash.TransformFinalBlock($Data, 0, $Data.Length)
        return [BitConverter]::ToString($hash.Hash).Replace('-', '').ToLowerInvariant()
    } finally { $hash.Dispose() }
}

function Test-ReferenceHash([byte[]]$Data, [string]$Expected) {
    if ((Get-GitBlobHash $Data) -eq $Expected) { return $true }
    # Git working trees may use CRLF and may normalize a UTF-8 BOM. Only accept
    # a representation that reproduces the pinned upstream Git blob exactly.
    $text = [Text.Encoding]::UTF8.GetString($Data).Replace("`r`n", "`n").TrimStart([char]0xFEFF)
    foreach ($lineEnding in @($text, $text.Replace("`n", "`r`n"))) {
        foreach ($variant in @($lineEnding, ([string][char]0xFEFF + $lineEnding))) {
            if ((Get-GitBlobHash ([Text.Encoding]::UTF8.GetBytes($variant))) -eq $Expected) { return $true }
        }
    }
    return $false
}

$runRoot = Join-Path $projectRoot ('_bin\upstream-audit\run-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $runRoot -Force
$entries = @()
$failures = @()
$unchanged = 0
$utf8 = [Text.UTF8Encoding]::new($false)
function Test-RelativePath([string]$Path) {
    return $Path -and -not [IO.Path]::IsPathRooted($Path) -and $Path -notmatch '(^|[\\/])\.\.([\\/]|$)' -and $Path -notmatch ':'
}
$seen = @{}
foreach ($file in $nativeFiles) {
    $localPath = if ($file.localPath) { $file.localPath } else { $file.path }
    if (-not (Test-RelativePath $file.path) -or -not (Test-RelativePath $localPath)) { throw 'Invalid relative source path in manifest.' }
    if ($seen.ContainsKey($localPath)) { throw "Duplicate local file in manifests: $localPath" }
    $seen[$localPath] = $true
    $local = Join-Path $projectRoot $localPath
    $approved = @($approvedFiles | Where-Object path -eq $file.path)
    $exists = Test-Path -LiteralPath $local -PathType Leaf
    if (-not $exists -and ($approved.Count -ne 1 -or -not $approved[0].allowMissing)) {
        $failures += "Native file missing: $localPath"
        continue
    }
    $localBytes = [byte[]]@()
    if ($exists) { $localBytes = [IO.File]::ReadAllBytes($local) }
    if ($exists -and (Test-ReferenceHash $localBytes $file.gitBlob)) { ++$unchanged; continue }
    $reference = Join-Path $referenceDirectory $file.path
    if (-not (Test-Path -LiteralPath $reference -PathType Leaf)) {
        $failures += "Reference file required for modified source: $($file.path)"
        continue
    }
    $referenceBytes = [IO.File]::ReadAllBytes($reference)
    if (-not (Test-ReferenceHash $referenceBytes $file.gitBlob)) {
        $failures += "Reference does not match pinned Git blob: $($file.path)"
        continue
    }
    $before = Join-Path $runRoot ('before\' + $file.path)
    $after = Join-Path $runRoot ('after\' + $file.path)
    $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($before)) -Force
    $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($after)) -Force
    $a = [Text.Encoding]::UTF8.GetString($referenceBytes).Replace("`r`n", "`n").TrimStart([char]0xFEFF).TrimEnd()
    $b = [Text.Encoding]::UTF8.GetString($localBytes).Replace("`r`n", "`n").TrimStart([char]0xFEFF).TrimEnd()
    [IO.File]::WriteAllText($before, $a + "`n", $utf8)
    [IO.File]::WriteAllText($after, $b + "`n", $utf8)
    $diff = & git.exe -c core.autocrlf=false -c core.safecrlf=false diff --no-index --ignore-space-at-eol --ignore-blank-lines --numstat -- $before $after
    if ($LASTEXITCODE -gt 1) { throw "Git comparison failed: $($file.path)" }
    $stats = @($diff | Where-Object { $_ -match '^\d+\s+\d+\s' })
    if (-not $stats.Count) { ++$unchanged; continue }
    $parts = $stats[0] -split '\s+', 3
    $added = [int]$parts[0]
    $removed = [int]$parts[1]
    if (-not $added -and -not $removed) { ++$unchanged; continue }
    $reason = ''
    if ($approved.Count -ne 1) {
        $failures += "Unreviewed native intervention: $($file.path)"
    } else {
        $reason = $approved[0].reason
        if ($added -gt $approved[0].maxAdded -or $removed -gt $approved[0].maxRemoved) {
            $failures += "Native intervention exceeded reviewed boundary: $($file.path) (+$added/-$removed)"
        }
    }
    $entries += [pscustomobject]@{ path = $file.path; localPath = $localPath; missing = -not $exists; added = $added; removed = $removed; reason = $reason }
}
$report = [pscustomobject]@{
    commit = $baseline.commit
    scope = $Scope
    nativeFiles = $nativeFiles.Count
    unchangedFiles = $unchanged
    modifiedFiles = $entries.Count
    addedLines = [int]($entries | Measure-Object -Property added -Sum).Sum
    removedLines = [int]($entries | Measure-Object -Property removed -Sum).Sum
    files = $entries
    failures = $failures
}
if (-not $ReportPath) { $ReportPath = Join-Path $runRoot 'report.json' }
$reportDirectory = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($ReportPath))
$null = New-Item -ItemType Directory -Path $reportDirectory -Force
[IO.File]::WriteAllText([IO.Path]::GetFullPath($ReportPath), ($report | ConvertTo-Json -Depth 6), $utf8)
Write-Output "Native boundary ($Scope): $($report.nativeFiles) files, $unchanged unchanged, $($entries.Count) differences, +$($report.addedLines)/-$($report.removedLines) lines."
if ($failures.Count) { throw ($failures -join "`n") }
Write-Output "PASS: pinned upstream hashes and native intervention limits. Report: $ReportPath"
