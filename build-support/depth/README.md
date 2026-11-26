# Depth Pro 2-S model preparation

Edition uses the pinned Apple Depth Pro 2-S checkpoint at 352×192, W4, with the upstream mixed FP16 export. The [Apple model agreement](../../licenses/LICENSE-Apple-DepthPro2.txt) applies to the checkpoint and its derivatives. Keep the agreement and research notice with the resulting model pair.

## Normal build

Run `build-x64.cmd`. A validated split pair is reused directly. An existing validated native pack can be split without rebuilding the graph. Otherwise the build uses the pinned `config.json` and `model.safetensors` from its verified cache or Hugging Face, and builds and runs the existing `DepthConvert` project. The CPU converter requires no Python, PyTorch, uv, Git, ONNX Runtime, DirectML DLL, or GPU. The renderer still needs its ordinary DirectML playback dependencies.

## Standalone conversion

Obtain `config.json` and `model.safetensors` from the [pinned official checkpoint](https://huggingface.co/apple/DepthPro2-S/tree/903a1c1ade918831d138adcc4aedbcc8f4da0078). Run the x64 `depth-convert.exe` from the build output at `_bin/depth-converter/` or use the same prebuilt EXE. Choose two new files in an existing directory.

```powershell
depth-convert.exe --from-safetensors model.safetensors config.json dp2-s-w4-352x192-fp16.dmlgraph depthpro2-s-fp16.dmlweights
```

Place both files in `edition/`. For permitted research, select a mode under **Edition Extended → Depth use**, apply it, and reopen the video if needed. The EXE is a resource-preparation tool. Playback does not invoke it.

## Recipe and parity

`DepthPro2S-352x192.recipe` contains the exported DirectML graph, six external auxiliary constants and the generated mapping for 201 learned tensors. The graph also contains 42 inline scalar constants (90 bytes). The six external constants cover normalization, zero values, and two halves of the FP32 rotary position table for the 352×192 image. The C++ writer reconstructs the learned tensors from Safetensors using FP16 conversion, matrix transposes, prefix concatenation and fused channel scaling. The graph retains the attention splits and W4 state bindings. The FP32 attention and depth head bindings retain the exact exporter values, including values first rounded to FP16 and widened by the upstream exporter.

The converter checks both input files by SHA-256, checks each generated tensor, and verifies the reconstructed native pack's digest before writing the split outputs. The reference pack was validated with a single DirectML partition, no CPU execution, and 10 consecutive frames with a history reset at frame 8. The recipe does not store learned tensor bytes.

## Regenerating the recipe

Ordinary builds consume the checked-in recipe. Use the maintenance path when reviewing an upstream change or reproducing the checked-in recipe. It uses the existing pinned exporter, locked Python/PyTorch environment, ONNX graph adapter and ORT DirectML validator. Maintain the same checkpoint, exporter revision, exporter lockfile, DirectML and ONNX Runtime versions for a byte-for-byte reproduction.

From a Developer PowerShell at the source root, use a work directory outside the source repository. Run the build scripts in a PowerShell process with `-ExecutionPolicy Bypass`, as the CMD entry point does for ordinary builds.

```powershell
$work = Join-Path $env:LOCALAPPDATA 'MPC-VR-Edition\DepthRecipeMaintenance'
$null = New-Item -ItemType Directory -Force -Path $work
. .\build-support\depth\model-cache.ps1
$checkpoint = Get-PinnedDepthCheckpoint $work
& .\build-support\depth\export-depthpro2.ps1 -WorkDirectory $work
& .\build-support\prepare-directml.ps1 -WithConverter -WithOnnxMaintenance -VisualStudioRoot $env:VSINSTALLDIR
msbuild MpcVideoRenderer.sln /t:DepthConvert /p:Configuration=Release /p:Platform=x64 /p:DepthOnnxMaintenance=true
$converter = (Resolve-Path .\_bin\depth-converter-maintenance\depth-convert.exe).Path
& .\build-support\depth\convert-depthpro2.ps1 -Converter $converter -WorkDirectory $work -Rebuild
$candidate = Join-Path $work ('DepthPro2S-352x192-' + [guid]::NewGuid() + '.recipe')
& $converter --make-recipe ($checkpoint['model.safetensors']) ($checkpoint['config.json']) `
    (Join-Path $work 'dp2-s-w4-352x192-fp16.dmlpack') $candidate
Get-FileHash -Algorithm SHA256 $candidate, .\build-support\depth\DepthPro2S-352x192.recipe
```

The current recipe SHA-256 is `6281379d6d648f50e087290d525f22df7c6db4d1cc4331c8405a2ed9ed05d54a`. A different digest for a fixed revision needs investigation. For an intentional version update, review the mapping, graph and fixed model contract, then rebuild the ordinary converter with the new recipe. Run `--from-safetensors` and verify the resulting pair against the newly validated reference:

```powershell
depth-convert.exe --verify-pair REFERENCE.dmlpack OUTPUT.dmlgraph OUTPUT.dmlweights
```

The maintenance build can also load the resulting split pair through the renderer's native loader and compare its outputs with the adapted ONNX:

```powershell
depth-convert.exe --validate-split ADAPTED.onnx OUTPUT.dmlgraph OUTPUT.dmlweights
```

`depth-convert.exe --licenses` prints the Apple model agreement and the converter's third-party notices. If distributing the prebuilt EXE with the embedded recipe, include the agreement and the research notice with it.
