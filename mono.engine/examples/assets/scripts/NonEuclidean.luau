--!strict
-- Six impossible spaces: shorter and longer tunnels, four doors to three
-- rooms, a pillar with hidden rooms, an uphill shortcut and an expanded cell.
-- Each opening has an explicit return portal and a clear physical doorway.
-- Both ends use Back (+Z locally), with the far face aimed into its room.
-- The engine applies the same seam transform to views, bodies and cameras.

local RunService = game:GetService("RunService")

-- Twenty-six directed surfaces fit the default budget of thirty-two.
local GATE_SIZE = Vector3.new(768, 768)

-- The exhibits stand in a row, and what each opens onto is built a long way off
-- - the illusion is that they are adjacent, and building them adjacent would let
-- one exhibit's lighting and geometry arrive at the edges of another.
local STAGE_STEP = 44

-- **A depth lane each, rather than one row of hidden spaces at a single
-- distance.** The exhibits stand 44 studs apart on the plain, but what they open
-- onto is up to 40 studs wide and offset up to 50 studs from its own exhibit, so
-- hidden spaces all built at one depth overlap: exhibit 3's third room stood
-- inside exhibit 4's first chamber, and exhibit 4's second stood inside exhibit
-- 6's first. A hole cannot show that as a mistake - it shows one room with
-- another room's wall standing through it, which reads as the *portal* being
-- wrong rather than the layout. `away(n)` hands exhibit `n` a depth nothing else
-- uses, so an exhibit may be as wide as its trick needs.
local AWAY = 500
local LANE = 120
local function away(exhibit: number): number
	return AWAY + (exhibit - 1) * LANE
end

-- Clear of a baseplate's top face at y = 0: coplanar surfaces do not flicker in
-- this engine, they resolve stably to whichever one the draw order happens to
-- favour. `scripts/demos/zfight-scan.sh` holds the control for that.
local GROUND_LIFT = 0.01

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
local entrances: { Part } = {}

local function surface(from: Part, to: Part, name: string)
	local portal = Instance.new("Portal")
	portal.Name = name
	portal.SurfaceSize = GATE_SIZE
	portal.Face = Enum.NormalId.Back
	portal.Destination = to
	portal.ImageTransparency = 0
	portal.Parent = from
	holes += 1
	table.insert(entrances, from)
end

local function link(from: Part, to: Part, name: string)
	surface(from, to, name)
	surface(to, from, name .. "Return")
end

type DoorOpening = { Across: number, Width: number, Height: number }

-- Solid posts and lintels leave the whole walking aperture clear.
local function wallWithDoors(
	name: string,
	frame: CFrame,
	width: number,
	height: number,
	doors: { DoorOpening }
)
	local left = -width / 2
	for index, door in doors do
		local begin = door.Across - door.Width / 2
		local finish = door.Across + door.Width / 2
		if begin > left then
			block(
				name .. "Post" .. index,
				frame * CFrame.new((left + begin) / 2, 0, 0),
				Vector3.new(begin - left, height, 0.5),
				FLOOR
			)
		end
		if door.Height < height then
			block(
				name .. "Lintel" .. index,
				frame * CFrame.new(door.Across, door.Height / 2, 0),
				Vector3.new(door.Width, height - door.Height, 0.5),
				FLOOR
			)
		end
		left = finish
	end
	if left < width / 2 then
		block(
			name .. "PostEnd",
			frame * CFrame.new((left + width / 2) / 2, 0, 0),
			Vector3.new(width / 2 - left, height, 0.5),
			FLOOR
		)
	end
end

-- A box with its +Z side left out, so a hole can open onto it. Returns the
-- middle of the missing side, which is where that hole's far gate goes.
local function chamber(
	name: string,
	origin: Vector3,
	span: number,
	height: number,
	colour: Color3,
	rearOpening: DoorOpening?
): Vector3
	local half = span / 2
	local mid = Vector3.new(0, height / 2, 0)

	block(
		name .. "Floor",
		CFrame.new(origin + Vector3.new(0, -0.25 + GROUND_LIFT, 0)),
		Vector3.new(span, 0.5, span),
		colour
	)
	block(name .. "Ceiling", CFrame.new(origin + Vector3.new(0, height, 0)), Vector3.new(span, 0.5, span), colour)
	local rear = CFrame.new(origin + mid - Vector3.new(0, 0, half))
	if rearOpening then
		wallWithDoors(name .. "Far", rear, span, height, { rearOpening })
	else
		block(name .. "Far", rear, Vector3.new(span, height, 0.5), WHITE)
	end
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
	block(
		name .. "Floor",
		CFrame.new(middle + Vector3.new(0, -0.25 + GROUND_LIFT, 0)),
		Vector3.new(7, 0.5, length),
		FLOOR
	)
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

	local inside = at + Vector3.new(0, 0, -away(1))
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

	local inside = at + Vector3.new(0, 0, -away(2))
	passage("LongInside", inside, 60, Color3.fromRGB(90, 150, 210))

	-- Posts down its length, because a corridor with nothing in it gives the eye
	-- no way to judge how far the end is - which is the whole claim being made.
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

	local rooms: { Vector3 } = {}
	for index = 1, 3 do
		local origin = at + Vector3.new((index - 2) * 30, 0, -away(3))
		local opening = chamber("House" .. index, origin, 18, 8, tints[index])
		marker("HouseMarker" .. index, origin, tints[index])
		rooms[index] = opening + Vector3.new(0, 0, 0.3)
	end

	wallWithDoors("HouseFacade", CFrame.new(at + Vector3.new(0, 4, 0)), 30, 8, {
		{ Across = -9.75, Width = 5, Height = 8 },
		{ Across = -3.25, Width = 5, Height = 8 },
		{ Across = 3.25, Width = 5, Height = 8 },
		{ Across = 9.75, Width = 5, Height = 8 },
	})

	for index = 1, 4 do
		-- Doors one and four share a room but have distinct return entrances.
		-- Coincident return panes would make the exit depend on iteration order.
		local room = ((index - 1) % 3) + 1
		local across = if index == 1 then -4.5 elseif index == 4 then 4.5 else 0
		link(
			mouth("HouseDoor" .. index, at + Vector3.new((index - 2.5) * 6.5, 4, 0.4), 5, 8),
			throat("HouseWay" .. index, rooms[room] + Vector3.new(across, 0, 0), 5, 8),
			"HouseDoor" .. index .. "Way"
		)
	end
end

-- --- 4. The pillar with no end --------------------------------------------
--
-- Each side leads into a different room. Their rear openings connect, so a
-- walk around the shaft includes both hidden rooms on every lap.
do
	local at = Vector3.new(STAGE_STEP * 3, 0, 0)

	block("PillarFloor", CFrame.new(at + Vector3.new(0, -0.25 + GROUND_LIFT, -6)), Vector3.new(24, 0.5, 16), FLOOR)
	block("PillarShaft", CFrame.new(at + Vector3.new(0, 5, -8)), Vector3.new(4, 10, 4), Color3.fromRGB(150, 140, 120))
	wallWithDoors("PillarBack", CFrame.new(at + Vector3.new(0, 4, -13)), 24, 8, {
		{ Across = -6, Width = 6, Height = 8 },
		{ Across = 6, Width = 6, Height = 8 },
	})

	local hues = {Color3.fromRGB(230, 150, 60), Color3.fromRGB(70, 200, 190)}
	local loops: { Part } = {}
	for index = 1, 2 do
		local origin = at + Vector3.new((index - 1.5) * 44, 0, -away(4))
		local opening = chamber(
			"Turn" .. index,
			origin,
			16,
			9,
			hues[index],
			{ Across = 0, Width = 6, Height = 8 }
		)
		marker("TurnMarker" .. index, origin + Vector3.new(4, 0, 0), hues[index])
		loops[index] = mouth("TurnLoop" .. index, origin + Vector3.new(0, 4, -7.7), 6, 8)

		link(
			mouth("PillarWay" .. index, at + Vector3.new((index - 1.5) * 12, 4, -12.6), 6, 8),
			throat("TurnWay" .. index, opening + Vector3.new(0, -0.5, 0.3), 6, 8),
			"AroundThePillar" .. index
		)
	end
	link(loops[1], loops[2], "PillarLoop")
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

	wallWithDoors("HillMouthWall", CFrame.new(at + Vector3.new(0, 3, -4)), 16, 6, {
		{ Across = 0, Width = 5, Height = 6 },
	})

	local top = at + Vector3.new(0, 14.4, -away(5))
	block(
		"HillTopFloor",
		CFrame.new(top + Vector3.new(0, -0.25 + GROUND_LIFT, -8)),
		Vector3.new(16, 0.5, 16),
		Color3.fromRGB(110, 140, 100)
	)
	block("HillTopFar", CFrame.new(top + Vector3.new(0, 3, -16.2)), Vector3.new(16, 6, 0.5), WHITE)
	marker("HillTopMarker", top + Vector3.new(0, 0, -10), Color3.fromRGB(240, 230, 140))

	link(
		mouth("HillMouth", at + Vector3.new(0, 3, -3.6), 5, 6),
		throat("HillTopMouth", top + Vector3.new(0, 3, 0), 5, 6),
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

	block("CellFloor", CFrame.new(at + Vector3.new(0, -0.25 + GROUND_LIFT, -10)), Vector3.new(20, 0.5, 20), FLOOR)

	-- The fourth side is left out to look in through, which is this exhibit's
	-- compromise: somebody who could walk in would have four of them.
	local sides = {
		{Name = "Far", At = Vector3.new(0, 4, -19.8), Wall = Vector3.new(0, 4, -20.2), Yaw = 0},
		{Name = "Left", At = Vector3.new(-9.8, 4, -10), Wall = Vector3.new(-10.2, 4, -10), Yaw = math.pi / 2},
		{Name = "Right", At = Vector3.new(9.8, 4, -10), Wall = Vector3.new(10.2, 4, -10), Yaw = -math.pi / 2},
	}

	local part = 0
	for _, side in sides do
		wallWithDoors(
			"Cell" .. side.Name,
			CFrame.new(at + side.Wall) * CFrame.Angles(0, side.Yaw, 0),
			20,
			8,
			{ { Across = 0, Width = 18, Height = 8 } }
		)
		part += 1

		-- **Turned to face into the room.** `Back` is the side an image is drawn
		-- on, so a panel in the left-hand wall needs a quarter turn to be seen
		-- from inside at all.
		local looked = mouth("CellWay" .. side.Name, at + side.At, 18, 8, side.Yaw)

		local origin = at + Vector3.new((part - 2) * 50, 0, -away(6))
		local opening = chamber("Quarter" .. part, origin, 40, 10, Color3.fromRGB(60 + part * 25, 90, 150))
		for post = 1, 4 do
			marker(
				"QuarterPost" .. part .. "x" .. post,
				origin + Vector3.new((post - 2.5) * 9, 0, -6 - (post % 2) * 7),
				Color3.fromRGB(210, 200 - post * 25, 120)
			)
		end

		link(looked, throat("QuarterWay" .. part, opening + Vector3.new(0, -1, 0.3), 18, 8), "Quarter" .. part)
	end
end

-- Where a player stands, on the plate and in front of the row.
--
-- **Without one, `scene::FindSpawn` finds nothing and starts everybody at the
-- origin** - which here is the middle of exhibit 1's tunnel mouth, on
-- `LongMouth`'s own plane with the shell's walls three studs either side. In
-- front of the row rather than in it, because these are exhibits looked *at*:
-- from here the whole plate is ahead and every mouth is a walk away.
block(
	"SpawnLocation",
	CFrame.new(Vector3.new(STAGE_STEP * 2.5, 0.5, 14)),
	Vector3.new(12, 1, 12),
	Color3.fromRGB(40, 110, 200)
)

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
local lastMode = "overview"

type TourShot = { Direction: Vector3, Near: boolean?, Away: boolean?, Roll: number? }
local shots: { TourShot } = {}
for _, elevation in { -30, 0, 30 } do
	for _, azimuth in { -175, -135, -95, -85, -45, -5, 5, 45, 85, 95, 135, 175 } do
		local yaw = math.rad(azimuth)
		local pitch = math.rad(elevation)
		table.insert(shots, {
			Direction = Vector3.new(
				math.sin(yaw) * math.cos(pitch), math.sin(pitch), math.cos(yaw) * math.cos(pitch)
			),
		})
	end
end
for _, shot: TourShot in {
	{ Direction = Vector3.new(0, 0, 1) },
	{ Direction = Vector3.new(0, 0, -1) },
	{ Direction = Vector3.new(1, 0.25, 2).Unit, Roll = 0.4 },
	{ Direction = Vector3.new(0.35, 0.1, 0.02), Near = true },
	{ Direction = Vector3.new(0.35, 0.1, -0.02), Near = true },
	{ Direction = Vector3.new(0.6, 0.2, 1).Unit },
	{ Direction = Vector3.new(0.6, 0.2, -1).Unit },
	{ Direction = Vector3.new(0, 0, 1), Away = true },
	{ Direction = Vector3.new(0, 0, -1), Away = true },
} do
	table.insert(shots, shot)
end

-- These attributes are editable in the inspector. A nonnegative sample seeks
-- one fixed shot; pause holds automatic playback and step advances one shot.
view:SetAttribute("TourMode", "overview")
view:SetAttribute("TourSample", -1)
view:SetAttribute("TourPaused", false)
view:SetAttribute("TourStep", 0)
view:SetAttribute("TourCount", #shots * #entrances)

local function angleShot(sample: number)
	local index = sample % (#shots * #entrances)
	local pane = entrances[math.floor(index / #shots) + 1]
	local shot = shots[index % #shots + 1]
	local size = pane.Size
	local frame = pane.CFrame
	local centre = frame:PointToWorldSpace(Vector3.new(0, 0, size.Z / 2))
	local offset = if shot.Near
		then Vector3.new(shot.Direction.X * size.X, shot.Direction.Y * size.Y, shot.Direction.Z)
		else shot.Direction * math.max(size.X, size.Y)
	local eye = frame:PointToWorldSpace(Vector3.new(0, 0, size.Z / 2) + offset)
	local toward = if shot.Away then eye + (eye - centre) else centre
	local pose = CFrame.lookAt(eye, toward) * CFrame.Angles(0, 0, shot.Roll or 0)
	if not view.CFrame:FuzzyEq(pose, 1e-6) then
		-- A seek is a camera cut. It cannot masquerade as a walk through every
		-- portal between two independently authored shots.
		view:CutTo(pose)
	end
	if view:GetAttribute("TourPane") ~= pane.Name then
		view:SetAttribute("TourPane", pane.Name)
	end
end

RunService.Heartbeat:Connect(function(deltaTime: number)
	-- Play owns its humanoid camera. The preview tour never takes it back.
	local current = workspace.CurrentCamera
	if current == nil or not current:Equals(view) then
		return
	end
	if view:GetAttribute("TourPaused") ~= true then
		elapsed += deltaTime
	end
	if view:GetAttribute("TourMode") == "angles" then
		local count = #shots * #entrances
		local selected = view:GetAttribute("TourSample")
		local sample = if typeof(selected) == "number" and selected >= 0 and selected < math.huge
			then math.floor(selected) % count else math.floor(elapsed * 2) % count
		local step = view:GetAttribute("TourStep")
		if typeof(step) == "number" and step == step and math.abs(step) < math.huge then
			sample += math.floor(step) % count
		end
		angleShot(sample)
		lastMode = "angles"
		return
	end

	-- A sweep along the row and back, seen from three-quarters on.
	--
	-- **Not square on, and that is what makes half of these legible.** The first
	-- two exhibits are about how long a tunnel *looks* from outside, which a
	-- camera staring straight down the mouth cannot show - it sees the hole and
	-- none of the shell. Standing off to one side puts the outside and the
	-- inside in one frame, which is the comparison every one of them is making.
	local travelled = elapsed * PACE
	local lap = travelled % (LAST * 2)
	local along = if lap > LAST then LAST * 2 - lap else lap
	local sway = math.sin(travelled * 0.05) * 6

	local pose = CFrame.lookAt(
		Vector3.new(along + 15 + sway, 8 + math.sin(travelled * 0.031) * 2, 21),
		Vector3.new(along, 3.5, -7)
	)
	if lastMode == "angles" then
		view:CutTo(pose)
	else
		view.CFrame = pose
	end
	lastMode = "overview"
end)

print(
	`non-euclidean: 6 exhibits, {holes} holes - shorter inside, longer inside, a room that is not there, a pillar with no end, a hill climbed downwards, and four times the volume in one room`
)
