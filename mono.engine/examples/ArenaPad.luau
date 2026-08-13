--!strict
-- The pad back, in the arena.
--
-- **The twin of `PlaygroundPad.luau`, and separate from `Arena.luau` for the
-- same reason.** A scene that names a destination only works where that
-- destination exists, and `TeleportService` destroys the player in the world
-- they left before the router discovers there is nowhere to put them — so a
-- pad baked into a world is a character that vanishes wherever that world is
-- hosted alone. Geometry in one file, destination in another.
--
-- The distance check and the server guard are `PlaygroundPad.luau`'s; that file
-- carries the argument for both.

local DESTINATION = "Playground"

local PAD_AT = Vector3.new(0, 0.5, -34)
local PAD_SIZE = Vector3.new(12, 1, 12)

-- How close counts as standing on it, in metres. **Measured from the pad's
-- centre to the character's centre**, and a character is five metres tall — so
-- this is comfortably more than half the pad's width and less than the distance
-- to anything else worth standing on.
local REACH = 8

local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
local workspace = game:GetService("Workspace")

local pad = Instance.new("Part")
pad.Name = "PlaygroundPad"
pad.Size = PAD_SIZE
pad.Position = PAD_AT
pad.Color = Color3.new(0.85, 0.45, 0.15)
pad.Anchored = true
pad.Parent = workspace

-- A post, so the pad is visible from across the floor rather than being a
-- differently coloured tile you find by accident.
local post = Instance.new("Part")
post.Name = "PlaygroundPost"
post.Size = Vector3.new(1, 10, 1)
post.Position = PAD_AT + Vector3.new(0, 5, 0)
post.Color = pad.Color
post.Anchored = true
post.Parent = workspace

-- **Only the authority sends anybody anywhere.** A replica running this would
-- be a client deciding who is in which world, which `TeleportService` refuses
-- anyway — this is the refusal made quiet rather than an error every tick.
if not RunService:IsServer() then
	return
end

RunService.Heartbeat:Connect(function()
	for _, player in Players:GetPlayers() do
		local character = workspace:FindFirstChild(player.Name)
		if character then
			-- **Cast, because `FindFirstChild` is typed `Instance`.** The script
			-- editor and `luau-lsp` infer nothing about what a lookup returned —
			-- `docs/DEFERRED.md` D00114 — so a part fetched by name has to be
			-- told what it is before its `Position` can be read.
			local root = character:FindFirstChild("HumanoidRootPart") :: BasePart?
			if root then
				local gap = root.Position - pad.Position
				if gap.Magnitude < REACH then
					-- The player is removed from this world by the call, so the
					-- next heartbeat does not find them and send them twice.
					TeleportService:Teleport(DESTINATION, player, { From = "Arena" })
				end
			end
		end
	end
end)
