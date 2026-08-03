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

-- Where the viewer stands. The reflected camera is this position mirrored
-- through the plane, which is what makes the image line up with the pane.
local EYE = Vector3.new(0, 5, 22)

local mirror = Instance.new("Part")
mirror.Name = "Mirror"
mirror.Anchored = true
mirror.Size = Vector3.new(MIRROR_WIDTH, MIRROR_HEIGHT, 0.4)
mirror.Position = Vector3.new(0, MIRROR_HEIGHT / 2, MIRROR_Z)
mirror.Color = Color3.new(1, 1, 1)

-- **This is the whole of it.** Surface 0 is what the surface camera writes, and
-- a part that names it samples that texture instead of its own tint. The tint
-- still multiplies, so a coloured mirror is a colour here and a plain one is
-- white.
mirror.Surface = 0
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

-- **The surface camera.** Setting `SurfaceSize` is what turns an ordinary
-- camera into one that renders to a texture: the property is structural, so the
-- component appears and the client's `FindSurfaceCamera` starts finding it.
local reflection = Instance.new("Camera")
reflection.Name = "Reflection"
reflection.SurfaceSize = Vector3.new(1024, 1024)

-- Mirrored through the plane: the same distance behind it as the eye is in
-- front, looking the other way. That is the whole of planar reflection, and it
-- is why the projection lines up with the pane rather than sliding across it.
local reflected = Vector3.new(EYE.X, EYE.Y, 2 * MIRROR_Z - EYE.Z)

-- **Aimed, not just placed.** `CFrame.new` carries identity rotation, which
-- looks down -Z — so a camera put behind the mirror faces *away* from it and
-- renders empty space. That was the first version of this line, and the mirror
-- came out showing the clear colour.
reflection.CFrame = CFrame.lookAt(reflected, Vector3.new(0, MIRROR_HEIGHT / 2, MIRROR_Z))
reflection.FieldOfView = 70

-- **The near plane sits at the mirror**, which is the poor-man's oblique clip.
-- The reflection camera is behind the pane looking through it, so everything
-- between the two — the frame, the back of the pane, anything the viewer is
-- standing behind — would occlude the reflection. Clipping at the plane removes
-- exactly that half of the world.
--
-- A real planar reflection skews the projection's near plane onto the mirror
-- instead, which handles a camera looking at the pane from an angle. This is
-- the head-on case done correctly and the oblique case done approximately.
reflection.NearPlaneZ = math.abs(reflected.Z - MIRROR_Z) + 0.3
reflection.Parent = workspace

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
