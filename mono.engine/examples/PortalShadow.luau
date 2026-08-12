--!strict
-- A body standing in a seam, on a floor that shows what its shadow is doing.
--
-- **Written for one report and shaped so that report is unmistakable.** A
-- character straddling a portal has two halves: the near one drawn directly and
-- the far one drawn as a clone in the destination room. Both are lit by the
-- world's one directional light, and the map between two panes is a rotation —
-- so the far half's shadow comes out turned by that rotation relative to the
-- near half's. Standing in the hole you see one body with two shadows pointing
-- different ways, and no shadow crossing the seam at all.
--
-- ## What to look at
--
-- The floor either side of the seam is a checker in two colours, blue for the
-- room the pane is in and red for the room it leads to, so which room a patch of
-- shadow is lying on can be read off the picture. The pair turns a quarter, so a
-- shadow that is wrong is wrong by ninety degrees rather than by something that
-- has to be measured.
--
-- Three things are worth checking and they fail independently:
--
--   1. **The near half's shadow.** Cast by the real body against the world's
--      light. This one has always been right.
--   2. **The far half's shadow.** Cast by the clone, against the same light,
--      which is what makes it point the wrong way. `scene::AppendPortalClones`
--      is where the clone is made.
--   3. **A shadow crossing the seam.** A caster on one side of a hole should
--      shadow the floor on the other, and does not — there is one shadow map,
--      fitted once to the whole scene from one direction, so light through a
--      hole is not modelled at all.
--
-- ## Why the body is not anchored
--
-- Only a body that can move is cloned. `scene::CloneThroughSeams` walks bodies
-- with motion and character limbs, which is what keeps a floor from being copied
-- into the room next door — so an anchored crate in a seam draws on one side
-- only and is not the case this scene is about. The crate here rests on the
-- floor under gravity, which is also what a character does.

local WORLD = workspace

local ROOM = 40 -- how far a floor reaches from its middle
local TILE = 5
local WALL_HEIGHT = 12
local PANE_THICKNESS = 0.25
local GAP = 90 -- between the two rooms, so neither lights the other

local SURFACE_SIZE = Vector3.new(1024, 1024)

local BLUE_A = Color3.fromRGB(20, 130, 235)
local BLUE_B = Color3.fromRGB(120, 200, 250)
local RED_A = Color3.fromRGB(200, 25, 30)
local RED_B = Color3.fromRGB(140, 15, 20)
local PANE = Color3.new(0.55, 0.57, 0.62)
local CRATE = Color3.fromRGB(240, 205, 40)

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

-- A checker, because a shadow's direction is only readable against something
-- with a grid in it.
local function floor(name: string, centre: Vector3, first: Color3, second: Color3)
	local count = math.floor(ROOM * 2 / TILE)
	for x = 0, count - 1 do
		for z = 0, count - 1 do
			local at = centre + Vector3.new((x + 0.5) * TILE - ROOM, -0.5, (z + 0.5) * TILE - ROOM)
			local shade = if (x + z) % 2 == 0 then first else second
			block(name .. x .. "_" .. z, Vector3.new(TILE, 1, TILE), CFrame.new(at), shade)
		end
	end
end

floor("Near", Vector3.zero, BLUE_A, BLUE_B)
floor("Far", Vector3.new(GAP, 0, 0), RED_A, RED_B)

-- --- the pair --------------------------------------------------------------
--
-- **A quarter turn between them, so a shadow that is wrong is wrong by ninety
-- degrees.** A parallel pair would leave the two shadows agreeing by luck, which
-- is the arrangement that hides this bug rather than showing it.
--
-- The authoring rule, which `Hallway.luau` states at length: a traveller who
-- walks in from the side a pane faces arrives on the side the destination faces.
-- So the near pane faces the camera and the far pane faces into its own room.
local nearPane = block(
	"NearPane",
	Vector3.new(TILE * 2, WALL_HEIGHT, PANE_THICKNESS),
	CFrame.new(Vector3.new(0, WALL_HEIGHT / 2, 0)),
	PANE
)
nearPane.CanCollide = false

-- Turned a quarter, so its own front points along +X into the middle of the far
-- floor rather than back down the corridor between the two rooms.
local farPane = block(
	"FarPane",
	Vector3.new(TILE * 2, WALL_HEIGHT, PANE_THICKNESS),
	CFrame.new(Vector3.new(GAP - 10, WALL_HEIGHT / 2, 0)) * CFrame.Angles(0, -math.pi / 2, 0),
	PANE
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

-- --- what casts the shadow -------------------------------------------------
--
-- **Halfway through, and unanchored so it is cloned.** Two studs of it on each
-- side of the plane is enough that both halves are on screen at once and neither
-- is a sliver.
local crate = Instance.new("Part")
crate.Name = "Straddler"
crate.Size = Vector3.new(3, 6, 3)
crate.CFrame = CFrame.new(Vector3.new(0, 3.5, 0))
crate.Color = CRATE
crate.Anchored = false
crate.Parent = WORLD

-- A second one well clear of the pane, as the control: its shadow is what a
-- correct one looks like on this floor, in this light, at this time of day.
block("Control", Vector3.new(3, 6, 3), CFrame.new(Vector3.new(-14, 3, 12)), CRATE)

-- --- light -----------------------------------------------------------------
--
-- A lamp over each room. The shadow this scene is about is cast by the world's
-- directional light rather than by these — `render` fits one shadow map to the
-- whole scene from `SUN_DIRECTION` — so these are here to make the floors
-- readable and not to cast anything.
local function lamp(name: string, at: Vector3)
	local fitting = block(name, Vector3.new(2, 0.5, 2), CFrame.new(at), Color3.new(1, 0.98, 0.92))
	fitting.CanCollide = false

	local bulb = Instance.new("PointLight")
	bulb.Name = "Bulb"
	bulb.Color = Color3.fromRGB(255, 250, 240)
	bulb.Brightness = 220
	bulb.Range = 120
	bulb.Parent = fitting
end

lamp("NearLamp", Vector3.new(0, 26, 14))
lamp("FarLamp", Vector3.new(GAP, 26, 0))

-- --- where you stand -------------------------------------------------------

block("SpawnLocation", Vector3.new(6, 1, 6), CFrame.new(Vector3.new(0, 0.5, 16)), BLUE_B)

-- `_G.SHADOW_VIEW` names a still for a headless capture; unset, walk it.
local WHICH = _G.SHADOW_VIEW or ""
local viewpoint: CFrame? = nil

if WHICH == "seam" then
	-- Down on the seam from over the near room's shoulder, which is the angle
	-- the report was made from: both halves of the crate and both patches of
	-- floor in one frame.
	viewpoint = CFrame.new(Vector3.new(9, 11, 16)) * CFrame.Angles(-0.5, 0.5, 0)
elseif WHICH == "far" then
	-- The same seam from the far room, so the clone can be compared against the
	-- body it is a copy of.
	viewpoint = CFrame.new(Vector3.new(GAP - 22, 11, 12)) * CFrame.Angles(-0.5, -1.0, 0)
end

if viewpoint ~= nil then
	local view = Instance.new("Camera")
	view.Name = "Viewer"
	view.FieldOfView = 70
	view.CFrame = viewpoint
	view.Parent = WORLD
	WORLD.CurrentCamera = view
end

print("portal shadow: near room at origin, far room at x=" .. GAP .. " through a quarter-turn pair")
