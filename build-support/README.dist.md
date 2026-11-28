# MPC Video Renderer Edition 0.3.1

- **Windows x64 only.**<br>
  Use a player that supports MPC Video Renderer.

## Installation

### Install

1. Extract the whole package to a folder you plan to keep.
2. Run `Install_MPCVR_64.cmd` as administrator.
3. Keep `MPCVRMFG64.ax` and the `edition` folder together.

### Uninstall

Run `Uninstall_MPCVR_64.cmd` as administrator from the package folder.

> **Important**
>
> Edition and the original MPC Video Renderer<br>
> use the same DirectShow CLSID.<br>
> Only one can be registered at a time.

To switch back, uninstall Edition and register the original again.

### Update or Move

1. Close the player.
2. Keep a copy of your custom VR profiles and any permitted research model files.
3. Replace the package files.

If you move the package, uninstall it from the old folder first.<br>
Then run the install script from the new folder.

## Playback Settings

Select **MPC Video Renderer** in your player.<br>
In MPC-BE x64, find this setting under **View → Options → Video**.<br>
Reopen the video after changing the renderer.

Choose **Direct3D 11** in the renderer settings.<br>
Use **Edition Extended** to turn on the features you want.

- **DLSS SR / DLAA**<br>
  Choose the largest source size to use with DLSS.<br>
  Supports 8-bit SDR video.
- **Enable DLSS Multi Frame Generation**<br>
  Adds frames based on video timing and the display refresh rate.
- **Order**<br>
  Choose whether to upscale or add frames first.
- **Startup**<br>
  **Play first** starts video while Extended features load. This is the default.<br>
  **Wait for Extended** prepares the selected features before playback.
- **Depth use**<br>
  Choose **Both**, **DLSS SR Only**, **MFG Only**, or **Disable**.<br>
  **Disable** is the default. Select another mode only for permitted research.
- **Enable video enhancement**<br>
  Controls Color Enhancement, Memory Color, Skin Tone, and Adaptive Tone on supported SDR playback paths while retaining their strengths. Set any effect to **0%** to disable it individually. With valid depth, Color Enhancement at **100%** can dim distant areas by up to 7.5% in linear light.
- **Show Edition information**<br>
  Switches **Show statistics** in the **Renderer** tab to the compact Edition view with a playback buffer timeline when available. Clear it to use the original MPC-VR statistics.
- **Show debug diagnostics**<br>
  Adds detailed timing and buffer information to the Edition statistics.
- **Show OSD**<br>
  Independently enables a brief corner notice for playback enhancements. It can show the upscale resolution, the MFG target FPS, or active enhancements. The target FPS is the planned rate. The statistics panel takes priority while visible.
- **Show depth overlay**<br>
  Shows the current source depth map when available.
- **Show optical flow**<br>
  Draws MFG motion arrows on the final picture.<br>
  Arrows point from the current source frame to the previous one.

## Package Files

| File | Purpose |
| --- | --- |
| `MPCVRMFG64.ax` | DirectShow video renderer |
| `Install_MPCVR_64.cmd` | Registers the renderer |
| `Uninstall_MPCVR_64.cmd` | Removes the registration |
| `edition/nvngx_dlss.dll` | NVIDIA runtime for DLSS SR and DLAA |
| `edition/nvngx_dlssg.dll` | NVIDIA runtime for frame generation |
| `edition/mpcvrmfg_nvngx.dll` | Edition bridge for frame generation |
| `edition/DirectML.dll` | DirectML runtime for SVDLUT and optional depth estimation |
| [`LICENSE.md`](LICENSE.md) | Project license and third-party notices |

## Optional research depth

The optional Depth Pro 2-S 352×192 W4 mixed FP16 resources are for non-commercial scientific research or academic development under the [Apple model agreement](LICENSE-Apple-DepthPro2.txt). See [RESEARCH-ONLY.txt](RESEARCH-ONLY.txt) for attribution and the changes made for DirectML.

For permitted research, open **Edition Extended**, choose **Both**, **DLSS SR Only**, or **MFG Only** under **Depth use**, and select **Apply**. Enable the corresponding DLSS feature and reopen the video if needed. Depth estimation starts disabled on new installations. If upgrading from a version before 0.3.0, select a depth mode again for research. DLSS SR, DLAA, and MFG use neutral depth when estimation is disabled or unavailable.

Keep the matching **dp2-s-w4-352x192-fp16.dmlgraph** and **depthpro2-s-fp16.dmlweights** in **edition/** and retain the Apple agreement and research notice with them.


## References

- [Depth converter guide](https://github.com/kannagi0303/MPC-VR-Edition/blob/main/build-support/depth/README.md)
- [Releases and source code](https://github.com/kannagi0303/MPC-VR-Edition/releases)
