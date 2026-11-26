# Model resources

This source tree includes the SVDLUT FiveK sRGB color graph and its license. The optional depth model for permitted research uses the official Depth Pro 2-S checkpoint at revision **903a1c1ade918831d138adcc4aedbcc8f4da0078**, exported at 352×192 with W4 and upstream mixed FP16.

The renderer looks for a matching **dp2-s-w4-352x192-fp16.dmlgraph** and **depthpro2-s-fp16.dmlweights** directly in **edition/**. The Release ZIP can include this pair as optional research resources. Depth is disabled by default. SR and MFG use neutral depth when estimation is disabled or the pair is unavailable.

Run **build-x64.cmd** for a permitted local research build. It reuses a verified resource pair or converts the pinned Safetensors checkpoint on the CPU. For standalone conversion and recipe regeneration, see the [converter guide](../build-support/depth/README.md).

[Apple Machine Learning Research Model License Agreement](../licenses/LICENSE-Apple-DepthPro2.txt)
