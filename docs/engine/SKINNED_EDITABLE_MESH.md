# Skinned editable meshes

A script can build a skinned mesh, bind it to a `Bone` tree it made, and
rebuild either one while the game runs. It can also read a received mesh asset
back into buffers.

```luau
local read = ContentService:ReadMesh("characters/body.amesh")

local rig = Instance.new("SkinnedMeshPart")
rig.Parent = workspace
local bones = {}
for index, joint in read.Skeleton do
	local bone = Instance.new("Bone")
	bone.Name = joint.Name
	bone.RestCFrame = joint.Rest
	bone.InverseBindCFrame = joint.InverseBind
	bone.Parent = if joint.Parent >= 0 then bones[joint.Parent + 1] else rig
	bones[index] = bone
end
rig:SetBones(bones)

local mesh = Instance.new("EditableMesh")
mesh.Parent = rig
mesh:SetGeometryFromBuffer(read.Layout, read.Vertices, read.Indices)
rig.MeshId = mesh.ContentId
```

An `Animator` under the rig plays clips against these bones as it does for an
imported rig.

## Skin on an EditableMesh

`EditableMesh:SetVertexSkin(vertex, joints, weights)` sets up to four
influences. Joints are palette slots from zero, below 256. Weights must be
finite and not negative. They do not have to sum to one: the renderer
normalises each vertex. A vertex whose weights are all zero is drawn unskinned.
`GetVertexSkin(vertex)` reads them back.

`SetGeometry` vertex records take `Joints` and `Weights` arrays with the same
rules. A mesh with no skin stores no skin arrays.

## Packed buffers

`EditableMesh:SetGeometryFromBuffer(layout, vertices, indices)` replaces the
whole mesh from little-endian bytes. In Luau the buffers are `buffer`; in
JavaScript they are an `ArrayBuffer` or any typed array. Like `SetGeometry`, it
yields until the commit, and a later edit to the same mesh makes it return
`false`.

| Layout field | Type | Element |
|---|---|---|
| `VertexCount` | required | |
| `IndexFormat` | `"u16"` or `"u32"`, default `"u32"` | |
| `Stride` | bytes, default tightly packed | |
| `Position` | required | f32 x3 |
| `Normal`, `Color` | optional | f32 x3 |
| `UV` | optional | f32 x2 |
| `Alpha` | optional | f32 |
| `Joints` | optional, needs `Weights` | u16 x4 |
| `Weights` | optional, needs `Joints` | f32 x4 |
| `Attribute0` to `Attribute3` | optional | f32 x4 |

A field is a byte offset that uses `Stride`, or `{ Offset, Stride }`, so one
interleaved buffer and consecutive blocks both work. A missing field takes the
value `AddVertex` gives. A refused layout raises an error that names the field.
Limits are 4,194,304 vertices and 16,777,216 indices, the same ceilings as the
mesh asset format.

`just terrain-collision-build-bench` runs the `engine.scene.bench.editablemesh`
suite, which measures 25,000 skinned vertices from one buffer through decode, validation and
commit. The budget is 10 ms.

## Binding bones

`SkinnedMeshPart:SetBones(bones)` makes bone `i` palette slot `i - 1`, and sets
the rig's `JointCount`. The list must name every `Bone` under the rig once,
with each parent before its children. A refusal changes nothing.

The palette is `TransformedWorldCFrame * InverseBindCFrame` for each bone,
rebuilt every presented frame. Edits to `InverseBindCFrame`, `RestCFrame`,
`Transform` or the mesh positions show on the next frame without reimporting
anything. The mesh may name fewer slots than the rig has.

## ReadMesh

`ContentService:ReadMesh(meshId)` returns what this world has received, or
`nil`. A headless server has no content and always returns `nil`. Name the mesh
on a `MeshPart` to fetch it.

| Field | |
|---|---|
| `VertexCount`, `IndexCount`, `JointCount` | numbers |
| `Vertices` | buffer of consecutive blocks: positions, normals, UVs, then joints and weights when skinned |
| `Layout` | the `SetGeometryFromBuffer` layout of `Vertices` |
| `Indices` | buffer of u32 |
| `Submeshes` | `{ FirstIndex, IndexCount }` per material run |
| `Skeleton` | `{ Name, Parent, Rest, InverseBind }` per palette slot |

`Parent` is a palette slot from zero, and `-1` for a root. It is always lower
than the joint's own slot, so the list is in the order `SetBones` takes.
Weights come back as floats that sum to one.

The glTF importer stores the skin's joint names, parents, rest frames and
inverse bind matrices in the mesh asset. It orders joints parents first and
remaps the vertex joint indices to match. Scale in either frame is dropped.
Meshes baked before this format version have an empty `Skeleton`.

## Attribute channels

`SetVertexAttribute(vertex, channel, values)` and the `Attribute0` to
`Attribute3` layout fields store up to four float4 channels per vertex. They
are saved, snapshotted and replicated with the mesh. The renderer does not
upload them yet, so a `ShaderScript` cannot read them. `docs/DEFERRED.md`
D00138 records what that needs.
