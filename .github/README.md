# MPC Video Renderer Edition

This is a personal edition of [**MPC Video Renderer 0.11.1.2636**](https://github.com/Aleksoid1978/VideoRenderer/releases/tag/0.11.1), made for my own video playback needs.

**NVIDIA DLSS Multi Frame Generation (MFG)** adds frames for smoother playback. **DLSS Super Resolution (DLSS SR)** upscales video.

This edition keeps the original renderer's features, including RTX Video Super Resolution (VSR). **MFG can also work together with VSR.**

It can work with DirectShow players. Full support is available in [MPC-BE](https://github.com/Aleksoid1978/MPC-BE).

**Windows x64 only.**

---

## Installation

Download and extract the latest release.

- Run `Install_MPCVR_64.cmd` to register the renderer.
- Run `Uninstall_MPCVR_64.cmd` to unregister it.

Keep the extracted files in the same location after registration.

> **Important**
>
> This edition uses the **same DirectShow CLSID as the original MPC Video Renderer**.
>
> Registering this edition replaces the original renderer's x64 registration. Both versions cannot be registered separately at the same time.
>
> To switch back to the original version, unregister this edition and register the original MPC Video Renderer again.

---

## About This Edition

### Why I Made This

I wanted to use DLSS frame generation for video playback while keeping the player and renderer I already use.

### Frame Generation

To keep things simple, MFG **automatically adjusts the frame generation multiplier** based on the video frame rate and display refresh rate.

The goal is to generate enough frames for smoother playback without unnecessary GPU work.

### Frame Timing

Generating more frames is not enough. **The original frames must stay in order**, and all frames must appear at the right time.

The renderer uses **presentation timestamps (PTS)** to track video timing. It can also estimate the frame rate when the source frame rate changes.

Good frame timing helps reduce stuttering, uneven motion, and unnecessary frame drops.

### GPU Performance

I develop and test this edition on an **NVIDIA GeForce RTX 4060 Ti 16GB**.

MFG uses GPU resources to generate extra frames. Higher resolutions and more generated frames can increase GPU usage.

I tested different ways to reduce GPU usage while keeping playback smooth.

### Image Quality

The **Super resolution** menu lets you choose RTX Video Super Resolution or **DLSS SR**.

The local build (`build-x64.cmd`) puts the [NVIDIA DLSS SR runtime](https://github.com/NVIDIA/DLSS/releases/tag/v310.9.16) in `dist/dlss/nvngx_dlss.dll`. Choose Direct3D 11 and enable video processor resizing.

DLSS SR works on progressive 8-bit SDR video when the output is larger than the source. It runs before MFG when MFG is enabled. If DLSS SR is unavailable, playback uses regular scaling.

MFG and RTX VSR can work together, but **the processing order affects image quality and GPU usage**.

**Quality — VSR before MFG**

VSR improves the original frames before MFG generates extra frames. This gives MFG clearer images to work with and may reduce artifacts in generated frames.

*However, this method usually uses more GPU resources.*

**Performance — MFG before VSR**

MFG generates extra frames first, then VSR improves the result. When the source resolution is low, MFG can work with smaller images and may use fewer GPU resources.

*However, generated frames may have more artifacts. The performance benefit also depends on the source resolution.*

Both methods have their advantages. The better choice depends on the video and GPU performance.

---

[License](LICENSE.md)
