# texpack: 2x textures with PBR maps

Makes 2x upscaled versions of the game's textures and builds a full PBR
(metal/roughness) map set for every material.

The work folder holds data made from the game files. Keep it outside this repository.

## Setup

- Python 3 with `numpy` and `Pillow`.
- [Real-ESRGAN ncnn Vulkan](https://github.com/xinntao/Real-ESRGAN/releases)
  (`realesrgan-ncnn-vulkan-20220424-windows.zip`), unpacked to
  `../tools/realesrgan/` next to the repo folder, or passed with `--esrgan`.

## Use

```
python texpack.py extract --game ../../original/v106 --out "../../../texpack_work"
python texpack.py build   --work "../../../texpack_work" [--filter bricks] [--limit 10]
```

`extract` decodes every 2D texture to `source/*.png` and writes
`source/index.json`, which records each texture's archive, format, size and the
SHA-1 of its `.res` entry.

`build` writes to `pack/`:

- `pack/<material>/`: `albedo`, `normal` (OpenGL, +Y), `roughness`, `metallic`,
  `ao`, `height`, `orm` (R = AO, G = roughness, B = metallic)
- `pack/_textures/`: UI, masks and effects, upscaled only
- `pack/manifest.json`: the source textures behind each output

## How the maps are made

Maps are grouped into materials by the Frostbite name suffix:

| Suffix | Role |
| --- | --- |
| `Df` `Dm` `Cm` `_c` `diffuse` `color` | colour |
| `Nm` `_n` `normal` | normal |
| `Sp` `spec` | specular |
| `Gm` | gloss |
| `Oc` `occlusion` `_ao` | AO |
| `Px` | parallax height |

Colour variants such as `Bricks_t03v02` borrow the base material's other maps.

- **Colour, specular, gloss, AO and masks** go through Real-ESRGAN x4plus, then
  Lanczos down to 2x. Edges wrap, so tiling textures stay seamless.
- **Normal maps** are resampled with Lanczos and renormalised, so no detail is
  invented. Fine detail from the upscaled albedo is then added on top. The
  green direction is detected from the albedo, and the output is OpenGL.
- **Height** comes from the game's `Px` map if there is one. Otherwise it is
  integrated from the normal map (Frankot-Chellappa), or estimated from the
  albedo as a last resort.
- **Roughness** comes from the `Gm` gloss map if there is one. Otherwise it is
  derived from the specular map, or estimated from the albedo.
- **Metallic** is an estimate. It is only non-zero for metal-named materials
  (guns, steel, engines...), where it follows the specular brightness.
- **AO** comes from the game's AO map, or is computed from the height.

## In-game use

```
python texpack.py install --work "../../../texpack_work" --game ../../original/v106 --out "../../../texpack_work/install"
bf1943 --textures "<...>/texpack_work/install"
```

`install` writes `rtex/*.rtex` (RGBA8 with a full mip chain) and
`replacements.txt`. Each line holds a hash of the first 4 KB of the texture's
tiled base level, the guest size and the file. The runtime hashes every
block-compressed 2D texture (DXT1/3/5, DXT5A, DXN) the game creates, and
uploads the match instead of the guest data, at its own size.

- Material albedo and normal maps are installed, plus the upscaled
  non-material textures. Menu/UI textures are left out unless `--ui` is given.
  `--no-normals` keeps the game's normal maps.
- Normal maps are converted back to the game's channel layout (DXT5nm: X in
  alpha) and green direction.
- For bf1943 `--pbr`, normal maps (NormalDXT1, NormalDXT5, DXN) are written
  as X and Y in red and green; blue holds the roughness in its upper half
  (128-255), or in its lower half where the material is metallic (over half;
  not metal keeps blue high as before, for shaders outside the --pbr table
  that take it as the normal's Z); alpha holds the height for parallax in
  191-253 (kept below 255 so the shader can tell pack maps from everything
  else, and near 1 for shaders that read alpha as a mask: the full 0-253
  speckled the first-person arms). Named `*.pbr.rtex`. The game rebuilds Z
  from X/Y; for DXT5nm the runtime swizzles X from red to where the game
  reads it (alpha). Metallic is kept off saturated albedo (wood on a gun).
  `--no-pbr` leaves them out. Spec, gloss and AO are not used.
- Everything is RGBA8, so the full install is about 3.8 GB on disk and uses
  more video memory than the original DXT textures.
