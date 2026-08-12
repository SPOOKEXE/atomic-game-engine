--!strict
-- Six impossible spaces, side by side, each one a different trick.
--
-- `Portals-1-world.luau` shows *that* a hole can lead somewhere the wall does
-- not. This shows what that is good for: a tunnel shorter inside than out, one
-- longer inside than out, a house with a room that is not there, a pillar you
-- can walk around for ever, a hill you climb by going down, and a room holding
-- four times its own volume.
--
-- **Every one of them is the same feature and none costs more than the others.**
-- A `Portal` is a `SurfaceCamera` with a `Destination`; its camera is placed at
-- `destination · half-turn · source⁻¹`, and that is the whole of it.
-- `NON-EUCLIDEAN.md` is the investigation and `Portals-1-world.luau` is the
-- minimal case. What differs below is *geometry*, not engine.
--
-- ## The one authoring rule, and it has two halves
--
-- `Face` is resolved on the destination as well as on the source, so a hole
-- shows whatever that face points at on the part it leads to. Every gate here
-- uses `Back` — local +Z — and the two ends mean the same thing by it:
--
-- * **the source's `Back` faces whoever is looking**, because that is the face
--   the image is drawn on;
-- * **the destination's `Back` faces the space to be shown**, because that is
--   the direction you would come out travelling.
--
-- They are one statement about which way you go through. Get the second wrong
-- and nothing complains — the pane renders, the camera is placed, the frustum
-- fits, and the picture is the back of a wall. `mouth()` and `throat()` below
-- exist so that neither end can be built by hand.
--
-- ## What these cannot do yet, said plainly
--
-- **You can look through them and not walk through them.** Traversal is
-- `ROADMAP.md` v0.15's character controller — `docs/DEFERRED.md` D00112 — so
-- every exhibit is arranged to make its point from outside, which is also how
-- they read in a video.
--
-- **And none of them changes scale.** The map is rigid, so the shrinking tunnel
-- — round once and you are smaller, round again and you fit — is the one trick
-- on that list this engine cannot currently express. It needs a similarity
-- rather than a rigid transform, in the placement and in the traversal that does
-- not exist yet. Filed with the rest of `D00112`.

local RunService = game:GetService("RunService")

-- **Sixteen surfaces a world, shared with every mirror in it.** See
-- `scene::MAX_SURFACES`. Twelve holes below, so the budget is a decision rather
-- than an accident: a seventh exhibit takes its holes from another.
local GATE_SIZE = Vector3.new(768, 768)

-- The exhibits stand in a row, and what each opens onto is built a long way off
-- — the illusion is that they are adjacent, and building them adjacent would let
-- one exhibit's lighting and geometry arrive at the edges of another.
local STAGE_STEP = 44
local AWAY = 500

local WHITE = Color3.new(0.92, 0.92, 0.94)
local FLOOR = Color3.fromRGB(96, 100, 110)
local HALF_TURN = CFrame.Angles(0, math.pi, 0)

local function block(name: string, cframe: CFrame, size: Vector3, colour: Color3): Part
	local part = Instance.new("Part")
	part.Name = name
	part.Anchored = true
	part.Size = size
	part.CFrame = cframe
	part.Color = colour

	-- Not casters. Every exhibit is a box with an opening in it, and boxes
	-- shadowing their own interiors is a row of black holes.
	part.CastShadow = false
	part.Parent = workspace
	return part
end

local function panel(name: string, cframe: CFrame, width: number, height: number): Part
	return block(name, cframe, Vector3.new(width, height, 0.4), WHITE)
end

-- The end you look in at: `Back` towards the viewer, who is down +Z.
local function mouth(name: string, at: Vector3, width: number, height: number, yaw: number?): Part
	return panel(name, CFrame.new(at) * CFrame.Angles(0, yaw or 0, 0), width, height)
end

-- The end you come out of: `Back` into the space, which every hidden space here
-- occupies down -Z from its own gate.
local function throat(name: string, at: Vector3, width: number, height: number): Part
	return panel(name, CFrame.new(at) * HALF_TURN, width, height)
end

local holes = 0

local function link(from: Part, to: Part, name: string)
	local portal = Instance.new("Portal")
	portal.Name = name
	portal.SurfaceSize = GATE_SIZE
	portal.Face = Enum.NormalId.Back
	portal.Destination = to

	-- Solid. A hole with an image transparency is a window with glass in, which
	-- is a different thing and also expressible.
	portal.ImageTransparency = 0
	portal.Parent = from

	holes += 1
end

-- A box with its +Z side left out, so a hole can open onto it. Returns the
-- middle of the missing side, which is where that hole's far gate goes.
local function chamber(
	name: string,
	origin: Vector3,
	span: number,
	height: number,
	colour: Color3
): Vector3
	local half = span / 2
	local mid = Vector3.new(0, height / 2, 0)

	block(name .. "Floor", CFrame.new(origin + Vector3.new(0, -0.25, 0)), Vector3.new(span, 0.5, span), colour)
	block(name .. "Ceiling", CFrame.new(origin + Vector3.new(0, height, 0)), Vector3.new(span, 0.5, span), colour)
	block(name .. "Far", CFrame.new(origin + mid - Vector3.new(0, 0, half)), Vector3.new(span, height, 0.5), WHITE)
	block(name .. "Left", CFrame.new(origin + mid - Vector3.new(half, 0, 0)), Vector3.new(0.5, height, span), WHITE)
	block(name .. "Right", CFrame.new(origin + mid + Vector3.new(half, 0, 0)), Vector3.new(0.5, height, span), WHITE)

	return origin + mid + Vector3.new(0, 0, half)
end

-- Something with a shape in a space, so one is told from another through a hole
-- by more than the colour of its floor.
local function marker(name: string, at: Vector3, colour: Color3): Part
	return block(name, CFrame.new(at + Vector3.new(0, 2.5, 0)), Vector3.new(2, 5, 2), colour)
end

-- The ground the row stands on.
--
-- **Only under the exhibits, not under what they lead to.** A plate that reached
-- the hidden spaces would be a floor stretching between places the scene claims
-- are adjacent, and it would be visible through every hole as a grey band under
-- a room that is supposed to be somewhere else.
block(
	"Baseplate",
	CFrame.new(Vector3.new(STAGE_STEP * 2.5, -0.4, -14)),
	Vector3.new(STAGE_STEP * 6 + 40, 0.5, 70),
	Color3.fromRGB(58, 62, 70)
)

-- A straight passage down -Z with its far end capped. Returns nothing; the gate
-- is the caller's, because only the caller knows which end is which.
local function passage(name: string, from: Vector3, length: number, cap: Color3)
	local middle = from - Vector3.new(0, 0, length / 2)

	block(name .. "Left", CFrame.new(middle + Vector3.new(-3.2, 4, 0)), Vector3.new(0.5, 8, length), WHITE)
	block(name .. "Right", CFrame.new(middle + Vector3.new(3.2, 4, 0)), Vector3.new(0.5, 8, length), WHITE)
	block(name .. "Roof", CFrame.new(middle + Vector3.new(0, 8, 0)), Vector3.new(7, 0.5, length), FLOOR)
	block(name .. "Floor", CFrame.new(middle + Vector3.new(0, -0.25, 0)), Vector3.new(7, 0.5, length), FLOOR)
	block(
		name .. "End",
		CFrame.new(from + Vector3.new(0, 4, -length - 0.2)),
		Vector3.new(7, 8, 0.5),
		cap
	)
end

-- --- 1. Shorter inside than out -------------------------------------------
--
-- **Thirty units of tunnel from the outside and eight from the inside.** The
-- mouth opens onto a short passage built elsewhere, so the far end arrives long
-- before the outside said it would. The shortest path between two points stopped
-- being a straight line.
do
	local at = Vector3.new(0, 0, 0)
	passage("LongShell", at, 30, FLOOR)

	local inside = at + Vector3.new(0, 0, -AWAY)
	passage("ShortInside", inside, 8, Color3.fromRGB(210, 120, 90))
	marker("ShortInsideMarker", inside + Vector3.new(0, 0, -6), Color3.fromRGB(240, 180, 90))

	link(
		mouth("LongMouth", at + Vector3.new(0, 4, 0), 6, 8),
		throat("ShortInsideMouth", inside + Vector3.new(0, 4, 0), 6, 8),
		"IntoTheShortTunnel"
	)
end

-- --- 2. Longer inside than out --------------------------------------------
--
-- The same trick with its sign flipped, and the pair is why both are here: one
-- of them alone reads as a modelling mistake, and two of them read as a rule.
do
	local at = Vector3.new(STAGE_STEP, 0, 0)
	passage("ShortShell", at, 8, FLOOR)

	local inside = at + Vector3.new(0, 0, -AWAY)
	passage("LongInside", inside, 60, Color3.fromRGB(90, 150, 210))

	-- Posts down its length, because a corridor with nothing in it gives the eye
	-- no way to judge how far the end is — which is the whole claim being made.
	for step = 1, 5 do
		marker(
			"LongInsidePost" .. step,
			inside + Vector3.new(if step % 2 == 0 then 2 else -2, 0, -step * 10),
			Color3.fromRGB(120, 200, 240)
		)
	end

	link(
		mouth("ShortMouth", at + Vector3.new(0, 4, 0), 6, 8),
		throat("LongInsideMouth", inside + Vector3.new(0, 4, 0), 6, 8),
		"IntoTheLongTunnel"
	)
end

-- --- 3. The house with a room that is not there ---------------------------
--
-- **Three rooms behind four doors.** Each door opens onto the next room round
-- and the fourth opens onto the first, so a viewer counting doors counts four
-- rooms and a viewer counting rooms finds three. The demo this is taken from
-- reports that nobody notices unless told, which is the interesting part: the
-- contradiction is in the bookkeeping and there is nothing to see.
do
	local at = Vector3.new(STAGE_STEP * 2, 0, 0)
	local tints = {
		Color3.fromRGB(200, 80, 80),
		Color3.fromRGB(90, 190, 110),
		Color3.fromRGB(90, 130, 220),
	}

	local rooms: { Part } = {}
	for index = 1, 3 do
		local origin = at + Vector3.new((index - 2) * 30, 0, -AWAY)
		local opening = chamber("House" .. index, origin, 18, 8, tints[index])
		marker("HouseMarker" .. index, origin, tints[index])
		rooms[index] = throat("HouseWay" .. index, opening + Vector3.new(0, 0, 0.3), 5, 7)
	end

	block("HouseFacade", CFrame.new(at + Vector3.new(0, 4, 0)), Vector3.new(30, 8, 0.5), FLOOR)

	for index = 1, 4 do
		-- **The fourth door is the first room again**, which is where the count
		-- stops adding up. Nothing anywhere says a room may be behind only one
		-- door.
		link(
			mouth("HouseDoor" .. index, at + Vector3.new((index - 2.5) * 6.5, 4, 0.4), 5, 7),
			rooms[((index - 1) % 3) + 1],
			"HouseDoor" .. index .. "Way"
		)
	end
end

-- --- 4. The pillar with no end --------------------------------------------
--
-- **Two objects in the frame at once that are in different spaces.** The pillar
-- has a hole down each side; the left opens onto one room and the right onto
-- another, and both are visible together. Walking round it does not bring you
-- back where you started — the cycle is longer than the pillar is.
do
	local at = Vector3.new(STAGE_STEP * 3, 0, 0)

	block("PillarFloor", CFrame.new(at + Vector3.new(0, -0.25, -6)), Vector3.new(24, 0.5, 16), FLOOR)
	block("PillarShaft", CFrame.new(at + Vector3.new(0, 5, -8)), Vector3.new(4, 10, 4), Color3.fromRGB(150, 140, 120))
	block("PillarBack", CFrame.new(at + Vector3.new(0, 4, -13)), Vector3.new(24, 8, 0.5), FLOOR)

	local hues = {Color3.fromRGB(230, 150, 60), Color3.fromRGB(70, 200, 190)}
	for index = 1, 2 do
		local origin = at + Vector3.new((index - 1.5) * 44, 0, -AWAY)
		local opening = chamber("Turn" .. index, origin, 16, 9, hues[index])
		marker("TurnMarker" .. index, origin, hues[index])

		link(
			mouth("PillarWay" .. index, at + Vector3.new((index - 1.5) * 12, 4, -12.6), 6, 8),
			throat("TurnWay" .. index, opening + Vector3.new(0, 0, 0.3), 6, 8),
			"AroundThePillar" .. index
		)
	end
end

-- --- 5. Uphill both ways --------------------------------------------------
--
-- **A slope climbed by going down.** The mouth is at the bottom of the hill and
-- what it opens onto is the top, so the shortest way up is down. There is no
-- gradient in this at all: the two ends of one hole are at different heights,
-- which a rigid map allows and a single space does not.
do
	local at = Vector3.new(STAGE_STEP * 4, 0, 0)

	-- The hill as steps, because a ramp wants a mesh and steps want nothing.
	for step = 1, 8 do
		block(
			"HillStep" .. step,
			CFrame.new(at + Vector3.new(0, step * 0.9 - 0.45, -6 - step * 2)),
			Vector3.new(16, step * 1.8, 2),
			Color3.fromRGB(110, 140, 100)
		)
	end

	block("HillMouthWall", CFrame.new(at + Vector3.new(0, 3, -4)), Vector3.new(16, 6, 0.5), FLOOR)

	local top = at + Vector3.new(0, 14.4, -AWAY)
	block("HillTopFloor", CFrame.new(top + Vector3.new(0, -0.25, -8)), Vector3.new(16, 0.5, 16), Color3.fromRGB(110, 140, 100))
	block("HillTopFar", CFrame.new(top + Vector3.new(0, 3, -16.2)), Vector3.new(16, 6, 0.5), WHITE)
	marker("HillTopMarker", top + Vector3.new(0, 0, -10), Color3.fromRGB(240, 230, 140))

	link(
		mouth("HillMouth", at + Vector3.new(0, 2.5, -3.6), 5, 5),
		throat("HillTopMouth", top + Vector3.new(0, 2.5, 0), 5, 5),
		"UphillBothWays"
	)
end

-- --- 6. Four times the volume, in one room --------------------------------
--
-- **The case the demo ends on and the one with a use.** A room you can pace out
-- is twenty across; its three built walls open onto three parts of a space four
-- times larger. For room-scale VR that is the whole problem: the play area stays
-- the size of the room and the level does not.
--
-- Three holes rather than one, because a single wall opening onto a bigger room
-- is a window. Several, each showing a different part and each consistent with
-- the wall it is set in, is a room bigger than itself.
do
	local at = Vector3.new(STAGE_STEP * 5, 0, 0)

	block("CellFloor", CFrame.new(at + Vector3.new(0, -0.25, -10)), Vector3.new(20, 0.5, 20), FLOOR)

	-- The fourth side is left out to look in through, which is this exhibit's
	-- compromise: somebody who could walk in would have four of them.
	local sides = {
		{Name = "Far", At = Vector3.new(0, 4, -19.8), Wall = Vector3.new(0, 4, -20.2), Size = Vector3.new(20, 8, 0.5), Yaw = 0},
		{Name = "Left", At = Vector3.new(-9.8, 4, -10), Wall = Vector3.new(-10.2, 4, -10), Size = Vector3.new(0.5, 8, 20), Yaw = math.pi / 2},
		{Name = "Right", At = Vector3.new(9.8, 4, -10), Wall = Vector3.new(10.2, 4, -10), Size = Vector3.new(0.5, 8, 20), Yaw = -math.pi / 2},
	}

	local part = 0
	for _, side in sides do
		block("Cell" .. side.Name, CFrame.new(at + side.Wall), side.Size, FLOOR)
		part += 1

		-- **Turned to face into the room.** `Back` is the side an image is drawn
		-- on, so a panel in the left-hand wall needs a quarter turn to be seen
		-- from inside at all.
		local looked = mouth("CellWay" .. side.Name, at + side.At, 18, 7, side.Yaw)

		local origin = at + Vector3.new((part - 2) * 50, 0, -AWAY)
		local opening = chamber("Quarter" .. part, origin, 40, 10, Color3.fromRGB(60 + part * 25, 90, 150))
		for post = 1, 4 do
			marker(
				"QuarterPost" .. part .. "x" .. post,
				origin + Vector3.new((post - 2.5) * 9, 0, -6 - (post % 2) * 7),
				Color3.fromRGB(210, 200 - post * 25, 120)
			)
		end

		link(looked, throat("QuarterWay" .. part, opening + Vector3.new(0, 0, 0.3), 18, 7), "Quarter" .. part)
	end
end

-- Where the scene is watched from.
--
-- **It moves, and that is not decoration.** Every hole is placed from wherever
-- this camera is, every frame, so it is the parallax of the far side sliding
-- against the near side that says "hole" instead of "picture". A still of any of
-- these proves nothing, which is the honest thing to say about a screenshot of a
-- portal.
local view = Instance.new("Camera")
view.Name = "Viewer"
view.CFrame = CFrame.new(Vector3.new(0, 5, 16))
view.FieldOfView = 70
view.Parent = workspace
workspace.CurrentCamera = view

local LAST = STAGE_STEP * 5
local PACE = 7
local elapsed = 0

RunService.Heartbeat:Connect(function(deltaTime: number)
	elapsed += deltaTime * PACE

	-- A sweep along the row and back, seen from three-quarters on.
	--
	-- **Not square on, and that is what makes half of these legible.** The first
	-- two exhibits are about how long a tunnel *looks* from outside, which a
	-- camera staring straight down the mouth cannot show — it sees the hole and
	-- none of the shell. Standing off to one side puts the outside and the
	-- inside in one frame, which is the comparison every one of them is making.
	local lap = elapsed % (LAST * 2)
	local along = if lap > LAST then LAST * 2 - lap else lap
	local sway = math.sin(elapsed * 0.05) * 6

	view.CFrame = CFrame.lookAt(
		Vector3.new(along + 15 + sway, 8 + math.sin(elapsed * 0.031) * 2, 21),
		Vector3.new(along, 3.5, -7)
	)
end)

print(
	`non-euclidean: 6 exhibits, {holes} holes — shorter inside, longer inside, a room that is not there, a pillar with no end, a hill climbed downwards, and four times the volume in one room`
)
