# Runs with the Windows PowerShell included in Windows. No Git or build required.
$ErrorActionPreference = 'Stop'

$root = (Get-Item -LiteralPath (Join-Path $PSScriptRoot '..')).FullName
$output = $root
$exportName = 'MPC-VR-Edition-for-GPT-{0:yyyyMMdd-HHmmss-fff}.zip' -f (Get-Date)
$destination = Join-Path $output $exportName
$temporary = Join-Path $output ([guid]::NewGuid().ToString('N') + '.partial')

# Include new project files by default. Skip generated directories before descent.
$excludedDirectories = @(
    '.git', '.vs', '.vscode', '.idea', '.agents', '.codex', '.aws', '.ssh', '.references',
    '_bin', '_obj', 'bin', 'obj', 'Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel',
    'artifacts', 'CMakeFiles', '__pycache__', '.cache', 'node_modules',
    '.zipdepth-build', '.zipdepth-lite-cache', '.zipdepth-directml-cache'
)
$excludedRootDirectories = @('build', 'builds', 'backup', 'backups', 'dist')
$excludedExtensions = @(
    '.exe', '.dll', '.ax', '.ocx', '.sys', '.lib', '.a', '.so', '.dylib',
    '.obj', '.o', '.res', '.exp', '.pdb', '.pch', '.idb', '.ipch', '.ipdb', '.iobj',
    '.ilk', '.cso', '.tlog', '.lastbuildstate', '.sdf', '.opensdf', '.suo', '.user',
    '.userosscache', '.db', '.opendb', '.log', '.tmp', '.bak', '.dmp', '.mdmp', '.etl', '.trace',
    '.zip', '.7z', '.rar', '.tar', '.gz', '.bz2', '.xz', '.nupkg', '.msi', '.partial',
    '.onnx', '.onnx_data', '.dmlpack', '.dmlgraph', '.tflite', '.safetensors', '.pt', '.pth', '.ckpt', '.gguf',
    '.pem', '.key', '.pfx', '.p12'
)

function Get-ExportFiles([string]$directory) {
    foreach ($item in Get-ChildItem -LiteralPath $directory -Force) {
        # Junctions and symbolic links may point outside this project.
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }

        if ($item.PSIsContainer) {
            if ($item.Name -in $excludedDirectories -or $item.Name -like 'cmake-build-*') { continue }
            if ($directory -eq $root -and ($item.Name -in $excludedRootDirectories -or
                $item.Name -like 'dist.stage-*' -or $item.Name -like 'dist.previous-*')) { continue }
            Get-ExportFiles $item.FullName
            continue
        }

        if ($item.Extension -in $excludedExtensions -or $item.Name -in @(
            '.git', 'CMakeCache.txt', 'Thumbs.db', 'desktop.ini', '.DS_Store'
        ) -or $item.Name -like '*.local.*' -or $item.Name -like '*~' -or $item.Name -match '^\.env($|\.)') {
            continue
        }
        if ($directory -eq $root -and $item.Name -eq 'revision.h') { continue }
        $item
    }
}

$temporaryCreated = $false
try {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $count = 0
    $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew)
    $temporaryCreated = $true
    try {
        $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $true)
        try {
            foreach ($file in Get-ExportFiles $root) {
                $entry = $file.FullName.Substring($root.Length + 1).Replace('\', '/')
                $null = [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                    $archive, $file.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal)
                $count++
            }
        } finally { $archive.Dispose() }
    } finally { $stream.Dispose() }

    if ($count -eq 0) { throw 'No project files were found.' }
    [IO.File]::Move($temporary, $destination)
    $temporaryCreated = $false
    $size = (Get-Item -LiteralPath $destination).Length / 1MB
    Write-Host ('Exported {0} files ({1:N2} MiB).' -f $count, $size)
    Write-Host $destination
} catch {
    if ($temporaryCreated) { [IO.File]::Delete($temporary) }
    [Console]::Error.WriteLine("Export failed: $($_.Exception.Message)")
    exit 1
}
