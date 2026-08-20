--!strict
-- The world `just stress` hosts: a floor, a spawn, and room for everybody.
--
-- **Deliberately barer than `Playground.luau`, and every omission is the
-- point.** A load test is a measurement of the server's replication and
-- simulation cost per client, so the scenery is whatever a character needs to
-- stand on and nothing else - steps, towers and a wall would put geometry in
-- every client's interest set and measure the broadphase instead.
--
-- What it does have that no other scene does is `MaxPlayers` raised past the
-- default fifty. `scene::AddPlayer` refuses past that number and the host says
-- so once per refusal, so two hundred clients against `Playground.luau` are
-- fifty players and a hundred and fifty spectators - which measures the
-- streaming path and never the input path.
--
--   just stress
--   just host --game .cache/build/release/assets/examples/Stress.luau --listen 9099

local FLOOR_SIZE = 400
local FLOOR_THICKNESS = 4

local workspace = game:GetService("Workspace")
local players = game:GetService("Players")

-- Above the client count `just stress` opens, so the ceiling is the flag on the
-- harness rather than a number in here.
players.MaxPlayers = 512

local function block(name: string, size: Vector3, at: Vector3, colour: Color3): Part
	local part = Instance.new("Part")
	part.Name = name
	part.Size = size
	part.Position = at
	part.Color = colour

	-- Anchored, for `Playground.luau`'s reason: an anchored part carries no
	-- rigid body, so the floor is not something the solver has to hold still
	-- under two hundred characters standing on it.
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
	Color3.new(0.24, 0.28, 0.24)
)

-- **One spawn, so everybody starts in the same place.** They walk apart
-- immediately - the harness gives each client a different heading - and starting
-- together is what makes the first seconds of the run the interesting ones:
-- every client is inside every other client's interest radius at once, which is
-- the worst case the priority scheduler has.
block("SpawnLocation", Vector3.new(24, 1, 24), Vector3.new(0, 0.5, 0), Color3.new(0.15, 0.45, 0.85))
