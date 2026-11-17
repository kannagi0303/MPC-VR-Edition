# ZipDepth Model Converter

Ready-to-use packs are available in [ZipDepth Models](../models/README.md).

`zipdepth-convert.exe` turns an ONNX model into a `.dmlpack` file<br>
for MPC Video Renderer Edition.

- **Build**<br>
  Run [`build-x64.cmd`](../build-x64.cmd) to create `tools/zipdepth-convert.exe`.
- **Requirements**<br>
  Use Windows x64 and a Direct3D 12 GPU that supports the bundled DirectML runtime.

## Model Requirements

- **File**<br>
  Use one `.onnx` file with the weights inside it.
- **Tensors**<br>
  Use fixed NCHW shapes and float32 for both tensors.

| Tensor | Name | 384×384 | 512×288 |
| --- | --- | --- | --- |
| Input | `image` | `[1, 3, 384, 384]` | `[1, 3, 288, 512]` |
| Output | `depth` | `[1, 1, 384, 384]` | `[1, 1, 288, 512]` |

Use the same size for input and output.<br>
The 512×288 pack needs a renderer build with widescreen model support.

The renderer sends RGB values from 0 to 1.<br>
Put any extra input normalization inside the model.

The renderer resizes the image to fill the model input.<br>
For a 512×288 model, it rotates portrait video 90° for analysis<br>
and restores the source orientation when it uses the depth map.

DirectML must run the whole model as one graph.<br>
The model can use mixed precision internally if DirectML supports it.<br>
The converter checks the output against the ONNX model before saving.

## Convert

1. Copy `tools\zipdepth-convert.exe` into the playback package's `edition` folder.<br>
   Put it next to `DirectML.dll`.
2. Open PowerShell in the playback package folder.<br>
   Make sure the output folder already exists.
3. Run the converter.

   ```powershell
   .\edition\zipdepth-convert.exe "C:\Models\my-model.onnx" "C:\Models\my-model.dmlpack"
   ```

## Use a Model Pack

If you already have a compatible `.dmlpack`, start here.<br>
The renderer detects the model size from the pack.

1. Close the player and back up `edition\zipdepth.dmlpack`.
2. Copy the pack you want to `edition\zipdepth.dmlpack`.<br>
   Keep the active file name exactly `zipdepth.dmlpack`.
3. Open the player and turn on **ZipDepth for SR / DLAA** or **ZipDepth for MFG**<br>
   in **Edition Extended**.

To restore the default model, close the player and put the backup back.

> **Important**
>
> A build recreates `dist/`.<br>
> Keep your custom model and backup outside that folder.

## License and Source

Run `zipdepth-convert.exe --licenses` to see the license and third-party notices.<br>
The converter includes ONNX Runtime and Microsoft Visual C++ runtime files.

- [Project license](../.github/LICENSE.md)
- [Converter source](../build-support/zipdepth/)
- [Microsoft Visual C++ redistribution terms](https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files)
