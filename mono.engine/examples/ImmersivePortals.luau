--!strict
-- Two worlds, one portal block each, and a hole you walk through rather than into.
--
-- `Portals-1-world.luau` is this scene's sibling and proves the *picture*: three
-- rooms in one world, panes that show somewhere the wall does not, and the one
-- matrix — `destination · source⁻¹` — that the whole feature rests on. This file
-- is about the other half, **being in the hole**, and it is deliberately the
-- smallest scene that can show it: a floor, one block, one brick.
--
-- ## What was wrong, and it was not the maths
--
-- A pane is an ordinary `Part` and an ordinary part collides. So the solver
-- stopped a character dead on the surface of every portal in the engine, and
-- `scene::CrossPortals` — which asks whether the segment a body covered this
-- tick changes sign through the plane — could never fire for a body parked *on*
-- that plane. Traversal was implemented, tested and unreachable. The wall in
-- front of the feature was the pane.
--
-- `scene::OpenPortals` is the rule that removes it: a part with a `Portal` on it
-- has its collider turned into a trigger, so contacts are still reported and no
-- impulse is solved. That is what lets a body **straddle** the plane — half in
-- each space, which is the frame everybody screenshots and the thing a picture
-- of a hole cannot do.
--
-- ## What each world is
--
-- One block, and a floating brick in front of it in a colour that is this
-- world's alone. The brick is the whole navigational apparatus: it is what tells
-- you which side you came out on, and it is placed clear of the block so it
-- reads from either approach. Demo 1 is warm, demo 2 is cool — an ordering a
-- person can name from a screenshot, which is the property `Mirrors-4-worlds`
-- chose its palette for.
--
-- Kept deliberately bare otherwise. A room with furniture in it is a room, and
-- what is under test here is a single surface and whether you can be inside it.
--
-- ## Where the two halves meet, and where they do not yet
--
-- **Traversal is cross-world and works**: walking into the block hands you to
-- `TeleportService`, which is `world::Postbox::Teleport` — the bus that moves a
-- player between *worlds* rather than between two places in one. You arrive in
-- front of the other demo's block, facing back at it, and the brick that greets
-- you is the other colour.
--
-- **The image in the pane is this world's**, and that is the piece that is not
-- finished. `client::CollectSurfaceViews` takes one `Store`, and the renderer's
-- surface pass draws "the same scene range" as the main pass — so a surface
-- camera can only ever render the world it is standing in. Showing world 2
-- through world 1's pane means uploading a second world's instances in one frame
-- and giving the surface pass its range, which is a renderer change and not a
-- script one. Until then the pane is aimed at this world's own far side, so the
-- block reads as a hole rather than as a slab, and the *arrival* is what crosses.
--
-- Said here rather than left to be discovered, because a demo whose caption
-- overstates it is worse than one that does not exist.

local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
local TeleportService = game:GetService("TeleportService")
local workspace = game:GetService("Workspace")

-- --- which of the two this is -------------------------------------------------

local DEMO_ONE = "immersive-portals-demo-1"
local DEMO_TWO = "immersive-portals-demo-2"

-- **The world's own name decides everything below**, which is the arrangement
-- `Mirrors-4-worlds.luau` uses and for the same reason: one file that both
-- worlds run cannot drift out of agreement with itself, and two files that
-- mirror each other by hand always eventually do.
local THIS_WORLD = game.JobId
local IS_SECOND = string.find(THIS_WORLD, DEMO_TWO, 1, true) ~= nil

local DESTINATION = IS_SECOND and DEMO_ONE or DEMO_TWO

-- Warm for the first, cool for the second. Distinct enough to name out loud,
-- which is the only requirement a wayfinding colour has.
local BRICK_COLOUR = IS_SECOND and Color3.new(0.20, 0.55, 0.95) or Color3.new(0.95, 0.45, 0.12)

-- --- the room -----------------------------------------------------------------

local FLOOR_SIZE = 60
local FLOOR_THICKNESS = 2

-- **Thin, because it is a hole and not a doorway.** A block deep enough to stand
-- inside would put the body between two faces rather than across one plane, and
-- which face it crossed would stop being a question with one answer.
local BLOCK_SIZE = Vector3.new(10, 8, 0.4)
local BLOCK_AT = Vector3.new(0, 4, 0)

-- How close to the block's plane counts as going through it, in metres.
--
-- **Measured on the axis the block is thin on**, not as a radius: a sphere
-- around the block's centre would also catch somebody walking past its edge,
-- which is the one place a portal must not take you.
local THROUGH = 1.5

-- Half the block's width plus a little, so walking round the end is possible
-- and is not mistaken for going through.
local WITHIN = BLOCK_SIZE.X / 2

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Size = Vector3.new(FLOOR_SIZE, FLOOR_THICKNESS, FLOOR_SIZE)
floor.Position = Vector3.new(0, -FLOOR_THICKNESS / 2, 0)
floor.Color = IS_SECOND and Color3.new(0.16, 0.18, 0.22) or Color3.new(0.22, 0.20, 0.17)
floor.Anchored = true
floor.Parent = workspace

-- **Named `SpawnLocation`, because that is what `scene::FindSpawn` looks for.**
-- A character loaded into a world with none starts at the origin, which here is
-- inside the floor.
local spawn = Instance.new("Part")
spawn.Name = "SpawnLocation"
spawn.Size = Vector3.new(10, 1, 10)
spawn.Position = Vector3.new(0, 0.5, 14)
spawn.Color = BRICK_COLOUR
spawn.Anchored = true
spawn.Parent = workspace

-- The block itself. **Anchored and left collidable in the script**, because
-- `scene::OpenPortals` is what opens it — writing `CanCollide = false` here
-- would hide whether the engine rule works, and this scene is one of the two
-- places that would notice if it stopped.
local block = Instance.new("Part")
block.Name = "PortalBlock"
block.Size = BLOCK_SIZE
block.Position = BLOCK_AT
block.Color = Color3.new(0.85, 0.85, 0.9)
block.Anchored = true
block.Parent = workspace

-- Something for the pane to be aimed at. See the header: the image is this
-- world's own until a surface camera can render another's.
--
-- **Placed just behind the block, so the mapping is very nearly the identity**
-- and what the pane shows is what is actually behind it. That makes the block
-- read as an opening rather than as a slab with a picture on it — which is the
-- whole complaint a portal you cannot walk into produces, and it should not
-- still look like one now that you can. A destination further away would be the
-- bigger-on-the-inside trick instead; `Portals-1-world.luau` is the scene for
-- that, and it is a different demonstration.
local beyond = Instance.new("Part")
beyond.Name = "Beyond"
beyond.Size = BLOCK_SIZE
beyond.Position = BLOCK_AT + Vector3.new(0, 0, -0.6)
beyond.Color = block.Color
beyond.Transparency = 1
beyond.CanCollide = false
beyond.Anchored = true
beyond.Parent = workspace

local portal = Instance.new("Portal")
portal.Name = "Hole"
portal.Face = Enum.NormalId.Front
-- Width then height, in texels. Wider than tall because the block is, and a
-- square target on a wide pane spends half its texels on nothing.
portal.SurfaceSize = Vector3.new(1024, 512)
portal.ImageTransparency = 0
portal.Destination = beyond
portal.Parent = block

-- --- the brick that says where you are ----------------------------------------

-- **In front of the block and off to one side of the walk-through line**, so it
-- is visible on approach and does not become an obstacle in the doorway. Floating
-- rather than standing: a brick on the floor is scenery and a brick in the air is
-- a marker, and this one is doing the second job.
local brick = Instance.new("Part")
brick.Name = IS_SECOND and "CoolBrick" or "WarmBrick"
brick.Size = Vector3.new(3, 3, 3)
brick.Position = BLOCK_AT + Vector3.new(0, 3.5, 7)
brick.Color = BRICK_COLOUR
brick.Anchored = true
brick.Parent = workspace

-- A second one behind the block in the same colour, because the point of the
-- brick is that it names the world — and a marker you can only see from one
-- approach names it only half the time.
local far = Instance.new("Part")
far.Name = brick.Name .. "Far"
far.Size = brick.Size
far.Position = BLOCK_AT + Vector3.new(0, 3.5, -7)
far.Color = BRICK_COLOUR
far.Anchored = true
far.Parent = workspace

-- --- never asleep -------------------------------------------------------------

-- **Both worlds tick whether or not anybody is standing in them.** The studio
-- suspends a world nobody is playing and the server closes one that goes empty —
-- `world::DecideLifecycle` is the one policy both share — and either is correct
-- in general and wrong for exactly this scene: the whole demonstration is that
-- the *other* world is live while you are looking at it from this one. A frozen
-- destination is a picture, which is the thing this file exists to stop being.
--
-- `KeepWorldAwake` is the supported way to say so, and it takes a reason because
-- a world held open with no explanation is the hardest kind of leak to find.
workspace:KeepWorldAwake("immersive portals: the other end must be live")

-- --- walking through ----------------------------------------------------------

-- **Only the authority sends anybody anywhere.** A replica running this would be
-- a client deciding who is in which world, which `TeleportService` refuses — so
-- this is the refusal made quiet rather than an error every tick.
if not RunService:IsServer() then
	return
end

RunService.Heartbeat:Connect(function()
	for _, player in Players:GetPlayers() do
		local character = workspace:FindFirstChild(player.Name)
		if character then
			-- Cast, because `FindFirstChild` is typed `Instance` — D00114.
			local root = character:FindFirstChild("HumanoidRootPart") :: BasePart?
			if root then
				local gap = root.Position - block.Position

				-- **Through the plane and inside the rectangle**, which is the
				-- same two questions `scene::CrossPortals` asks of a same-world
				-- pane. Being near the block is not enough: walking round its
				-- end must leave you in the world you were in.
				local through = math.abs(gap.Z) < THROUGH
				local within = math.abs(gap.X) < WITHIN and math.abs(gap.Y) < BLOCK_SIZE.Y / 2 + 2

				if through and within then
					-- The player is removed from this world by the call, so the
					-- next heartbeat does not find them and send them twice.
					TeleportService:Teleport(DESTINATION, player, { From = THIS_WORLD })
				end
			end
		end
	end
end)
