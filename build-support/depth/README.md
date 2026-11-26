# Depth Pro 2 resource preparation

The renderer accepts one matching Depth Pro 2-S 352×192 W4 pair with the upstream mixed FP16 precision. The [Apple Machine Learning Research Model License Agreement](../../licenses/LICENSE-Apple-DepthPro2.txt) applies to the checkpoint and its derivatives. Use the research build only for permitted research. DLSS SR and MFG use neutral depth while estimation is disabled or resources are unavailable.

## Build from source

Run **build-x64.cmd** for the local incremental build. It stages the validated resources in **dist/edition/**. When packaging the model pair for research, keep RESEARCH-ONLY.txt and LICENSE-Apple-DepthPro2.txt with it. A package for general playback can omit these four research files. Keep the SVDLUT graph and the playback licenses.

The build checks the fixed split pair in `%LOCALAPPDATA%\MPC-VR-Edition\DepthPro2Research` first, then the verified native intermediate and the ONNX cache in the same directory. It requests the pinned offline exporter only when none of these resources can supply the model. The exporter uses the upstream lockfile and runs in `%LOCALAPPDATA%\DepthPro2OfflineExport`, with Python and PyTorch required for export. Adaptation of a previously exported ONNX uses only Python and ONNX. The converter validates DirectML execution and checks native output against ONNX Runtime on a frame sequence before accepting the intermediate.

The pinned exporter is `protortyp/depthpro2-onnx` at `1afa8587b6c285875b29d80892a457016ea0757f`. The official Apple S checkpoint is `903a1c1ade918831d138adcc4aedbcc8f4da0078`. The exporter supplies the graph and precision policy. The Edition adapter binds focal length for the inverse-depth renderer interface and applies the targeted DirectML Reshape compatibility fix.

## Use a precompiled converter

If you have a permitted and compatible graph and weight pair, keep both files in the renderer package **edition/** directory. Select a depth mode under **Edition Extended → Depth use** and apply it before running the experiment.

When a precompiled converter is available, use `depth-convert.exe` beside its matching `DirectML.dll`. It embeds its offline converter dependencies and requires a GPU with DirectML support. Run these commands with an Edition-adapted 352×192 W4 mixed FP16 ONNX you may use for research.

```cmd
depth-convert.exe model.onnx model.dmlpack
depth-convert.exe --split model.dmlpack dp2-s-w4-352x192-fp16.dmlgraph depthpro2-s-fp16.dmlweights
depth-convert.exe --verify-pair model.dmlpack dp2-s-w4-352x192-fp16.dmlgraph depthpro2-s-fp16.dmlweights
```

Only the Graph and weights go into `edition/`. `depth-convert.exe --licenses` displays the converter's third-party notices. A source build that needs conversion or splitting produces this same tool in `_bin/depth-converter/`. The playback package omits the converter.
