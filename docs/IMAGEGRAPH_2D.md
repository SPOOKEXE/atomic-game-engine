# Static 2D ImageGraph

Image Composer creates static images from source images and basic 2D nodes.
A `.imagegraph` file stores the editable graph. Export produces an ordinary
`.atex` texture. Set `ParticleEmitter.Texture` or `ImageLabel.Image` to the
exported content name, such as `sprite.atex`, through the existing asset flow.
The game loads that texture without evaluating the graph. Graph outputs are
display images; using a `.imagegraph` project as a numeric material map, such as
a normal or roughness map, is refused.

## Studio

Open **View > Image Composer**. The initial graph contains a white Solid node
bound to the `image` output.

1. Add nodes with the palette buttons and connect their image sockets.
2. Select a node to edit its controls in the sidebar. Source paths are relative
   to the project directory. Keep the project beside or above its source images.
3. Enter an **Output name** and click **Bind selected node**. Choose the named
   output with **Preview output**.
4. Enter a `.imagegraph` path in the top field and click **Save**. **Open** loads
   that path. **Undo** and **Redo** restore graph edits and output bindings.
5. Click **Reload** after changing source image files on disk. Preview results
   and decoded sources are cached between edits.
6. Enter an **Asset** name, such as `sprite.atex`, and click **Export .atex**.
   This writes into Studio's configured baked content directory and registers
   the ordinary texture in its asset catalogue. **Publish assets** uses the
   existing Assets publication flow for the content origin.

Invalid edits show a diagnostic and retain the last successful preview. Export
refuses invalid edits instead of writing that older preview. Failed saves or
exports preserve the prior file. Saving into a different directory rewrites
relative source paths and refuses moves that would require escaping that directory.

## Nodes

Coordinates start at the top left. X increases rightward and Y downward.
Images use straight-alpha RGBA8 with encoded sRGB colour channels, using the
[standard sRGB transfer curve](https://registry.khronos.org/DataFormat/specs/1.4/dataformat.1.4.html#TRANSFER_SRGB). Sampling and
blending operate on those encoded channels. Linear `.atex` source RGB is converted
to encoded sRGB before composition; alpha stays linear. Single-channel textures
become opaque grayscale, and floating source values are clamped to the SDR range.

| Node | Controls and result |
|---|---|
| Source | Project-relative PNG, JPEG, BMP, GIF atlas, or ordinary `.atex`. SVG rasterization and recursive graph sources are refused. |
| Solid | Positive integer Width and Height, and RGBA Colour. |
| Resize | Explicit Width and Height; `nearest` or `bilinear` sampling. |
| Crop | Integer X and Y, and explicit Width and Height. Pixels outside the source are transparent. |
| Transform | Explicit output Width and Height; Translate X/Y in pixels; nonzero Scale X/Y; clockwise Degrees; Pivot X/Y in pixels; `nearest` or `bilinear`. Scale and rotation occur around the pivot, followed by translation. Pixels outside the source are transparent. |
| Flip | Horizontal and Vertical switches; dimensions stay unchanged. |
| Blend | Background and Foreground inputs of equal dimensions; foreground Opacity from 0 to 1; normal source-over composition. Use Resize or Transform to match canvases first. |

Named outputs reference node IDs. A single output is selected automatically;
projects with several outputs require a name when baking from the CLI.
Node IDs, kinds, source paths and output names are strings in the project.
Canvas positions affect the editor layout only.

## CLI example

Put `source.png` in `art/` and save this as `art/sprite.imagegraph`:

```json
{
  "version": 1,
  "nodes": [
    {
      "id": "source", "kind": "image.source", "inputs": [],
      "position": [0, 0], "properties": {"path": "source.png"}
    },
    {
      "id": "flip", "kind": "image.flip", "inputs": ["source"],
      "position": [240, 0],
      "properties": {"horizontal": true, "vertical": false}
    }
  ],
  "outputs": [{"name": "sprite", "node": "flip"}]
}
```

After building the `assetc` target with the `dev` preset, run:

```console
.cache/build/dev/tools/assetc --input art --output baked --imagegraph-output sprite --no-mipmaps
```

This bakes `baked/sprite.atex`, with the source image flipped horizontally.
The directory bake also imports `source.png` as `source.atex`. To preserve the
full authored output size, add `--max-texture 0`; the ordinary default cap is
2048 pixels on the longest axis. Omit `--no-mipmaps` to build the usual mip chain.
Publish the baked directory through the existing asset publication workflow
before referencing these names from a game using that content origin.

Project files are limited to 1 MiB. Dimensions are limited to 4096 pixels per
axis, each RGBA image and encoded source to 64 MiB, and retained graph results to
256 MiB. Evaluation also has a 64-million pixel-work budget. Source references
must be relative, with no parent segments, and stay inside the allowed source
root after resolving symlinks. A failed bake returns a nonzero CLI exit status
and preserves the previously exported graph texture.

The project schema accepts only the documented 2D node kinds and controls.
It does not load projects from the archived Composer implementation.
