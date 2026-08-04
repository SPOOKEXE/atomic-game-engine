--!strict
-- A mirror, and what it takes to make one.
--
-- `Rings.luau` proves the *loading* path — a script builds a world through the
-- class table and animates it on `RunService.Heartbeat`. This proves the
-- **rendering** one: shadows, a second camera that draws into a texture, and a
-- surface that shows it.
--
-- ## How a mirror works here, in three lines of setup
--
-- A `Camera` with a `SurfaceSize` renders the world into a texture instead of
-- onto the screen. A `Part` with a `Surface` of 0 samples that texture,
-- projected from where the camera was standing. Point the camera at the mirror
-- from the reflected position and what the mirror shows is what is behind you.
--
-- **Three lines is now two.** Parent a `SurfaceCamera` to a part, pick a face,
-- and the engine does the reflection — see `scene/SurfaceCameras.hpp`. What
-- follows keeps the geometry it always had and has lost the vector maths.
--
-- **It is a frame stale, and that is the design rather than a limitation.** The
-- surface is rendered before the main pass and shows the world as it was on the
-- previous frame — which is what breaks the dependency cycle between a mirror
-- and the scene it reflects, and exactly the latency `world::ViewChannel`
-- assumed when it was built at v0.2.
--
-- ## What it is not
--
-- The projection is **planar**: right for a flat pane, wrong for anything
-- curved. A general reflection needs a cube map or a screen-space trace and
-- neither belongs in a pipeline this size. The narrowness is deliberate — this
-- is one mirror rendered correctly rather than a reflection system rendered
-- approximately.

local RunService = game:GetService("RunService")

-- The mirror plane. Flat, facing +Z, standing at the origin.
local MIRROR_WIDTH = 16
local MIRROR_HEIGHT = 9
local MIRROR_Z = -6

-- Where the viewer stands. **Only the viewer's position now** — the reflection
-- is worked out from wherever this camera is, every frame, so moving it moves
-- the image with it instead of leaving the mirror correct for one spot.
local EYE = Vector3.new(0, 5, 22)

local mirror = Instance.new("Part")
mirror.Name = "Mirror"
mirror.Anchored = true
mirror.Size = Vector3.new(MIRROR_WIDTH, MIRROR_HEIGHT, 0.4)
mirror.Position = Vector3.new(0, MIRROR_HEIGHT / 2, MIRROR_Z)
mirror.Color = Color3.new(1, 1, 1)

-- The tint still multiplies whatever the reflection carries, so a coloured
-- mirror is a colour here and a plain one is white.
--
-- **No `Surface` line.** Parenting a `SurfaceCamera` to this part is what tells
-- it which texture to show — see the camera below.
mirror.Parent = workspace

-- A frame around it, so the pane reads as a mirror rather than as a hole.
local frameThickness = 0.5
for _, offset in
	{
		{ Vector3.new(0, MIRROR_HEIGHT + frameThickness / 2, MIRROR_Z), Vector3.new(MIRROR_WIDTH + 1, frameThickness, 0.6) },
		{ Vector3.new(0, -frameThickness / 2, MIRROR_Z), Vector3.new(MIRROR_WIDTH + 1, frameThickness, 0.6) },
		{ Vector3.new(-(MIRROR_WIDTH + frameThickness) / 2, MIRROR_HEIGHT / 2, MIRROR_Z), Vector3.new(frameThickness, MIRROR_HEIGHT + 1, 0.6) },
		{ Vector3.new((MIRROR_WIDTH + frameThickness) / 2, MIRROR_HEIGHT / 2, MIRROR_Z), Vector3.new(frameThickness, MIRROR_HEIGHT + 1, 0.6) },
	}
do
	local bar = Instance.new("Part")
	bar.Name = "Frame"
	bar.Anchored = true
	bar.Position = offset[1]
	bar.Size = offset[2]
	bar.Color = Color3.fromRGB(60, 50, 40)
	bar.Parent = workspace
end

-- The floor, which is what makes a shadow legible. A shadow needs something to
-- fall on, and a scene of floating cubes has nothing.
local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = Vector3.new(60, 0.5, 60)
floor.Position = Vector3.new(0, -0.25, 0)
floor.Color = Color3.fromRGB(120, 120, 125)
floor.Parent = workspace

-- The things that cast, and that the mirror shows. Deterministic placement, for
-- the reason `Rings.luau` gives: a shared generator makes the scene depend on
-- the order things were created in.
local function hashed(index: number, salt: number): number
	local value = bit32.bxor(index * 0x9E3779B1, salt * 0x85EBCA77)
	value = bit32.bxor(value, bit32.rshift(value, 15))
	value = (value * 0x2545F491) % 0x100000000
	value = bit32.bxor(value, bit32.rshift(value, 13))
	return (value % 100000) / 100000
end

local function range(index: number, salt: number, low: number, high: number): number
	return low + hashed(index, salt) * (high - low)
end

type Caster = { Part: Part, Base: Vector3, Phase: number, Rate: number }
local casters: { Caster } = {}

for index = 0, 23 do
	local part = Instance.new("Part")
	part.Name = "Caster"
	part.Anchored = true

	local edge = range(index, 31, 0.8, 2.0)
	part.Size = Vector3.new(edge, edge, edge)
	part.Color = Color3.fromRGB(range(index, 19, 60, 240), range(index, 23, 70, 210), range(index, 29, 90, 250))

	local base = Vector3.new(range(index, 3, -12, 12), range(index, 5, 1.5, 7), range(index, 7, -2, 9))
	part.Position = base
	part.Parent = workspace

	table.insert(casters, {
		Part = part,
		Base = base,
		Phase = range(index, 11, 0, 6.283185),
		Rate = range(index, 13, 0.4, 1.3),
	})
end

-- **The surface camera, as one instance parented to the pane.** Everything
-- below used to be arithmetic in this file: mirror the eye through the plane,
-- aim back at the pane, push the near plane out to the glass. All of it is
-- `scene::AimSurfaceCameras` now, run every frame from wherever the viewer
-- actually is.
--
-- That last part is the difference that matters. The hand-written version was
-- correct only for the frame it was written for, which is why this example used
-- to pin its viewer in place: a moving eye made the reflection wrong, and the
-- mirror looked broken rather than fixed to one position.
--
-- `Face` picks which side of the pane it projects off. `Enum.NormalId.Front` is
-- -Z, which is the side the pane's own front faces.
local reflection = Instance.new("SurfaceCamera")
reflection.Name = "Reflection"
reflection.SurfaceSize = Vector3.new(1024, 1024)
reflection.Face = "Front"
reflection.FieldOfView = 70

-- **The image's own opacity, which is not the pane's.** At 0 the reflection is
-- solid whatever the pane's `Transparency` is — so a sheet of glass you can see
-- through still carries a mirror image, which is what glass does and what one
-- shared number could never express. Raise it towards 1 and the reflection
-- fades while the pane stays exactly as transparent as it was.
reflection.ImageTransparency = 0

-- Parenting is the whole of the setup: the engine places the camera off this
-- face and tells the pane which surface to show, so there is no `mirror.Surface`
-- line here any more. Setting it by hand as well would be one fact recorded
-- twice, and forgetting it rendered a perfect reflection into a texture nothing
-- sampled.
reflection.Parent = mirror

-- The camera the scene is *watched* through. Not the surface one: a world has
-- several cameras and `workspace.CurrentCamera` names the live one, which is
-- exactly the distinction `ActiveCamera` exists for.
local view = Instance.new("Camera")
view.Name = "Viewer"
view.CFrame = CFrame.new(EYE)
view.FieldOfView = 70
view.Parent = workspace
workspace.CurrentCamera = view

-- The casters bob, so the shadows move and the reflection moves with them.
-- Motion is what makes both legible: a still shadow reads as painted-on
-- geometry, and a still reflection reads as a texture.
RunService.Heartbeat:Connect(function(deltaTime: number)
	for _, caster in casters do
		caster.Phase += caster.Rate * deltaTime
		caster.Part.Position = caster.Base + Vector3.new(0, math.sin(caster.Phase) * 1.5, 0)
		caster.Part.CFrame *= CFrame.Angles(0, caster.Rate * deltaTime, 0)
	end
end)

print(`mirrors: {#casters} casters, one surface camera at 1024x1024`)
