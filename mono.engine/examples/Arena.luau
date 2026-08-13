--!strict
-- The other end of a teleport, and a ring to stand in.
--
-- **A second world exists so that `TeleportService` has somewhere to go.** The
-- Teleport bus has been able to move a payload between two worlds since v0.2 and
-- the studio has run several worlds in parallel since then — and there were no
-- two worlds a player could be in, because there were no players. A teleport
-- with one world is a delete.
--
-- Paired with `Playground.luau`. `ArenaPad.luau` is the pad back; it is a
-- separate file for the reason `PlaygroundPad.luau` gives — a scene that names a
-- destination is a scene that only works where that destination exists, and the
-- playground is also what `scripts/demos/run-local-server.sh` hosts on its own.

local ARENA_RADIUS = 60
local WALL_HEIGHT = 12

local workspace = game:GetService("Workspace")

local FLOOR = Color3.new(0.30, 0.24, 0.22)
local PAD = Color3.new(0.85, 0.45, 0.15)
local WALL = Color3.new(0.42, 0.34, 0.30)

local function block(name: string, size: Vector3, at: Vector3, colour: Color3): Part
	local part = Instance.new("Part")
	part.Name = name
	part.Size = size
	part.Position = at
	part.Color = colour
	part.Anchored = true
	part.Parent = workspace
	return part
end

block("Ground", Vector3.new(ARENA_RADIUS * 2, 4, ARENA_RADIUS * 2), Vector3.new(0, -2, 0), FLOOR)

-- Where an arriving character stands. The name is the contract:
-- `scene::FindSpawn` looks for exactly this under `Workspace`.
block("SpawnLocation", Vector3.new(14, 1, 14), Vector3.new(0, 0.5, 0), PAD)

-- Four walls, so the arena reads as a room rather than as a slab. Set back from
-- the edge, because a wall exactly on the boundary of the floor is a wall you
-- can fall past.
local INSET = ARENA_RADIUS - 4
block("WallNorth", Vector3.new(INSET * 2, WALL_HEIGHT, 2), Vector3.new(0, WALL_HEIGHT / 2, -INSET), WALL)
block("WallSouth", Vector3.new(INSET * 2, WALL_HEIGHT, 2), Vector3.new(0, WALL_HEIGHT / 2, INSET), WALL)
block("WallWest", Vector3.new(2, WALL_HEIGHT, INSET * 2), Vector3.new(-INSET, WALL_HEIGHT / 2, 0), WALL)
block("WallEast", Vector3.new(2, WALL_HEIGHT, INSET * 2), Vector3.new(INSET, WALL_HEIGHT / 2, 0), WALL)

-- Something to jump onto, so the arena is worth walking around in.
for index = 1, 6 do
	local angle = (index / 6) * math.pi * 2
	block(
		"Pillar" .. index,
		Vector3.new(6, 4 + index, 6),
		Vector3.new(math.cos(angle) * 24, (4 + index) / 2, math.sin(angle) * 24),
		WALL
	)
end
