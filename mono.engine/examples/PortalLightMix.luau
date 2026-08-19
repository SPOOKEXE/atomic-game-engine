--!strict
-- A red lamp on one side of a hole, a green lamp on the other, and the pools
-- where each room shows both.
--
-- **The scene the roadmap's "two coloured lights either side of a portal mix"
-- line is measured against.** Each lamp stands inside its own seam's reach, so
-- `client::CollectLights` copies it through: the red lamp gains a twin behind
-- the far pane and the green lamp a twin behind the near one. The floor in
-- front of each pane is then lit twice - by its own room's lamp directly and by
-- the other room's through the hole - and on a neutral floor with no other
-- light that reads as one yellow-ish pool with both channels in it.
--
-- ## What to look at
--
--   * `near` - the near room. Correct is a mixed pool in front of the pane:
--     red from this room's own lamp, green that can only have come through the
--     hole, because no green lamp exists on this side. The corner past both
--     ranges stays dark.
--   * `far` - the far room, mirrored: its own green plus transported red.
--
-- The mix is the check, and each channel is a separate failure: a pool with no
-- green in the near view means the green copy was not made or landed wrong; no
-- red in it means this room's own lamp is broken, which is the control.
--
-- ## The geometry, and why the numbers are what they are
--
-- Rooms 120 apart against ranges of 20, so nothing lights the other room
-- directly. Each lamp is 4 in front of its pane - inside its seam's reach, so
-- the copy is made, and its twin lands 4 *behind* the far pane, shining
-- forward into the room the hole shows, which is where the sub-camera stands
-- and for the same reason. Both pools overlap on the patch a stud or two in
-- front of each pane, and neither reaches the room's far corner.

local WORLD = workspace
local Lighting = game:GetService("Lighting")

local GAP = 120 -- between the two rooms, far beyond either lamp's range
local ROOM = 30 -- how far a floor reaches from its middle
local PANE = 10 -- the hole is square and this is its side

local SURFACE_SIZE = Vector3.new(1024, 1024)

local FLOOR = Color3.fromRGB(200, 200, 200)
local GLASS = Color3.new(1, 1, 1) -- white, or the pane tints the far room
local RED = Color3.fromRGB(255, 40, 40)
local GREEN = Color3.fromRGB(60, 255, 60)

-- No sun and no ambient: every lit pixel is one of the two lamps'.
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

block("NearFloor", Vector3.new(ROOM * 2, 1, ROOM * 2), CFrame.new(0, -0.5, 0), FLOOR)
block("FarFloor", Vector3.new(ROOM * 2, 1, ROOM * 2), CFrame.new(GAP, -0.5, 0), FLOOR)

-- The pane pair `PortalSeam.luau` proves out: near front faces -Z where the
-- viewer stands, far pane turned a quarter so its front points +X into its
-- room.
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

-- --- the two lamps -----------------------------------------------------------
--
-- Each 4 in front of its own pane's face: -Z of the near pane, +X of the far.
-- Range 20 so the pools end inside their rooms and the corners stay dark.
local function lamp(name: string, at: Vector3, colour: Color3)
	local fitting = block(name, Vector3.new(0.5, 0.5, 0.5), CFrame.new(at), Color3.new(0.1, 0.1, 0.1))
	fitting.CanCollide = false

	local bulb = Instance.new("PointLight")
	bulb.Name = "Bulb"
	bulb.Color = colour
	bulb.Brightness = 250
	bulb.Range = 20
	bulb.Parent = fitting
end

lamp("RedLamp", Vector3.new(0, 6, -4), RED)
lamp("GreenLamp", Vector3.new(GAP - 6, 6, 0), GREEN)

block("SpawnLocation", Vector3.new(6, 1, 6), CFrame.new(0, 0.5, -14), FLOOR)

-- --- where you stand ---------------------------------------------------------
--
-- The line `capture-portal-lighting.sh` rewrites; `_G` is readonly in the
-- script sandbox, so substitution is the only mechanism that works from a
-- shell. Same arrangement as `PortalSeam.luau`.
local VIEW = ""
local WHICH = VIEW
local viewpoint: CFrame? = nil

if WHICH == "near" then
	-- The mixed pool in front of the near pane, with the room's south-west
	-- corner in frame as the dark control.
	viewpoint = CFrame.lookAt(Vector3.new(-14, 12, -22), Vector3.new(0, 1, -4))
elseif WHICH == "far" then
	-- The far room's own pool, mirrored: green direct, red through the hole.
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

print("portal light mix: red at the origin pane, green at x=" .. GAP - 6 .. ", no sun")
