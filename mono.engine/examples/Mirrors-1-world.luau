--!strict
-- A room made of mirrors, and what it takes to make one.
--
-- `Rings.luau` proves the *loading* path - a script builds a world through the
-- class table and animates it on `RunService.Heartbeat`. This proves the
-- **rendering** one: shadows, cameras that draw into a texture, and surfaces
-- that show them.
--
-- ## How a mirror works here, in two lines of setup
--
-- Parent a `SurfaceCamera` to a part, pick a face, and the engine does the
-- reflection - see `scene/SurfaceCameras.hpp`. The camera renders the world
-- into a texture from the eye's position mirrored through that face, and the
-- part is told to sample it. There is no vector maths in this file.
--
-- **It is a frame stale, and that is the design rather than a limitation.** The
-- surface is rendered before the main pass and shows the world as it was on the
-- previous frame - which is what breaks the dependency cycle between a mirror
-- and the scene it reflects, and exactly the latency `world::ViewChannel`
-- assumed when it was built at v0.2.
--
-- ## Four walls, four surfaces, and a mirror inside a mirror
--
-- The baseplate is walled in on all four sides and every wall is authored as a
-- mirror facing the origin. **All four reflect**, and each one shows the other
-- three.
--
-- That took two things, both of which arrived at v0.8:
--
--   - a **surface slot per camera**, each owning a ping-ponged texture pair of
--     its own. Every camera used to share slot 0, so all four walls wrote into
--     one target and three of them projected the fourth's image across
--     themselves. `scene::AimSurfaceCameras` hands the slots out in entity
--     order and writes both ends of the pairing - the camera that renders into
--     a slot and the pane that samples it - so nothing in this file names one;
--   - a **per-view exclusion** in the surface pass. It used to skip every
--     mirror, because a mirror sits between its own reflection camera and the
--     world and would fill its texture with itself. It now skips only the
--     surface being rendered *for*, so the other three are drawn.
--
-- **What a mirror shows of another mirror is one frame old, and that is the
-- design rather than a limitation.** Each surface is being rendered for the
-- others, so there is no order in which any of them could have this frame's
-- image ready first. Each pass therefore samples the half of the pair it is not
-- writing - the frame before. A reflection of a reflection is two frames behind,
-- three is three, and the recursion fades out rather than terminating: at some
-- depth the bounce is older than anything on screen and stops being legible.
--
-- Stand between two facing walls and you can see it directly. The corridor of
-- images is finite, and the far end of it is the oldest frame the pair still
-- holds.
--
-- ## What else it is not
--
-- The projection is **planar**: right for a flat pane, wrong for anything
-- curved. A general reflection needs a cube map or a screen-space trace and
-- neither belongs in a pipeline this size.
--
-- The projection stays planar for the recursion too, so a mirror seen in a
-- mirror is a flat image of a flat image and not a second view frustum. That is
-- right for what this scene is and wrong the moment a pane is not flat.

local RunService = game:GetService("RunService")

-- The baseplate, and therefore the room. Everything below is placed off these
-- three numbers so the enclosure cannot drift out of agreement with the floor
-- it stands on.
local PLATE = 60
local PLATE_HALF = PLATE / 2

local WALL_HEIGHT = 20
local WALL_THICKNESS = 0.4

-- **Outside the plate, not on its edge.** The wall's centre sits half a
-- thickness beyond the baseplate, so its inward face lands exactly on the plate
-- boundary and the floor runs to the glass with no seam of background between
-- them.
local WALL_OFFSET = PLATE_HALF + WALL_THICKNESS / 2

-- How far from the middle the cubes are allowed to wander. Well inside the
-- walls: a caster clipping through a mirror reads as a broken reflection rather
-- than as a cube in the wrong place, and the two are hard to tell apart from the
-- viewer's chair.
local CASTER_SPREAD = 12

-- Where the viewer stands. **A position only** - the reflection is worked out
-- from wherever this camera is, every frame, so moving it moves the image with
-- it instead of leaving the mirror correct for one spot.
--
-- **Behind the cubes rather than among them**, which is why this is further out
-- than `CASTER_SPREAD` and not a round number chosen for looks: a caster is
-- placed anywhere in that square, so an eye inside it eventually has one spawn
-- in its face and fill the screen. Standing clear puts the cluster between the
-- viewer and the north wall, which is where a mirror demo wants it.
local EYE = Vector3.new(0, 7, 18)
local EYE_FOV = 60

-- The floor, which is what makes a shadow legible. A shadow needs something to
-- fall on, and a scene of floating cubes has nothing.
-- Clear of a baseplate's top face at y = 0: coplanar surfaces do not flicker in
-- this engine, they resolve stably to whichever one the draw order happens to
-- favour. `scripts/demos/zfight-scan.sh` holds the control for that.
local GROUND_LIFT = 0.01

local floor = Instance.new("Part")
floor.Name = "Baseplate"
floor.Anchored = true
floor.Size = Vector3.new(PLATE, 0.5, PLATE)
floor.Position = Vector3.new(0, -0.25 + GROUND_LIFT, 0)
floor.Color = Color3.fromRGB(120, 120, 125)
floor.Parent = workspace

-- **Axis-aligned, so there is no rotation anywhere in this file.** `Face` is
-- resolved in the part's *local* space and then rotated into the world, so an
-- unrotated wall with the right face already has the right inward normal:
-- `Back` is +Z, `Front` is -Z, `Right` is +X, `Left` is -X, and each of the four
-- below is the one that points at the origin.
--
-- Rotating the panes and giving them all the same face would be the same
-- geometry expressed as arithmetic somebody has to check, and `ReachOf` would
-- then be picking a half-extent axis that this table no longer states.
--
-- The X walls run the plate's exact depth and the Z walls overrun by a
-- thickness at each end, so the corners meet flush instead of overlapping into
-- a z-fighting post.
--
-- **One effect per wall, which is the point of putting four here.** A demo that
-- showed one grade would show what the feature does; four facing each other show
-- what it *costs*, which is nothing - the surface pass is identical for all of
-- them and the difference is a handful of instructions where the pane samples.
--
-- They also show each other. Stand between the thermal wall and the swirl and
-- the corridor of reflections carries both grades, each applied once by the pane
-- it belongs to rather than compounding down the recursion - because the texture
-- holds an ungraded picture and only the pane that shows it grades.
local WALLS = {
	{
		Name = "MirrorNorth",
		Position = Vector3.new(0, WALL_HEIGHT / 2, -WALL_OFFSET),
		Size = Vector3.new(PLATE + WALL_THICKNESS * 2, WALL_HEIGHT, WALL_THICKNESS),
		Face = Enum.NormalId.Back,

		-- The wall the viewer faces on startup, so the first thing anybody sees
		-- is that these do something.
		Effect = Enum.SurfaceEffect.NightVision,
	},
	{
		Name = "MirrorEast",
		Position = Vector3.new(WALL_OFFSET, WALL_HEIGHT / 2, 0),
		Size = Vector3.new(WALL_THICKNESS, WALL_HEIGHT, PLATE),
		Face = Enum.NormalId.Left,
		Effect = Enum.SurfaceEffect.Thermal,
	},
	{
		Name = "MirrorWest",
		Position = Vector3.new(-WALL_OFFSET, WALL_HEIGHT / 2, 0),
		Size = Vector3.new(WALL_THICKNESS, WALL_HEIGHT, PLATE),
		Face = Enum.NormalId.Right,
		Effect = Enum.SurfaceEffect.Cctv,
	},
	{
		Name = "MirrorSouth",
		Position = Vector3.new(0, WALL_HEIGHT / 2, WALL_OFFSET),
		Size = Vector3.new(PLATE + WALL_THICKNESS * 2, WALL_HEIGHT, WALL_THICKNESS),
		Face = Enum.NormalId.Front,

		-- Behind the viewer at the start, and the only one that moves texels
		-- rather than grading them.
		Effect = Enum.SurfaceEffect.Swirl,
	},
}

-- **Wide rather than square, at the same texel count.** A wall is four times as
-- wide as it is tall and a square target spends half its texels on sky above it;
-- 2048x512 is the same megapixel as the 1024x1024 a single pane used and puts it
-- where the wall is. All four declare the same size because all four walls are
-- the same shape, not because they share a target - since v0.8 each index owns
-- its own pair.
local SURFACE_SIZE = Vector3.new(2048, 512)

for _, wall in WALLS do
	local pane = Instance.new("Part")
	pane.Name = wall.Name
	pane.Anchored = true
	pane.Position = wall.Position
	pane.Size = wall.Size

	-- White, because the tint multiplies whatever the reflection carries. A
	-- coloured mirror is a colour here and a plain one is white.
	pane.Color = Color3.new(1, 1, 1)

	-- **Not a caster.** A twenty-metre wall on every side would rake the floor
	-- with its own shadow and bury the cube shadows this scene exists to show.
	-- `CastShadow` takes it out of the shadow pass without taking it out of the
	-- frame, which is the whole distinction the property was added for.
	pane.CastShadow = false

	-- **Nothing here says which texture this pane shows, and that is the
	-- point.** A pane is a mirror because a `SurfaceCamera` is parented to it -
	-- a plain `Camera` projects nothing - so `scene::AimSurfaceCameras` hands
	-- out the slots and writes both ends of the pairing itself.
	pane.Parent = workspace

	local reflection = Instance.new("SurfaceCamera")
	reflection.Name = "Reflection"
	reflection.SurfaceSize = SURFACE_SIZE
	reflection.Face = wall.Face

	-- **A texture of its own, and nothing in this file asks for one.** Every
	-- surface camera used to share slot 0, so all four wrote into one target and
	-- the last to render won - three walls projected a fourth wall's image
	-- across themselves. The fix was a slot per camera, and the slots are handed
	-- out by `scene::AimSurfaceCameras` in entity order rather than typed here:
	-- a render-target index is the engine's bookkeeping, and an authored one is
	-- a number two cameras can silently be given the same value of.

	-- **Not authored any more, and the attempt is left here as the reason.**
	-- This used to be 70 degrees, chosen to be "wide enough to still cover the
	-- pane when the viewer walks up to it" - which is the one thing a constant
	-- cannot be. The reflected camera stands as far behind the glass as the eye
	-- is in front, so the pane subtends the same angle from the camera as from
	-- the viewer: this wall needs 24 degrees from the middle of the room and 127
	-- from five units away, and past the authored 70 the reflection stopped
	-- covering the wall and drew as a hard-edged rectangle floating on grey.
	--
	-- `scene::AimSurfaceCameras` fits it to the pane's corners every frame, so a
	-- value set here would be overwritten - as `CFrame` and `NearPlaneZ` already
	-- are, and for the same reason. A surface camera parented to a part is
	-- placed by the engine, frustum included.

	-- **The image's own opacity, which is not the pane's.** At 0 the reflection
	-- is solid whatever the pane's `Transparency` is - so a sheet of glass you
	-- can see through still carries a mirror image, which is what glass does and
	-- what one shared number could never express.
	reflection.ImageTransparency = 0

	-- **A grade on the way out, not a second render.** The surface pass draws
	-- the world exactly as it would have whatever this says; the effect is
	-- applied in `opaque.frag` where the pane samples the texture. So a wall
	-- costs the same to render graded as ungraded, and switching one at runtime
	-- redraws nothing.
	reflection.Effect = wall.Effect
	reflection.Parent = pane
end

-- The things that cast, and that the mirrors show. Deterministic placement, for
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

	-- **Symmetric in X and Z, which the single-pane version was not.** It
	-- spread the cubes across a band in front of one mirror; four walls want a
	-- cluster in the middle that every one of them has something to reflect.
	local base = Vector3.new(
		range(index, 3, -CASTER_SPREAD, CASTER_SPREAD),
		range(index, 5, 1.5, 8),
		range(index, 7, -CASTER_SPREAD, CASTER_SPREAD)
	)
	part.Position = base
	part.Parent = workspace

	table.insert(casters, {
		Part = part,
		Base = base,
		Phase = range(index, 11, 0, 6.283185),
		Rate = range(index, 13, 0.4, 1.3),
	})
end

-- The camera the scene is *watched* through. Not a surface one: a world has
-- several cameras and `workspace.CurrentCamera` names the live one, which is
-- exactly the distinction `ActiveCamera` exists for.
--
-- An identity rotation looks down -Z, so this faces the north wall - the one
-- built first and therefore the one that reflects.
local view = Instance.new("Camera")
view.Name = "Viewer"
view.CFrame = CFrame.new(EYE)
view.FieldOfView = EYE_FOV
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

local grades = {}
for _, wall in WALLS do
	table.insert(grades, wall.Effect.Name)
end

print(
	`mirrors: {#casters} casters, {#WALLS} mirrored walls, each on a surface of its own - `
		.. table.concat(grades, ", ")
)
