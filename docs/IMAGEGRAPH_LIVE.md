# live ImageGraph

The v2 parser, per-world graph owner, Luau API, render cache and Studio paths are implemented. Signed headless Vulkan consumer integration, focused GPU tests, an optimized benchmark and real Studio checks have passed. Targeted ImageLabel color, portal ownership and adopted-texture checks also pass. Historical strict CI on `7d6c3c0e` passed 661 suites; current strict CI passed 664 suites with none skipped or failed. Current architecture covers 50 modules, 6 programs and 36 layered modules. Full dev and server CTest passed on the earlier implementation snapshot. See [IMAGEGRAPH_LIVE_PLAN.md](IMAGEGRAPH_LIVE_PLAN.md) for evidence and remaining gates. The static workflow is in [IMAGEGRAPH_2D.md](IMAGEGRAPH_2D.md).

Live graphs keep a small job: seven 2D image nodes, named inputs, and ordinary image outputs. They do not add 3D, audio or broad simulation features.

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

The seven node kinds are `image.source`, `image.solid`, `image.resize`, `image.crop`, `image.transform`, `image.flip` and `image.blend`. Their controls match the static graph guide. Bindable properties are:

- `source`: `path`
- `solid`: `width`, `height`, `colour`
- `resize`: `width`, `height`, `filter`
- `crop`: `x`, `y`, `width`, `height`
- `transform`: `width`, `height`, `translate_x`, `translate_y`, `scale_x`, `scale_y`, `degrees`, `pivot_x`, `pivot_y`, `filter`
- `flip`: `horizontal`, `vertical`
- `blend`: `opacity`

Source `interpretation` and output `space` are structural fields and cannot be bound. A colour binding replaces the whole RGBA8 value; there is no `colour.a` binding.

Parameter types are `number`, `boolean`, `colour` and `string`. A colour default is four integer channels from 0 to 255. Parameter names, binding input names and instance keys use portable tokens: 1 to 128 ASCII characters from letters, digits, `_`, `-` and `.`, except `.` and `..` alone. Static node and output names keep their wider v1 rules; live output references and live cooking require portable output tokens, with a diagnostic for a name that does not fit.

Source interpretation is `colour` or `data`. Colour sources normalize to encoded sRGB bytes. Data sources preserve raw channel bytes. Graph math operates on stored byte channels and quantizes each intermediate. Output `space` is `srgb` or `linear`; it declares the intended meaning of the bytes and does not convert them. Alpha remains a byte channel. Choose `data` and `linear` for numeric material maps.

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

## Studio status

Studio provides parameter and binding controls, GPU preview, `PublishLive` and `Apply` into ordinary image slots. Real-editor checks passed graph sizing, v2 save/open, diagnostics, last-good behavior, missing-key refusal, two undock/resnap cycles, three consecutive Play/Stop cycles and the final native viewport drag/resnap. The restored orange output appeared in both ImageLabel and ParticleEmitter. An isolated task store allowed successful signed publish/apply checks without changing the default user store. A mismatched signing key was refused without writes. The historical 661-suite result remains on `7d6c3c0e`; current strict CI passed 664 suites. Full dev and server CTest passed on the earlier implementation snapshot. See the live plan for measured results and verification limits.
