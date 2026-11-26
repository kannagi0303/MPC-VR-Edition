# MPC Video Renderer Edition

- Based on [MPC Video Renderer 0.11.1.2636](https://github.com/Aleksoid1978/VideoRenderer/releases/tag/0.11.1).
- Edition source version: **0.3.0**.
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
- **Optional depth estimation**<br>
  Shares a depth map between DLSS SR / DLAA and MFG when compatible research resources are installed.
- **SDR video enhancement**<br>
  Adjust color grading, memory colors, skin tones, and adaptive contrast independently.
- **VR projection**<br>
  View one eye of supported side-by-side fisheye video as a flat 16:9 picture.

## Installation

### Install

1. Download the Edition ZIP from [Releases](https://github.com/kannagi0303/MPC-VR-Edition/releases).
   Extract the whole ZIP.
2. Run `Install_MPCVR_64.cmd` as administrator.
3. Keep `MPCVRMFG64.ax` and its `edition/` folder together.

### Uninstall

Run `Uninstall_MPCVR_64.cmd` as administrator.

> **Important**
>
> Edition and the original MPC Video Renderer<br>
> use the same DirectShow CLSID.<br>
> Only one can be registered at a time.

## How It Works

In the **Renderer** tab, select **Use Direct3D 11** to use Edition features. This tab also holds video processor, scaling, HDR, and playback statistics settings. Open **Edition Extended** for DLSS, depth routing, video enhancement, and display information.

DLSS requires a compatible NVIDIA RTX GPU and driver. DLSS SR / DLAA supports 8-bit SDR video.

### Frame generation and timing

MFG uses video timestamps and the display refresh rate<br>
to decide how many frames to add.<br>
It gives priority to original frames and can skip late generated frames.

The renderer can skip repeated frames when MFG can fill the gap.

### Upscaling and processing order

Use **DLSS SR / DLAA** to choose the largest source resolution eligible for DLSS. The target is the display-fit size. At 1:1 the renderer uses DLAA, and when upscaling it uses DLSS SR.

Use **Order** to select **Upscale then MFG** or **MFG then upscale**. Both orders can use DLSS SR or RTX Video Super Resolution on supported playback paths.<br>
Upscaling first gives MFG a larger image to work with.<br>
MFG can use the original video size when the video and output support it.<br>
Some playback paths resize the video before MFG, even with DLSS SR and RTX VSR off.

### Startup

- **Play first** (default)<br>
  Start playback while the selected Extended features load.<br>
  They turn on when ready.
- **Wait for Extended**<br>
  Prepare enabled MFG and DLSS SR features when opening a video.<br>
  MFG starts adding frames once it has a pair of source frames.

### Depth estimation

**Depth use** routes the optional Depth Pro 2-S 352×192 W4 model, which runs on DirectML with upstream mixed FP16 precision.

- **Both**<br>
  Share depth between DLSS SR / DLAA and MFG when enabled.
- **DLSS SR Only** or **MFG Only**<br>
  Use depth estimation for one feature.
- **Disable** (default)<br>
  Keep depth estimation off.

The package can include the model files for non-commercial scientific research or academic development. Read RESEARCH-ONLY.txt and the [Apple model agreement](../licenses/LICENSE-Apple-DepthPro2.txt) before enabling depth. On upgrade to 0.3.0, select a depth mode again for research. DLSS SR, DLAA, and MFG use neutral depth when estimation is disabled or unavailable.

### Video Enhancement

These controls apply to supported SDR playback paths. Each has a numeric field and a slider from **0% to 100%**. Set an individual control to **0%** to bypass it.

| Control | Effect |
| --- | --- |
| **Color Enhancement** | Applies the calibrated SVDLUT color grade. |
| **Memory Color** | Adjusts colors such as sky and foliage. |
| **Skin Tone** | Independently adjusts skin tones. |
| **Adaptive Tone** | Adjusts contrast based on the current shot. |

Use **Enable video enhancement** to bypass or re-enable all four effects together while retaining their saved strengths.

### Display Information

- **Show Edition information** adds a compact summary to **Show statistics** or a brief corner badge when statistics are hidden.
- **Include detailed diagnostics** adds GPU, depth, and buffer diagnostics to the summary.
- **Show depth overlay** displays the current depth map. **Show optical flow** draws MFG motion arrows on the picture.

### VR Projection

The **VR Projection** group appears when the renderer finds an `.ini` profile in `VRProfiles/` next to `MPCVRMFG64.ax`. For supported progressive SDR full side-by-side video, enable **Fisheye to flat**, choose an eye, and adjust **Vertical framing** and **Reprojection** as needed.

## Build from source

Run **build-x64.cmd** to build incrementally and prepare the depth resources. When the model pair is missing, the native converter reads the pinned Safetensors checkpoint and prepares the pair on the CPU. The build stages the renderer and the optional research resources in **dist/**. Keep the Apple agreement and research notice with the model files when packaging them.

## References

- [NVIDIA DLSS](https://github.com/NVIDIA/DLSS)
- [Depth model and license](../models/README.md)
- [Playback guide](../build-support/README.dist.md)
- [Depth converter guide](../build-support/depth/README.md)
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
