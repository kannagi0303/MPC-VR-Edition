# ZipDepth Models

| Model | Download | Playback ZIP |
| --- | --- | --- |
| 384×384 mixed FP16 | [zipdepth.dmlpack](https://raw.githubusercontent.com/kannagi0303/MPC-VR-Edition/main/models/zipdepth_384x384_mixed_fp16/zipdepth.dmlpack) | Included |
| 512×288 FP32 | [zipdepth.dmlpack](https://raw.githubusercontent.com/kannagi0303/MPC-VR-Edition/main/models/zipdepth_512x288_fp32/zipdepth.dmlpack) | Separate download |

## Change the Model

1. Close the player.
2. Download the model you want.
3. Replace `edition/zipdepth.dmlpack` with the downloaded file.
4. Open the player again.

Keep the name `zipdepth.dmlpack`.
The renderer detects both model sizes.

## Source and License

Both packs come from the Nightfall EdgePad models based on Fabio Tosi's ZipDepth.
The 512×288 pack uses the ONNX model from Nightfall Meteor.
The 384×384 pack uses the mixed FP16 ONNX model in this repository.

- [Nightfall model release](https://github.com/tB0nE/nightfall/releases/tag/edgepad-models-2026-10-08)
- [MIT license](LICENSE.txt)
- [ZipDepth converter guide](../tools/zipdepth-convert.md)
