--!strict
-- A bar standing in a hole, arranged so that being drawn twice is unmistakable.
--
-- **The scene `NON-EUCLIDEAN.md` Part V.1 is measured against.** A body
-- straddling a portal is copied onto the far side so the far room has something
-- to show. Without a cut both copies are drawn *whole*: the original hangs out
-- of the back of the pane into the room it is walking into, and the copy hangs
-- out of the far pane back into the room it came from. A pane set into a thick
-- wall hides both overhangs, which is why every earlier portal scene looked
-- right; this one has a free-standing pane and nothing to hide behind.
--
-- ## What to look at
--
-- The straddler is a long bar pointing straight through the hole, five studs of
-- it on each side of the plane. So:
--
--   * `side` — the camera off to one side, pane edge-on. Correct is **half a
--     bar**: the near five studs and nothing past the plane. A whole bar here
--     is the duplicate this scene exists to catch.
--   * `front` — square on to the hole. Correct is the near half in this room
--     and the rest continuing inside the picture, meeting at the glass with no
--     step and no gap.
--   * `far` — the same seam from the far room, where the copy is the near half
--     and the original is what the hole shows.
--
-- The control bar is the same bar well clear of the pane. It is never cut, so
-- it is what a whole bar looks like at this distance, in this light — which is
-- the comparison a still frame needs to be worth anything.
--
-- ## Why the bar is anchored
--
-- Because what may be copied through a hole is **what fits through it**, and no
-- longer whether it can move. That rule was `Motion` and `CharacterLimb` once,
-- and it refused an anchored crate resting in a seam though it is as much a
-- thing standing in the hole as anything that walked there. An anchored bar
-- also stands exactly where the file puts it, which is what a capture wants.
--
-- `VIEW` names a still for a headless capture; empty, the scene is walkable
-- and spawns in front of the hole. See the note beside it.

local WORLD = workspace

local GAP = 120 -- between the two rooms
local ROOM = 30 -- how far a floor reaches from its middle
local PANE = 10 -- the hole is square and this is its side
local BAR = 10 -- and the bar is exactly as long as the hole is wide

local SURFACE_SIZE = Vector3.new(1024, 1024)

local NEAR_FLOOR = Color3.fromRGB(40, 60, 90)
local FAR_FLOOR = Color3.fromRGB(120, 40, 40)
local GLASS = Color3.new(0.55, 0.57, 0.62)
local STRADDLER = Color3.fromRGB(255, 214, 0)
local CONTROL = Color3.fromRGB(120, 230, 120)

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

-- --- the two rooms -----------------------------------------------------------
--
-- Flat and plain. A checker would read the shadow's direction, which is what
-- `PortalShadow.luau` is for; what this scene needs is a background that makes
-- a yellow bar countable.
block("NearFloor", Vector3.new(ROOM * 2, 1, ROOM * 2), CFrame.new(0, -0.5, 0), NEAR_FLOOR)
block("FarFloor", Vector3.new(ROOM * 2, 1, ROOM * 2), CFrame.new(GAP, -0.5, 0), FAR_FLOOR)

-- --- the hole ----------------------------------------------------------------
--
-- **Free-standing, which is the whole point.** Set into a wall, a pane hides
-- everything that pokes through it and the duplicate is invisible.
--
-- The authoring rule, which `Hallway.luau` states at length: point each pane's
-- face at the space a traveller comes from, and the destination's face at the
-- space they should arrive in. The near pane faces -Z, where the viewer stands;
-- the far pane is turned a quarter so its own front points along +X, into the
-- middle of the far floor.
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

-- --- what stands in it -------------------------------------------------------
--
-- Ten studs long through a ten-stud hole, centred on the plane, so exactly half
-- of it is on each side and the two halves are the same length to the pixel.
-- One stud square, so it fits the hole's footprint with room to spare and is
-- therefore cut rather than left whole.
block("Straddler", Vector3.new(1, 1, BAR), CFrame.new(0, 3, 0), STRADDLER)

-- The same bar, at the same height and the same distance from the side camera,
-- well clear of the hole. Never cut, so it is the length a whole bar reads as.
block("Control", Vector3.new(1, 1, BAR), CFrame.new(0, 3, -18), CONTROL)

-- --- light -------------------------------------------------------------------
--
-- **One lamp, in the near room, close to the hole — because the second thing
-- this scene proves is that its light reaches the other side.** A local light
-- is mapped through every seam it is within range of, so this one has a copy
-- behind the far pane shining forward into the far room, exactly where the
-- sub-camera stands and for exactly that reason. `NON-EUCLIDEAN.md` Part V.3.
--
-- **Cyan, and saturated**, so what arrives on a red floor is unmistakably light
-- that came through the hole rather than the world's own sun with a different
-- exposure. `capture-portal-seam.sh` measures the blue channel of the far floor
-- for that reason.
local function lamp(name: string, at: Vector3, colour: Color3, brightness: number, range: number)
	local fitting = block(name, Vector3.new(2, 0.5, 2), CFrame.new(at), colour)
	fitting.CanCollide = false

	local bulb = Instance.new("PointLight")
	bulb.Name = "Bulb"
	bulb.Color = colour
	bulb.Brightness = brightness
	bulb.Range = range
	bulb.Parent = fitting
end

lamp("NearLamp", Vector3.new(0, 8, -7), Color3.fromRGB(70, 220, 255), 320, 60)

-- --- what proves a *point* goes through -------------------------------------
--
-- **A particle is not a body and is not cut.** It is a point, wholly on one side
-- of the plane or the other, so it is *moved* through the hole rather than
-- copied — drawing it in both places would be two sparks where one was authored.
-- The visible failure without it is a torch whose flame dies at the seam while
-- the torch holding it does not.
--
-- **Standing in the hole itself**, so its plume spreads to both sides of the
-- plane in the same frame: the sparks this side stay, the ones past it are in
-- the far room and are drawn there. Magenta, so the report's yellow and green
-- classifiers cannot mistake a spark for a bar.
local nozzle = block("Nozzle", Vector3.new(1, 1, 1), CFrame.new(0, 6, -6), Color3.new(0.6, 0.2, 0.6))
nozzle.CanCollide = false

local sparks = Instance.new("ParticleEmitter")
sparks.Name = "Through"
sparks.Rate = 200
sparks.Lifetime = NumberRange.new(1.0, 1.4)
sparks.Speed = NumberRange.new(1, 2)
sparks.SpreadAngle = Vector2.new(10, 10)

-- **Driven at the hole rather than aimed at it.** An emitter's own speed is
-- outward from its shape; the acceleration is what carries a plume in a
-- direction, and +Z is straight through the pane six studs away.
sparks.Acceleration = Vector3.new(0, 0, 26)
sparks.Size = NumberSequence.new({
	NumberSequenceKeypoint.new(0, 0.5),
	NumberSequenceKeypoint.new(1, 0.9),
})
sparks.Color = ColorSequence.new({
	ColorSequenceKeypoint.new(0, Color3.fromRGB(255, 60, 220)),
	ColorSequenceKeypoint.new(1, Color3.fromRGB(180, 40, 255)),
})
sparks.Parent = nozzle

block("SpawnLocation", Vector3.new(6, 1, 6), CFrame.new(0, 0.5, -14), NEAR_FLOOR)

-- --- where you stand ---------------------------------------------------------

-- **A line this file rewrites rather than a global.** `_G` is readonly in the
-- script sandbox, so nothing outside a scene can set one — the capture scripts
-- copy the file and substitute the line below, which is the only mechanism that
-- actually works from a shell.
local VIEW = ""
local WHICH = VIEW
local viewpoint: CFrame? = nil

if WHICH == "side" then
	-- **Pane edge-on, bar broadside.** The one frame where half a bar and a
	-- whole bar cannot be confused, and the one the report counts pixels in:
	-- both bars lie across the view at the same depth, so their lengths are
	-- comparable and the straddler's is what the cut changes.
	viewpoint = CFrame.lookAt(Vector3.new(-26, 4, -6), Vector3.new(0, 3.5, -6))
elseif WHICH == "front" then
	-- **Over the shoulder, not square on.** Straight down the bar's own axis it
	-- is an end cap and says nothing; from the quarter you see the near half
	-- run into the glass and the rest continue inside the picture, which is the
	-- join this whole mechanism is about.
	--
	-- Off to the near-west so the control bar is behind the camera rather than
	-- around it — at `z = -16` square on, the eye stands *inside* the control.
	viewpoint = CFrame.lookAt(Vector3.new(-13, 8, -13), Vector3.new(0, 4, 0))
elseif WHICH == "far" then
	-- The far room, looking back at its own pane, where the copy is the half
	-- you can see and the original is what the hole shows.
	viewpoint = CFrame.lookAt(Vector3.new(GAP + 13, 8, 13), Vector3.new(GAP - 10, 4, 0))
end

if viewpoint ~= nil then
	local view = Instance.new("Camera")
	view.Name = "Viewer"
	view.FieldOfView = 70
	view.CFrame = viewpoint
	view.Parent = WORLD
	WORLD.CurrentCamera = view
end

print("portal seam: near room at origin, far room at x=" .. GAP .. " through a quarter-turn pair")
