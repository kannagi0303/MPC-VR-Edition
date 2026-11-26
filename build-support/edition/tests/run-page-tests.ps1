param([string]$MSBuildPath, [switch]$VisualTabsOnly)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $MSBuildPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio vswhere.exe is required.' }
    $visualStudioRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $visualStudioRoot) { throw 'Visual Studio C++ tools were not found.' }
    $MSBuildPath = Join-Path $visualStudioRoot 'MSBuild\Current\Bin\amd64\MSBuild.exe'
    if (!(Test-Path -LiteralPath $MSBuildPath -PathType Leaf)) {
        $MSBuildPath = Join-Path $visualStudioRoot 'MSBuild\Current\Bin\MSBuild.exe'
    }
}
if (-not (Test-Path -LiteralPath $MSBuildPath)) { throw 'MSBuild.exe was not found.' }

# Uses only installed/local dependencies. Does not invoke dependency download,
# filter registration, packaging, or a renderer that writes user preferences.
$commonBuildArguments = @('/p:Configuration=Release', '/p:Platform=x64',
    "/p:SolutionDir=$projectRoot\", '/m:1', '/nr:false', '/nologo', '/v:minimal')
& $MSBuildPath (Join-Path $projectRoot 'MpcVideoRenderer.sln') /t:MpcVideoRenderer @commonBuildArguments
$buildArguments = @('/t:Build') + $commonBuildArguments
if ($LASTEXITCODE -ne 0) { throw 'Renderer build failed.' }
& $MSBuildPath (Join-Path $PSScriptRoot 'EditionPageIntegration.vcxproj') @buildArguments
if ($LASTEXITCODE -ne 0) { throw 'Property page test build failed.' }

if (-not $VisualTabsOnly) {
# Compile the actual production source-selection method against backend doubles.
# Source extraction avoids maintaining a second copy of the routing algorithm.
$processingSource = [IO.File]::ReadAllText((Join-Path $projectRoot 'Source\Edition\DX11ProcessingSource.cpp'))
$processingMarker = 'const Tex2D_t* CDX11VideoProcessor::SelectEditionProcessingSource('
$processingStart = $processingSource.IndexOf($processingMarker)
if ($processingStart -lt 0 -or $processingStart -ne $processingSource.LastIndexOf($processingMarker)) {
    throw 'The processing-source test entry point could not be located uniquely.'
}
$generatedDirectory = Join-Path $projectRoot '_bin\edition-tests\generated'
$null = New-Item -ItemType Directory -Path $generatedDirectory -Force
# Exercise the native key reader/writer and startup-only tuning from their real
# implementation, together with Edition migrations and save verification.
$rendererSource = [IO.File]::ReadAllText((Join-Path $projectRoot 'Source\VideoRenderer.cpp'))
$nativeKeys = [regex]::Matches($rendererSource, '(?m)^#define OPT_[^\r\n]+') | ForEach-Object Value
$readMarker = 'void CMpcVideoRenderer::ReadSettings('
$constructorMarker = 'CMpcVideoRenderer::CMpcVideoRenderer('
$readStart = $rendererSource.IndexOf($readMarker)
$readEnd = $rendererSource.IndexOf($constructorMarker)
$tuningStart = $rendererSource.IndexOf("`t`tDWORD dw;", $readEnd)
$tuningEnd = $rendererSource.IndexOf("`tReadPresetKeys();", $tuningStart)
$verifySource = [IO.File]::ReadAllText((Join-Path $projectRoot 'Source\Edition\RendererSettings.cpp'))
$verifyMarker = 'HRESULT VerifySavedSettings('
$verifyStart = $verifySource.IndexOf($verifyMarker)
if ($readStart -lt 0 -or $readEnd -le $readStart -or $tuningStart -lt 0 -or $tuningEnd -le $tuningStart -or
    $verifyStart -lt 0 -or $readStart -ne $rendererSource.LastIndexOf($readMarker) -or
    $verifyStart -ne $verifySource.LastIndexOf($verifyMarker)) {
    throw 'The settings persistence test entry points could not be located uniquely.'
}
$nativeMethods = $rendererSource.Substring($readStart, $readEnd - $readStart).Replace('CMpcVideoRenderer::', 'SettingsPersistence::')
# The slice ends with the existing key.Open conditional's closing brace.
$tuning = $rendererSource.Substring($tuningStart, $tuningEnd - $tuningStart).TrimEnd()
if (-not $tuning.EndsWith('}')) { throw 'The startup tuning block has changed.' }
$tuning = $tuning.Substring(0, $tuning.Length - 1).Replace('m_Sets.', 'sets.')
$generated = ($nativeKeys -join "`n") + "`n" + $nativeMethods +
    "`nvoid ReadStartupTuning(CRegKey& key, Settings_t& sets) {`n" + $tuning + "`n}`nnamespace edition {`n" +
    $verifySource.Substring($verifyStart)
[IO.File]::WriteAllText((Join-Path $generatedDirectory 'SettingsPersistence.generated.inc'), $generated, [Text.UTF8Encoding]::new($false))
& $MSBuildPath (Join-Path $PSScriptRoot 'SettingsPersistenceIntegration.vcxproj') @buildArguments
if ($LASTEXITCODE -ne 0) { throw 'Settings persistence test build failed.' }
& (Join-Path $projectRoot '_bin\edition-tests\SettingsPersistenceIntegration.exe')
if ($LASTEXITCODE -ne 0) { throw 'Settings persistence tests failed.' }
$vpSource = [IO.File]::ReadAllText((Join-Path $projectRoot 'Source\D3D11VP.cpp'))
$bridgeSource = [IO.File]::ReadAllText((Join-Path $projectRoot 'Source\Edition\D3D11VideoProcessorBridge.cpp'))
$vpMarker = 'HRESULT CD3D11VP::SetSuperRes(const int'
$vpEndMarker = 'HRESULT CD3D11VP::SetRTXVideoHDRNvidia('
$policyMarker = 'std::optional<HRESULT> CD3D11VP::ApplyEditionSuperResPolicy('
$vpStart = $vpSource.IndexOf($vpMarker)
$vpEnd = $vpSource.IndexOf($vpEndMarker)
$policyStart = $bridgeSource.IndexOf($policyMarker)
if ($vpStart -lt 0 -or $vpEnd -le $vpStart -or $policyStart -lt 0 -or
    $vpStart -ne $vpSource.LastIndexOf($vpMarker) -or $policyStart -ne $bridgeSource.LastIndexOf($policyMarker)) {
    throw 'The native VP/product policy test entry points could not be located uniquely.'
}
[IO.File]::WriteAllText((Join-Path $generatedDirectory 'SuperResolutionPolicy.generated.inc'),
    ($vpSource.Substring($vpStart, $vpEnd - $vpStart) + $bridgeSource.Substring($policyStart)), [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $generatedDirectory 'EditionProcessingSource.generated.inc'),
    $processingSource.Substring($processingStart), [Text.UTF8Encoding]::new($false))
$pipelineSource = [IO.File]::ReadAllText((Join-Path $projectRoot 'Source\Edition\DX11VideoPipeline.cpp'))
$prepareMarker = 'std::optional<HRESULT> CDX11VideoProcessor::PrepareEditionRender('
$renderMarker = 'std::optional<HRESULT> CDX11VideoProcessor::RenderEditionVideo('
$prepareStart = $pipelineSource.IndexOf($prepareMarker)
$prepareEnd = $pipelineSource.IndexOf($renderMarker)
if ($prepareStart -lt 0 -or $prepareEnd -le $prepareStart -or
    $prepareStart -ne $pipelineSource.LastIndexOf($prepareMarker) -or
    $prepareEnd -ne $pipelineSource.LastIndexOf($renderMarker)) {
    throw 'The Render preparation test entry points could not be located uniquely.'
}
[IO.File]::WriteAllText((Join-Path $generatedDirectory 'EditionRenderPreparation.generated.inc'),
    $pipelineSource.Substring($prepareStart, $prepareEnd - $prepareStart), [Text.UTF8Encoding]::new($false))
& $MSBuildPath (Join-Path $PSScriptRoot 'ProcessingSourceIntegration.vcxproj') @buildArguments
if ($LASTEXITCODE -ne 0) { throw 'Processing-source test build failed.' }
& (Join-Path $projectRoot '_bin\edition-tests\ProcessingSourceIntegration.exe')
if ($LASTEXITCODE -ne 0) { throw 'Processing-source transition tests failed.' }

}

$runDirectory = Join-Path $projectRoot ('_bin\edition-tests\run-' + [guid]::NewGuid().ToString('N'))
$withProfiles = Join-Path $runDirectory 'with-profiles'
$withoutProfiles = Join-Path $runDirectory 'without-profiles'
$null = New-Item -ItemType Directory -Path (Join-Path $withProfiles 'VRProfiles') -Force
$null = New-Item -ItemType Directory -Path $withoutProfiles -Force
$filter = Join-Path $projectRoot '_bin\Filter_x64\MPCVRMFG64.ax'
Copy-Item -LiteralPath $filter -Destination (Join-Path $withProfiles 'MPCVRMFG64.ax')
Copy-Item -LiteralPath $filter -Destination (Join-Path $withoutProfiles 'MPCVRMFG64.ax')
Copy-Item -LiteralPath (Join-Path $projectRoot 'build-support\VRProfiles\jvrporn.ini') -Destination (Join-Path $withProfiles 'VRProfiles\jvrporn.ini')
$test = Join-Path $projectRoot '_bin\edition-tests\EditionPageIntegration.exe'
$pageArgs = @()
if ($VisualTabsOnly) { $pageArgs += '--visual-tabs' }
& $test (Join-Path $withProfiles 'MPCVRMFG64.ax') 1 @pageArgs
if ($LASTEXITCODE -ne 0) { throw 'Property page tests with JVR profiles failed.' }
& $test (Join-Path $withoutProfiles 'MPCVRMFG64.ax') 0 @pageArgs
if ($LASTEXITCODE -ne 0) { throw 'Property page tests without JVR profiles failed.' }
if ($VisualTabsOnly) {
    Write-Output 'Visible tab/DC/background and Apply host-repaint tests passed with and without VR profiles.'
} else {
    Write-Output 'Edition settings persistence, VP policy, processing-source and property page tests passed. Isolated artifacts were retained under _bin/edition-tests.'
}
