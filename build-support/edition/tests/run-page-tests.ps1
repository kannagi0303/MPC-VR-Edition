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

# Compile the renderer's policy methods against small backend stand-ins.
# Markers identify source boundaries; the C++ fixtures assert behavior.
function Read-Production([string]$Path) {
    return [IO.File]::ReadAllText((Join-Path $projectRoot $Path)).Replace("`r`n","`n")
}
function Fragment([string]$Source, [string]$Start, [string]$End) {
    $first = $Source.IndexOf($Start,[StringComparison]::Ordinal)
    $last = if ($first -ge 0) { $Source.IndexOf($End,$first + $Start.Length,[StringComparison]::Ordinal) } else { -1 }
    if ($last -lt 0) { throw "Missing C++ fragment: $Start" }
    return $Source.Substring($first,$last - $first)
}
function Method([string]$Source, [string]$Name) {
    return (Fragment $Source $Name "`n}") + "`n}`n"
}
function Fixture([string]$Name, [string]$Source) {
    [IO.File]::WriteAllText((Join-Path $generatedDirectory "$Name.generated.inc"),$Source,[Text.UTF8Encoding]::new($false))
}
$frames = Read-Production 'Source/DX11VideoProcessorCompletedFrames.cpp'
$next = Method $frames 'bool CDX11VideoProcessor::NextCompletedFrame('
$present = Method $frames 'bool CDX11VideoProcessor::PresentCompletedFrame('
Fixture CompletedBufferMethod (Method $frames 'CDX11VideoProcessor::CompletedBufferTime CDX11VideoProcessor::GetCompletedBufferTime(')
Fixture FairProducerMethods ((Method $frames 'bool CDX11VideoProcessor::CompletedFairProducerCanvas(') +
    (Method $frames 'bool CDX11VideoProcessor::TryServiceCompletedProducer('))
Fixture FairAdmission (Fragment (Method $frames 'const char* CDX11VideoProcessor::CompletedAdmissionBlock(') '    if (hasCompletedFrame) {' '    if (completed == UINT64_MAX)')
Fixture FairPermit (Fragment $present '    if (!TryCompletedPresentPermit()) {' '    const auto completed = m_completedFence->GetCompletedValue();')
Fixture FairHandoff (Fragment $next '    const auto sourceTurn =' '    // Complete a deferred window resize')
Fixture FairPresent (Method $present '    if (hr == S_OK && CompletedFairProducerCanvas()) {')
Fixture ResizeAdmissionMethods ((Method $frames 'bool CDX11VideoProcessor::CompletedGeometryChanging(') +
    (Method $frames 'const char* CDX11VideoProcessor::CompletedOverlapBlock('))
Fixture ResizeCommit (Fragment $next 'constexpr ULONGLONG resizeSettleMs = 140;' '// Early B-only')
Fixture DxgiClassification (Fragment (Read-Production 'Source/Edition/DX11Statistics.cpp') 'const auto previousCount = m_mfgDxgiLastPresentCount;' 'm_mfgRateTick=rateTick;')
$depthHeader = Read-Production 'Source/Depth/Depth.h'
$depth = Read-Production 'Source/Depth/Depth.cpp'
Fixture DepthCounterMethods ((Fragment $depthHeader '    struct Counters {' '    static constexpr float OutputDepthSpan') +
    (Fragment $depthHeader '    void RecordSrEvaluation(' '    bool CoolingDown('))
Fixture DepthPublicationRate (Fragment $depth '    // Wall-clock rate of valid maps' "`n}`n`nCDepth::Estimate CDepth::ForFrame")
Fixture DepthConsumerGate (Fragment (Method $depth 'bool CDepth::DrawEstimate(') '    PollCompleted();' '    D3D11_TEXTURE2D_DESC desc{};')
$raw = Read-Production 'Source/Depth/DepthGpuShaders.h'
$draw = Read-Production 'Source/Depth/DepthDrawShader.h'
Fixture DepthRawScalars ((Method $raw 'bool FiniteDepth(') + ((Fragment $raw 'float2 RawEmpty()' "[numthreads(256, 1, 1)]`nvoid RawRangeTiles").Replace('0.0','0.0f')))
$normalize = [regex]::Match((Method $raw 'void RawNormalize('), 'Depth\[pixel\] = ([^;]+);')
$expand = [regex]::Match((Method $draw 'float RawExpand('), 'return (0\.5 \+ \(depth - 0\.5\)[^;]+);')
if (!$normalize.Success -or !$expand.Success) { throw 'Missing RAW shader expressions' }
Fixture DepthRawNormalize ($normalize.Groups[1].Value -replace '(?<![\w.])(0\.56|0\.12|0\.5)(?![\w.])','${1}f')
Fixture DepthRawExpand ($expand.Groups[1].Value -replace '(?<![\w.])(0\.12|0\.5)(?![\w.])','${1}f')
$tone = Read-Production 'Source/Edition/DX11AdaptiveTone.cpp'
Fixture AdaptiveTonePriming (Method $tone 'void CDX11VideoProcessor::PrimeAdaptiveToneFrames(')
Fixture SourceToneMethods ((Method $tone 'float CDX11VideoProcessor::AdaptiveToneWeight(') +
    (Method $tone 'bool CDX11VideoProcessor::SourceToneEligible(') +
    (Method $tone 'ID3D11Texture2D* CDX11VideoProcessor::PrepareSourceTone('))
Fixture DepthSourceToneSr (Method (Read-Production 'Source/Edition/DX11SuperResolution.cpp') 'Tex2D_t* CDX11VideoProcessor::EvaluateDlssSR(')
$color = Read-Production 'Source/DX11VideoProcessorSvdLut.cpp'
Fixture OptionalColorMethods ((Method $color 'void CDX11VideoProcessor::PrepareOptionalColorBytecode(') +
    (Method $color 'void CDX11VideoProcessor::PrimeOptionalColorFrames('))
Fixture PerceptualMethods ((Method $color 'bool CDX11VideoProcessor::PerceptualEnabled(') +
    (Method $color 'HRESULT CDX11VideoProcessor::DrawCompletedSvdLut(') + (Method $color 'void CDX11VideoProcessor::DrawSvdLut('))
$perceptualShader = Read-Production 'Source/DLSS/PerceptualColorShader.h'
$toneShader = Read-Production 'Source/Edition/AdaptiveToneShader.h'
$grade = Method $perceptualShader 'float4 SvdLutGradeMain('
$activation = Fragment $grade '    float activation = saturate(DepthTuning.w);' "#endif`n    if (Enhancements.x"
$tail = $grade.Substring($grade.IndexOf('    float3 toned =',[StringComparison]::Ordinal))
Fixture AdaptiveToneComposition ((Method $perceptualShader 'float Smooth(') +
    (Method $perceptualShader 'float3 Enhance(') + (Method $toneShader 'float3 ApplyAdaptiveTone(') +
    "`nfloat4 Compose(float3 original,float3 grade,float depthWeight,float farWeight) { struct {float a=1;} src;`n" +
    $activation + $tail)
$control = Read-Production 'Source/Edition/RendererControls.cpp'
$endHandler = "`nreturn {};`n}`n"
Fixture EnhancementControlMethods ((Method $control 'void CMpcVideoRenderer::ApplyEnhancementStrengths()') +
    (Fragment $control 'std::optional<HRESULT> CMpcVideoRenderer::SetEditionInt(' '    // One atomic MFG setting') + $endHandler +
    (Fragment $control 'std::optional<HRESULT> CMpcVideoRenderer::GetEditionInt(' '    // Independent settings channel') + $endHandler +
    (Fragment $control 'std::optional<HRESULT> CMpcVideoRenderer::SetEditionBool(' '    if (!strcmp(field,"adaptiveTone"))') + $endHandler +
    (Fragment $control 'std::optional<HRESULT> CMpcVideoRenderer::GetEditionBool(' '    if (!strcmp(field, "adaptiveTone"))') + $endHandler)

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
