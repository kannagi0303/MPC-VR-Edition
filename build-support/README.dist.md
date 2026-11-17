# MPC Video Renderer Edition

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
2. Back up any custom `edition/zipdepth.dmlpack`.
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
- **ZipDepth**<br>
  Choose **Both**, **DLSS SR Only**, **MFG Only**, or **Disable**.<br>
  **Both** is the default. Features use flat depth while an estimate is unavailable.
- **Show feature status**<br>
  Shows which DLSS or RTX VSR features are active.
- **Show depth overlay**<br>
  Blends the grayscale depth map into the video before DLSS processing.
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
| `edition/DirectML.dll` | DirectML runtime for ZipDepth |
| `edition/zipdepth.dmlpack` | Default 384×384 mixed FP16 ZipDepth model |
| [`LICENSE.md`](LICENSE.md) | Project license and third-party notices |

## Change the ZipDepth Model

The renderer reads `edition\zipdepth.dmlpack` when ZipDepth starts.<br>
It detects 384×384 and 512×288 models from the file.

1. Close the player.
2. Download a pack from [ZipDepth Models](https://github.com/kannagi0303/MPC-VR-Edition/tree/main/models).
3. Replace `edition\zipdepth.dmlpack` with the downloaded file.
4. Open the player again.

To restore the default model, repeat these steps with the 384×384 pack.

## References

- [ZipDepth converter guide](https://github.com/kannagi0303/MPC-VR-Edition/blob/main/tools/zipdepth-convert.md)
- [Releases and source code](https://github.com/kannagi0303/MPC-VR-Edition/releases)
