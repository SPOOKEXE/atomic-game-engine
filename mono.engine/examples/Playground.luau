--!strict
-- A floor, a spawn pad and things to jump on. The world the character was built
-- for.
--
-- **Every scene this repository shipped before v0.14 was a scene to look at.**
-- The slide runs itself, the mirrors reflect whatever orbits past them, the
-- portals are walked through by a camera on rails. None of them has a floor a
-- person can stand on, and none has anywhere to put a player - so the first
-- character had nowhere to be.
--
-- This one is deliberately dull, and that is the point:
--
--   * a floor wide enough that walking off it takes a while;
--   * a `SpawnLocation`, which is what `scene::FindSpawn` looks for;
--   * steps, because a jump that clears nothing proves nothing;
--   * a wall, because a character that walks through geometry looks exactly
--     like one that does not until it meets some.
--
-- **It is also the local-server demo.** `scripts/demos/run-local-server.sh`
-- hosts this file and connects two clients to it, which is the smallest
-- arrangement that shows a character walking on one screen and moving on
-- another.

local FLOOR_SIZE = 240
local FLOOR_THICKNESS = 4

local workspace = game:GetService("Workspace")

-- Colours picked so the three kinds of surface are told apart without reading
-- anything: ground, the thing you spawn on, the things you climb.
local GROUND = Color3.new(0.24, 0.28, 0.24)
local PAD = Color3.new(0.15, 0.45, 0.85)
local STEP = Color3.new(0.55, 0.42, 0.28)
local WALL = Color3.new(0.35, 0.35, 0.40)

local function block(name: string, size: Vector3, at: Vector3, colour: Color3): Part
	local part = Instance.new("Part")
	part.Name = name
	part.Size = size
	part.Position = at
	part.Color = colour

	-- **Anchored, all of it.** Scenery the solver may move is scenery that ends
	-- up somewhere else after a character has leaned on it for a minute, and an
	-- anchored part carries no rigid body at all - so this is a cheaper world as
	-- well as a stiller one.
	part.Anchored = true
	part.Parent = workspace
	return part
end

-- Clear of a baseplate's top face at y = 0: coplanar surfaces do not flicker in
-- this engine, they resolve stably to whichever one the draw order happens to
-- favour. `scripts/demos/zfight-scan.sh` holds the control for that.
local GROUND_LIFT = 0.01

block(
	"Ground",
	Vector3.new(FLOOR_SIZE, FLOOR_THICKNESS, FLOOR_SIZE),
	Vector3.new(0, -FLOOR_THICKNESS / 2 + GROUND_LIFT, 0),
	GROUND
)

-- **A part still *named* `SpawnLocation`, which v0.15 kept working on purpose.**
-- `SpawnLocation` is a class now - `Instance.new("SpawnLocation")` gives one
-- with a `TeamColor`, a `Neutral` flag and an `Enabled` flag - and a plain block
-- wearing the name is read as an enabled, neutral spawn so that every scene
-- written before the class keeps spawning people. `scene::FindSpawn` puts a
-- character on the top face; a world with no spawn at all starts everybody at
-- the origin, which here is inside the floor.
block("SpawnLocation", Vector3.new(12, 1, 12), Vector3.new(0, 0.5, 0), PAD)

-- A staircase of four, each one a little under the jump height, so walking up it
-- works and so does jumping the whole run.
for index = 1, 4 do
	local height = index * 2
	block(
		"Step" .. index,
		Vector3.new(10, height, 6),
		Vector3.new(-24, height / 2, -10 - index * 8),
		STEP
	)
end

-- Two towers far enough apart to be a target rather than a wall.
block("TowerA", Vector3.new(8, 24, 8), Vector3.new(40, 12, 40), WALL)
block("TowerB", Vector3.new(8, 36, 8), Vector3.new(62, 18, 40), WALL)

-- A wall to walk into. **The one piece of geometry here that is a test rather
-- than scenery**: a character controller that replaces horizontal velocity
-- every tick will happily push itself through a thin box if the solver is not
-- given something to solve, and this is where that shows.
block("Wall", Vector3.new(60, 14, 2), Vector3.new(0, 7, 60), WALL)
