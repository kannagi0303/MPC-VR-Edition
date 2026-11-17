# MPC Video Renderer Edition

- Based on [MPC Video Renderer 0.11.1.2636](https://github.com/Aleksoid1978/VideoRenderer/releases/tag/0.11.1).
- **Windows x64 only.**<br>
  Works with players that support MPC Video Renderer.

## Edition Features

- **NVIDIA DLSS**
  - **Multi Frame Generation (MFG)**<br>
    Adds frames for smoother motion.
  - **Super Resolution (SR)**<br>
    Upscales video.
  - **DLAA**<br>
    Smooths edges without changing the video size.
- **Repeat detection**<br>
  Finds repeated frames so MFG can fill the gap.
- **Processing order**<br>
  Choose whether to upscale or add frames first.
- **ZipDepth**<br>
  Uses the GPU to estimate depth from video.

## Installation

### Install

1. Download `MPC-VR-Edition.zip` from [Releases](https://github.com/kannagi0303/MPC-VR-Edition/releases).
   Extract the whole ZIP.
2. Run `Install_MPCVR_64.cmd` as administrator.
3. Keep the extracted files in place.

### Uninstall

Run `Uninstall_MPCVR_64.cmd` as administrator.

> **Important**
>
> Edition and the original MPC Video Renderer<br>
> use the same DirectShow CLSID.<br>
> Only one can be registered at a time.

## How It Works

Choose **Direct3D 11** in the renderer settings.<br>
Turn on the features you want under **Edition Extended**.

### Frame generation and timing

MFG uses video timestamps and the display refresh rate<br>
to decide how many frames to add.<br>
It gives priority to original frames and can skip late generated frames.

The renderer can skip repeated frames when MFG can fill the gap.

### Upscaling and processing order

DLSS SR upscales supported 8-bit SDR video.<br>
If the renderer processes the video at its original resolution,<br>
it uses DLAA to smooth edges.

On supported videos, DLSS SR or RTX Video Super Resolution<br>
can run before or after MFG.<br>
Upscaling first gives MFG a larger image to work with.<br>
MFG can use the original video size when the video and output support it.<br>
Some playback paths resize the video before MFG, even with DLSS SR and RTX VSR off.

### Startup

- **Play first** (default)<br>
  Start playback while the selected Extended features load.<br>
  They turn on when ready.
- **Wait for Extended**<br>
  Prepare MFG and DLSS SR before playback.<br>
  MFG starts adding frames once it has a pair of source frames.

### Depth estimation

ZipDepth uses DirectML to estimate depth on the GPU.

- **Both** (default)<br>
  Share depth between DLSS SR / DLAA and MFG when enabled.
- **DLSS SR Only** or **MFG Only**<br>
  Use ZipDepth for one feature.
- **Disable**<br>
  Turn off depth estimation.

DLSS SR, DLAA, and MFG use flat depth while an estimate is unavailable.

### Change the ZipDepth Model

- **384×384 mixed FP16**<br>
  The default pack for general playback. It has a smaller file size.
- **512×288 FP32**<br>
  An optional pack for widescreen videos.<br>
  It uses a 16:9 input and more horizontal pixels.

1. Close the player.
2. Download your pack from [ZipDepth Models](../models/README.md).
3. Replace `edition/zipdepth.dmlpack` with the downloaded file.
4. Open the player again.

Keep the file name `zipdepth.dmlpack`.<br>
To restore the default, use the 384×384 pack.

## References

- [NVIDIA DLSS](https://github.com/NVIDIA/DLSS)
- [ZipDepth model downloads and license](../models/README.md)
- [Playback guide](../build-support/README.dist.md)
- [ZipDepth converter guide](../tools/zipdepth-convert.md)
- [Windows x64 build script](../build-x64.cmd)
- [Project license](LICENSE.md)
- [Third-party licenses](../licenses/)

## About the Author

### [かんなぎ (Kannagi)](https://github.com/kannagi0303)

I started this project to try DLSS<br>
with the player and renderer I already use.<br>
I paid close attention to camera pans.<br>
I also watched how evenly frames appeared.<br>
I wanted to reduce GPU work while keeping playback smooth.

I mainly test with MPC-BE on Windows 11,<br>
an RTX 4060 Ti 16 GB, and a 4K 60 Hz display.

I also work on [yt-dlp-gui](https://github.com/kannagi0303/yt-dlp-gui),<br>
[yt-dlp-gui-v2](https://github.com/kannagi0303/yt-dlp-gui-v2), and [RelayGate](https://github.com/kannagi0303/RelayGate).
