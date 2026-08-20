--!strict
-- A lamp standing in the hole itself, in a world with no other light in it.
--
-- **The scene the roadmap's "light in a portal emits out of the portal" line is
-- measured against.** A local light within reach of a seam is copied through it
-- by `client::CollectLights`, so a lamp *in* the pane's own plane has to light
-- both mouths: its own room directly, and the far room through the copy that
-- lands on the far pane. What makes the far half readable is that nothing else
-- here shines - the sun is turned off and both ambients are black, so every lit
-- pixel in the far room is light that crossed the seam and there is no exposure
-- to argue about.
--
-- ## What to look at
--
--   * `near` - the near room, lamp-side. Correct is an amber pool on the floor
--     in front of the pane. Dark here means the lamp itself is wrong, not the
--     portal, which is why the view exists as a control.
--   * `far` - the far room, looking at its own pane. Correct is the same amber
--     pool in front of the far pane. Dark here with `near` lit is the failure
--     this scene exists to catch: the copy was not made, or landed somewhere
--     else.
--
-- Both rooms also keep a corner outside the lamp's range, so the report can
-- check the pool is a pool rather than a raised exposure: lit by the pane,
-- dark in the corner, in the same still.
--
-- ## Why the rooms are 120 apart
--
-- A local light in this pipeline is unshadowed and spills through every wall,
-- so the only thing keeping direct light out of the far room is distance. The
-- gap is four times the lamp's range: anything the far floor shows arrived
-- through the seam or not at all.

local WORLD = workspace
local Lighting = game:GetService("Lighting")

local GAP = 120 -- between the two rooms, far beyond the lamp's range
local ROOM = 30 -- how far a floor reaches from its middle
local PANE = 10 -- the hole is square and this is its side

local SURFACE_SIZE = Vector3.new(1024, 1024)

-- Neutral floors, so what a still shows is the lamp's own colour rather than
-- the floor's albedo arguing with it.
local FLOOR = Color3.fromRGB(200, 200, 200)
local GLASS = Color3.new(1, 1, 1) -- white, or the pane tints the far room
local AMBER = Color3.fromRGB(255, 160, 40)

-- No sun and no ambient. Every lit pixel below is the lamp's.
Lighting.Brightness = 0
Lighting.Ambient = Color3.new(0, 0, 0)
Lighting.OutdoorAmbient = Color3.new(0, 0, 0)

local function block(name: string, size: Vector3, at: CFrame, colour: Color3): Part
	local part = Instance.new("Part")
	part.Name = name
	part.Size = size
	part.CFrame = at
	part.Color = colour
	part.Anchored = true
	part.Parent = WORLD
	return part
end

-- Clear of a baseplate's top face at y = 0: coplanar surfaces do not flicker in
-- this engine, they resolve stably to whichever one the draw order happens to
-- favour. `scripts/demos/zfight-scan.sh` holds the control for that.
local GROUND_LIFT = 0.01

block("NearFloor", Vector3.new(ROOM * 2, 1, ROOM * 2), CFrame.new(0, -0.5 + GROUND_LIFT, 0), FLOOR)
block("FarFloor", Vector3.new(ROOM * 2, 1, ROOM * 2), CFrame.new(GAP, -0.5 + GROUND_LIFT, 0), FLOOR)

-- The pane pair, arranged exactly as `PortalSeam.luau` proves out: the near
-- pane's front faces -Z where the viewer stands, and the far pane is turned a
-- quarter so its own front points +X into the middle of the far floor.
local nearPane = block("NearPane", Vector3.new(PANE, PANE, 0.2), CFrame.new(0, PANE / 2, 0), GLASS)
nearPane.CanCollide = false

local farPane = block(
	"FarPane",
	Vector3.new(PANE, PANE, 0.2),
	CFrame.new(GAP - 10, PANE / 2, 0) * CFrame.Angles(0, -math.pi / 2, 0),
	GLASS
)
farPane.CanCollide = false

local function link(pane: Part, destination: Part, name: string)
	local portal = Instance.new("Portal")
	portal.Name = name
	portal.SurfaceSize = SURFACE_SIZE
	portal.Face = Enum.NormalId.Front
	portal.Destination = destination
	portal.ImageTransparency = 0
	portal.Parent = pane
end

link(nearPane, farPane, "ToFar")
link(farPane, nearPane, "ToNear")

-- --- the lamp, in the hole ---------------------------------------------------
--
-- **The bulb sits on the pane's own plane**, so its distance to the seam
-- rectangle is zero and the copy is unconditional - a scene that placed it a
-- stud out would be testing the range rule as well, and this scene tests only
-- "in the portal, out of the portal". The fitting is small and dark so the
-- pixels it covers cannot be mistaken for the pool it throws.
--
-- **Range 25, against rooms 60 across.** The pool has to end inside the room,
-- so each still holds a lit patch by the pane and a dark corner past the range
-- - the pair a report needs to tell transported light from a raised exposure.
local fitting = block("LampFitting", Vector3.new(0.5, 0.5, 0.5), CFrame.new(0, PANE / 2, 0), Color3.new(0.1, 0.1, 0.1))
fitting.CanCollide = false

local bulb = Instance.new("PointLight")
bulb.Name = "Bulb"
bulb.Color = AMBER
bulb.Brightness = 250
bulb.Range = 25
bulb.Parent = fitting

block("SpawnLocation", Vector3.new(6, 1, 6), CFrame.new(0, 0.5, -14), FLOOR)

-- --- where you stand ---------------------------------------------------------
--
-- **A line the capture script rewrites rather than a global.** `_G` is readonly
-- in the script sandbox, so nothing outside a scene can set one -
-- `capture-portal-lighting.sh` copies the file and substitutes the line below,
-- the same mechanism `PortalSeam.luau` uses.
local VIEW = ""
local WHICH = VIEW
local viewpoint: CFrame? = nil

if WHICH == "near" then
	-- The lamp-side control: the pool in front of the pane, and the room's
	-- south-west corner dark past the range, both in frame.
	viewpoint = CFrame.lookAt(Vector3.new(-14, 12, -22), Vector3.new(0, 1, -4))
elseif WHICH == "far" then
	-- The far room looking back at its own pane, framed as `near` is: the
	-- transported pool in front of the glass, the far corner dark.
	viewpoint = CFrame.lookAt(Vector3.new(GAP + 12, 12, 22), Vector3.new(GAP - 6, 1, 4))
end

if viewpoint ~= nil then
	local view = Instance.new("Camera")
	view.Name = "Viewer"
	view.FieldOfView = 70
	view.CFrame = viewpoint
	view.Parent = WORLD
	WORLD.CurrentCamera = view
end

print("portal light: one amber lamp in the pane at the origin, far room at x=" .. GAP .. ", no sun")
