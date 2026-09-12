--!strict
-- Twenty thousand moving parts served through the real replication path.
--
-- All parts stay inside one client's interest window. Each Heartbeat writes
-- every Position, so the steady state measures dirty-row collection, priority,
-- packing, encryption, transport, replica application, and the script boundary.
--
--   just preset=release stress-motion

local OBJECT_COUNT = 20_000
local COLUMNS = 200
local SPACING = 0.55
local HALF_TRAVEL = 3.0
local SPEED = 5.0

local workspace = game:GetService("Workspace")
local players = game:GetService("Players")
local RunService = game:GetService("RunService")

players.MaxPlayers = 16

local movers: {Instance} = {}
local positions: {number} = {}
local directions: {number} = {}
local heights: {number} = {}
local depths: {number} = {}

-- The placements `BulkMoveTo` takes, beside `movers`. Overwritten in place every
-- beat rather than rebuilt: at this object count a fresh table a tick is real
-- allocation on top of the moves.
local placements: {CFrame} = {}
local built = false

local function buildMovers()
	if built then
		return
	end

	for index = 1, OBJECT_COUNT do
		local column = (index - 1) % COLUMNS
		local row = math.floor((index - 1) / COLUMNS)
		local x = (column - (COLUMNS - 1) / 2) * SPACING
		local z = (row - 49.5) * SPACING
		local y = 2.0 + ((index - 1) % 8) * 0.45

		local part = Instance.new("Part")
		part.Name = "Mover"
		part.Size = Vector3.new(0.35, 0.35, 0.35)
		part.Position = Vector3.new(x, y, z)
		part.Color = Color3.new(0.15 + (index % 5) * 0.12, 0.55, 0.9)
		part.Anchored = true
		part.Parent = workspace

		movers[index] = part
		placements[index] = part.CFrame
		positions[index] = x
		directions[index] = if index % 2 == 0 then 1 else -1
		heights[index] = y
		depths[index] = z
	end

	built = true
end

players.PlayerAdded:Connect(function(player: Instance)
	-- Exercises the per-player path while leaving the world-wide option free for
	-- hosts to configure independently. Building after admission also lets the
	-- first small snapshot complete before the 20,000-row stream begins.
	(player :: Player).LocalSimulatedNetworkLatency = 40
	buildMovers()
end)

RunService.Heartbeat:Connect(function(deltaTime: number)
	if not built then
		return
	end
	local step = SPEED * deltaTime
	for index = 1, OBJECT_COUNT do
		local x = positions[index] + directions[index] * step
		local centre = ((index - 1) % COLUMNS - (COLUMNS - 1) / 2) * SPACING
		if x > centre + HALF_TRAVEL then
			x = centre + HALF_TRAVEL
			directions[index] = -1
		elseif x < centre - HALF_TRAVEL then
			x = centre - HALF_TRAVEL
			directions[index] = 1
		end

		positions[index] = x
		placements[index] = CFrame.new(x, heights[index], depths[index])
	end

	-- **One call for the whole field.** This demo exists to put a large delta on
	-- the wire every tick, and what it used to spend getting there was twenty
	-- thousand crossings of the language boundary - which is the cost
	-- `scene::BulkMoveTo` removes, and not the replication this is measuring.
	workspace:BulkMoveTo(movers, placements)
end)
