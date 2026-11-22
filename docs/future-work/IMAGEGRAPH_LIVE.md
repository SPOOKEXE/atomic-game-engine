# live ImageGraph

The v2 parser, per-world graph owner, Luau API, render cache and Studio paths are implemented. Signed headless Vulkan consumer integration, focused GPU tests, an optimized benchmark and real Studio checks have passed. Targeted ImageLabel color, portal ownership and adopted-texture checks also pass. Historical strict CI on `7d6c3c0e` passed 661 suites; current strict CI passed 664 suites with none skipped or failed. Current architecture covers 50 modules, 6 programs and 36 layered modules. Full dev and server CTest passed on the earlier implementation snapshot. See [IMAGEGRAPH_LIVE_PLAN.md](IMAGEGRAPH_LIVE_PLAN.md) for evidence and remaining gates. The static workflow is in [IMAGEGRAPH_2D.md](IMAGEGRAPH_2D.md).

Live graphs compose 2D images: twenty node kinds covering sources, transforms, generators, colour and maths, blends, filters and mesh attribute rasterization, with named inputs and ordinary image outputs. A graph comes from signed content or from a script at runtime. They do not add 3D, audio or broad simulation features.

## author a live graph

Version 1 remains the static format. Its unextended writes stay v1. Version 2 adds named parameters, node-property bindings, source interpretation and output space. The writer uses v2 when those live fields are present.

The exact v2 document shape is:

```json
{
  "version": 2,
  "nodes": [
    {
      "id": "tile",
      "kind": "image.solid",
      "inputs": [],
      "position": [0, 0],
      "properties": {"width": 64, "height": 64, "colour": [255, 128, 32, 255]}
    },
    {
      "id": "spin",
      "kind": "image.transform",
      "inputs": ["tile"],
      "position": [220, 0],
      "properties": {
        "width": 64, "height": 64,
        "translate_x": 0, "translate_y": 0,
        "scale_x": 1, "scale_y": 1, "degrees": 0,
        "pivot_x": 32, "pivot_y": 32, "filter": "nearest"
      }
    }
  ],
  "outputs": [{"name": "image", "node": "spin", "space": "srgb"}],
  "parameters": [{"name": "angle", "type": "number", "default": 0}],
  "bindings": [{"node": "spin", "property": "degrees", "input": "angle"}]
}
```

Every node has exactly `id`, `kind`, `inputs`, `position` and `properties`. Every output has `name`, `node` and `space`. Every parameter has `name`, `type` and `default`. Every binding has `node`, `property` and `input`. Version 2 requires all five root keys: `version`, `nodes`, `outputs`, `parameters` and `bindings`. Unknown or missing fields fail. Version 1 keeps its original root, node and output fields.

## nodes

Inputs connect positionally in the order listed. Optional inputs trail the required ones, and a later optional input needs every earlier one. Generators fill their own `width` by `height` extent. Every other node takes the first input's extent, and nodes marked "same size" need every connected input to match it. Values below are stored channels in `0..1`; results clamp to that range.

| Kind | Inputs | Properties and result |
|---|---|---|
| `image.source` | none | `path`, `interpretation`. See the source rules below. |
| `image.solid` | none | `width`, `height`, `colour`. |
| `image.resize` | Image | `width`, `height`, `filter`. |
| `image.crop` | Image | `x`, `y`, `width`, `height`. |
| `image.transform` | Image | `width`, `height`, `translate_x`, `translate_y`, `scale_x`, `scale_y`, `degrees`, `pivot_x`, `pivot_y`, `filter`. |
| `image.flip` | Image | `horizontal`, `vertical`. |
| `image.blend` | Background, Foreground, optional Mask (same size) | `opacity`, `mode`: `normal`, `multiply`, `screen`, `overlay`, `add`, `subtract`, `soft_light`, `difference`. The Mask red channel scales the opacity per pixel. Modes composite with straight alpha; `normal` without a mask is the v2 source-over. `mode` is omitted from the JSON when it is `normal`. |
| `image.noise` | optional Vector | `width`, `height`, `type` (`value`, `gradient`), `scale` (cells per unit), `octaves` 1 to 8, `lacunarity`, `gain`, `seed`. Fractal noise in grey with opaque alpha. |
| `image.voronoi` | optional Vector | `width`, `height`, `scale`, `randomness` 0 to 1, `feature` (`f1`, `f2`, `f2_f1`, `edge`, `cell`), `seed`. Distances are in cell units; `cell` is a random value per cell. |
| `image.gradient` | optional Vector | `width`, `height`, `type` (`linear`, `radial`), `start_x`, `start_y`, `end_x`, `end_y` in normalized position. |
| `image.wave` | optional Vector | `width`, `height`, `type` (`bands`, `rings`), `profile` (`sine`, `saw`, `triangle`), `scale`, `degrees`, `phase`, `distortion`, `detail_scale`, `detail` 1 to 8, `seed`. Distortion adds value noise in periods. |
| `image.ramp` | Image | `factor` (`red`, `green`, `blue`, `alpha`, `luminance`), `interpolation` (`linear`, `constant`, `smooth`), `stops`: 1 to 16 ascending `{"position", "colour"}` entries. |
| `image.levels` | Image | `input_minimum`, `input_maximum`, `output_minimum`, `output_maximum`, `gamma`, `clamp`, `channels`. Maps the input range to the output range, with t to the power 1/gamma. |
| `image.math` | A, optional B (same size) | `operation` (`add`, `subtract`, `multiply`, `divide`, `minimum`, `maximum`, `power`), `value` (four constants used without B), `channels`. Division by zero writes zero. |
| `image.channels` | First, optional Second, Third, Fourth (same size) | `picks`: four entries for red, green, blue and alpha, each `N.c` (input N, channel `r`, `g`, `b` or `a`), `zero` or `one`. One input with picks such as `0.g 0.g 0.g one` splits; several inputs merge. |
| `image.invert` | Image | `channels`. One minus the value. |
| `image.blur` | Image | `radius` 0 to 64 pixels. Separable gaussian of premultiplied colour with clamped edges; sigma is radius / 3. |
| `image.normal` | Height | `strength`, `height` (channel), `flip_y`. Tangent-space normal from central differences, encoded as 0.5 + 0.5n with opaque alpha. Green points up; `flip_y` gives DirectX-style maps. |
| `image.warp` | Image, Vector (same size) | `strength` in pixels, `filter`. Offsets each pixel by the Vector red and green, where 0.5 is no offset. |
| `image.mesh_attribute` | none | `width`, `height`, `mesh`, `attribute` 0 to 3, `components`, `tile_u`, `tile_v`, `minimum`, `maximum`, `padding` 0 to 16. See mesh attributes below. |

`channels` is text naming one or more of `rgba`, such as `rgb`. Unselected channels pass through. A generator's optional Vector input replaces each pixel's normalized position with that input's red and green channels, read at the same normalized position. This is how a baked region UV field drives a pattern across UDIM seams. Noise, voronoi and wave hash integer lattice cells with the seed, so the same document, sources and seed always produce the same bytes on one platform.

Every property can be bound to a parameter except a source `interpretation`. Numbers bind to `number` parameters, flags to `boolean`, colours to `colour`, and every other property to `string`, using its text spelling: `0:000000ff 1:ffffffff` for ramp stops, `1 1 1 1` for a math value, `rgb` for channels and `0.r 0.g 0.b 0.a` for picks. Integer properties refuse a non-integral number. Source `interpretation` and output `space` are structural fields and cannot be bound. A colour binding replaces the whole RGBA8 value; there is no `colour.a` binding.

Parameter types are `number`, `boolean`, `colour` and `string`. A colour default is four integer channels from 0 to 255. Parameter names, binding input names and instance keys use portable tokens: 1 to 128 ASCII characters from letters, digits, `_`, `-` and `.`, except `.` and `..` alone. Static node and output names keep their wider v1 rules; live output references and live cooking require portable output tokens, with a diagnostic for a name that does not fit.

Source interpretation is `colour` or `data`. Colour sources normalize to encoded sRGB bytes. Data sources preserve raw channel bytes. Graph math operates on stored channels and quantizes each intermediate to the document precision. Output `space` is `srgb` or `linear`; it declares the intended meaning of the bytes and does not convert them. Choose `data` and `linear` for numeric material maps.

## precision and output formats

Version 3 adds a root `precision` and an output `format`, and requires both:

```json
{
  "version": 3,
  "precision": "wide",
  "nodes": [...],
  "outputs": [{"name": "height", "node": "blur", "space": "linear", "format": "r16f"}],
  "parameters": [],
  "bindings": []
}
```

`precision` is `byte` or `wide`. Byte stores every intermediate in 8 bits per channel, the v1 and v2 contract. Wide stores 16 bits per channel, so smooth falloffs in height and mask graphs do not band. Byte results are identical in both modes. `format` is `rgba8`, `r16f` (the red channel as one IEEE binary16 value) or `rgba16f`. The writer uses version 3 only when a document is wide or has a non-RGBA8 output, so earlier documents keep their encoding. Wide intermediates count eight bytes per pixel against the retained budget.

## author a graph from a script

A plugin or game script can author a graph at runtime. No cook or publish step is needed:

```lua
local graph = Instance.new("ImageGraph")
graph.Name = "Skin"
graph.InstanceKey = "skin"
graph.Parent = workspace
graph:SetGraphDocument(json) -- a v1, v2 or v3 document as text
part.TextureID = graph:GetImage("albedo")
```

`SetGraphDocument` refuses the document at the call with the reader, input, source and output checks that signed content passes through, and raises the diagnostic. Unlike signed content, a script document may name world-local `editable-image://` sources and `editable-mesh://` meshes. `nil` clears it, and `Graph` names the asset again. `GetGraphDocument` returns the stored text. The document is saved and replicated with the instance as its `GraphDocument` property, up to 1 MiB. Studio edit evaluates it without a content origin, and so does a client without one.

## mesh attributes

`image.mesh_attribute` rasterizes one float4 attribute channel of an `EditableMesh` into a UV tile, so Body Space data such as region, region UV, landmark distance or tissue becomes an image input. `mesh` is the mesh's `ContentId`, such as `editable-mesh://42`. `components` is one to three of `xyzw` written to red, green and blue; a single component fills all three. Values map from `minimum` and `maximum` to `0..1`. UV u in `[tile_u, tile_u + 1)` and v in `[tile_v, tile_v + 1)` map onto the tile, so UDIM 1001 + u + 10v is `tile_u = u`, `tile_v = v`. Pixel centres inside a triangle take its interpolated value; uncovered pixels are transparent, and `padding` grows covered edges outwards by that many pixels. Tiles go up to 4096 pixels.

Hosts copy the mesh, rasterize on the CPU and upload the tile as a data source before GPU evaluation. The uploaded name hashes the node, the mesh and its revision, so an unchanged mesh is rasterized once. Signed content may contain mesh attribute nodes only when their `mesh` is bound to an input, since a mesh is world-local.

## node editor for plugins

A plugin dock widget can host the Image Composer node editor over a document with `widget:SetImageGraph(json)`. Each valid edit fires `widget.ImageGraphChanged(json)`. See [Studio plugins](../engine/STUDIO_PLUGINS.md#image-graph-editor).


## cook and publish

Use a project-relative source such as `effects/fire.imagegraph`. Runtime graph asset names end in `.aimagegraph` and use portable relative paths: no spaces, control bytes, non-ASCII, `%`, `?`, `#`, backslashes, colons or `.` / `..` path segments. Cook the graph and its exact normalized `.atex` source closure:

```console
.cache/build/dev/tools/assetc --live-imagegraphs --input art --output baked --only effects/fire.imagegraph
```

`--live-imagegraphs` keeps the graph live as `.aimagegraph`; `--only SOURCE` selects one source and its live graph texture closure. `--input` and `--output` are required. Publish the cooked graph and source textures through the existing signed content flow. The ordinary static bake remains the path when a single `.atex` image is wanted.

Live graph sources resolve to exact `.atex` assets after cooking. The graph asset is opaque signed content. The runtime admits verified graph content before parsing it. Graphs cannot recursively use another graph as a source.

## use a graph in a world

Create a world-owned `ImageGraph` instance with a unique stable key. Set `Graph` to the cooked asset name, then parent the instance into the world:

```lua
local graph = Instance.new("ImageGraph")
graph.Name = "FireImage"
graph.InstanceKey = "fire"
graph.Graph = "effects/fire.aimagegraph"
graph.Output = "image"
graph.Parent = workspace

local image = graph:GetImage("image")
```

Assign `image` to an ordinary image slot. The stable reference is `imagegraph-instance://fire#image`; the world supplies the scope. A direct asset output uses `imagegraph://effects/fire.aimagegraph#image`.

The graph instance reference can be used anywhere the engine accepts a texture
content name. Common slots include `ImageLabel.Image`, `ImageButton.Image`,
`MeshPart.TextureID`, `Texture.Texture` and `ParticleEmitter.Texture`. In Studio,
the **Image slot** list contains writable texture properties shared by the
current selection. **Publish live graph** cooks and publishes the graph and its
normalized source textures through signed content. **Apply to selection** then
creates a world-owned `ImageGraph` and assigns its selected output reference to
that property on each selected instance. The live output remains evaluated at
runtime; static export instead bakes one ordinary `.atex` texture.

`SetInput` changes an authored parameter override. `GetInput` returns that explicit override, or `nil` when the graph default is in use. Passing `nil` to `SetInput` resets the override. Inputs accept numbers, booleans, `Color3` and strings. `Color3` becomes encoded RGBA8 with alpha 255. For example, update the transform binding from a heartbeat:

```lua
local RunService = game:GetService("RunService")
local angle = 0
RunService.Heartbeat:Connect(function(delta)
    angle = angle + delta
    graph:SetInput("angle", math.sin(angle) * 20)
end)
```

An unchanged value does not advance the instance revision. A real change dirties the bound node and its downstream image cone. Do not serialize an ECS entity ID as graph identity. Use the stable graph asset name and instance key.

Set inputs in the authoritative world. For a replicated controller, use a server script; replica worlds refuse changes to graph inputs.

## limits

Documents are limited to 1 MiB and 256 nodes, images to 4096 pixels per axis. One evaluation retains at most 1 GiB of intermediates and does at most 2^30 pixels of declared work; a node's work is its pixel count times a per-kind factor, such as two passes of `2 ceil(radius) + 1` taps for a blur or four per octave for noise. Refusals name the node.

## GPU evaluation

The renderer runs every kind except `image.mesh_attribute`, which hosts lower to a data source first. Byte graphs use RGBA8 intermediates and wide graphs RGBA16 UNORM; a blur also keeps a wide row-pass texture. Wide precision and blur need RGBA16 storage images and are refused on a device without them. R16F and RGBA16F outputs publish as `R16_FLOAT` and `RGBA16_FLOAT` textures. The GPU uses float maths, so generator and distance results can differ from the CPU evaluator by a step at cell and lattice boundaries; each path is deterministic on its own.

`just imagegraph-anatomy-bench` cooks a 2048 by 2048, 20-node skin detail graph (region UV, voronoi, ramp, noise, rings, levels, maths, masked blend modes, blur, normal, warp, channels and gradient) and prints completed GPU timestamps. On 2026-11-22 with the `bench` preset on Vulkan, paced at `MONO_GPU_BUDGET_PERCENT=25` with other desktop work active, 11 samples took 13.3 to 33.1 ms of GPU time, median 17.7 ms, with 0.2 to 0.4 ms of CPU recording. Each sample re-seeds every generator; 18 dispatches ran, since the source and the unseeded solid stay resident. This is under the 50 ms target, measured on one machine.

## Studio status

Studio provides parameter and binding controls, GPU preview, `PublishLive` and `Apply` into ordinary image slots. Real-editor checks passed graph sizing, v2 save/open, diagnostics, last-good behavior, missing-key refusal, two undock/resnap cycles, three consecutive Play/Stop cycles and the final native viewport drag/resnap. The restored orange output appeared in both ImageLabel and ParticleEmitter. An isolated task store allowed successful signed publish/apply checks without changing the default user store. A mismatched signing key was refused without writes. The historical 661-suite result remains on `7d6c3c0e`; current strict CI passed 664 suites. Full dev and server CTest passed on the earlier implementation snapshot. See the live plan for measured results and verification limits.
