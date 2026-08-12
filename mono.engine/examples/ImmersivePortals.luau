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
-- ## Both halves cross, and they cross by different routes
--
-- **Traversal** hands you to `TeleportService`, which is `world::Postbox::
-- Teleport` — the bus that moves a player between *worlds* rather than between
-- two places in one. You arrive in front of the other demo's block and the
-- brick that greets you is the other colour.
--
-- **The image** is the other world too, and getting there took a renderer
-- change. `client::CollectSurfaceViews` takes one `Store` and the surface pass
-- drew "the same scene range" as the main pass, so a surface camera could only
-- ever render the world it stood in. Now `render::SurfaceView` carries an
-- instance range and `client::AttachForeignSurfaces` — which runs in the host,
-- because only the host holds both worlds — gathers the far world's draw list
-- and points the pane at it.
--
-- **Beside this world's list and never joined to it**, which is the whole of
-- `Renderer::Render`'s `foreign` argument. Joined, the far world was culled
-- against this camera, sorted into this scene's plan and submitted by the main
-- pass — so both rooms drew on top of each other in every viewport, the other
-- world's character stood in this one, and the range the pane had been given
-- named whatever the sort had moved into those slots. One list, three symptoms.
--
-- **And whoever is standing in the pane goes with it.** The far world holds no
-- copy of a body that is still in this one, so a character halfway through was
-- whole in the room it was leaving and absent from the picture of the room it
-- was entering. `scene::AppendPortalClones` is the same answer a same-world
-- pane gets in `Portals-1-world.luau`, asked across the boundary.
--
-- **Two fields, and the split is the point.** `Portal.Destination` is a part in
-- *this* world and decides where the camera stands; `Portal.DestinationWorld` is
-- a name and decides whose instances are drawn through it. The first is
-- arithmetic a store can do for itself and the second is a draw list across a
-- boundary rule 3 keeps shut — so one is a handle and the other is a name, which
-- is the same shape a teleport already uses.
--
-- The stand-in below sits where the far world's block sits. That is exact
-- because both worlds are laid out the same way; a destination laid out
-- somewhere else wants its offset baked into the stand-in, which is the same lie
-- a same-world portal already tells and is `NON-EUCLIDEAN.md`'s subject.
--
-- **What a foreign pane does not do is recurse.** A surface drawn from another
-- world is one plain run — the far world's own panes show flat rather than as
-- images of a third world — because the `scene::ScenePlan` that partitions
-- mirrors describes one world and knows nothing about the appended tail.

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

-- **And whose world is drawn through it.** `Portal.Destination` decides where
-- the camera stands and `DestinationWorld` decides what it sees — the first is
-- arithmetic inside this store and the second is a draw list on the far side of
-- a boundary no store may reach across, which is why they are two fields and why
-- the host resolves the second. `client::AttachForeignSurfaces` is that host
-- step.
--
-- The stand-in above sits where the other demo's block sits, because both worlds
-- are laid out the same way — so the mapping the camera is placed through is
-- already the right one and only the *contents* change.
portal.DestinationWorld = DESTINATION

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

-- --- arriving ------------------------------------------------------------------

-- **Where a body that came through the hole belongs, which is not the spawn
-- pad.** A same-world portal keeps the crossing continuous — `scene::
-- CrossPortals` maps the body through `destination · half-turn · source⁻¹`, so
-- you come out of the far pane where you went into the near one, still walking.
-- A cross-world one is a `TeleportService` call, and a teleport has always
-- landed people on `SpawnLocation`: you walk into a hole and appear fourteen
-- metres away facing a wall, which reads as the portal not working at all.
--
-- The map here is the identity, and that is not a shortcut. Both worlds are laid
-- out the same way with the block at the same place, so "where you were relative
-- to the block" is the whole of the crossing — the same offset, the same side
-- carried on through. A destination laid out differently wants its own offset
-- baked in, which is `NON-EUCLIDEAN.md`'s subject and is a scene's
-- business rather than the engine's.
--
-- **Pushed clear of the plane on arrival.** Landing exactly where you crossed
-- would put you inside the far block's trigger box, and the next heartbeat
-- would send you straight back — a portal that bounces you between two worlds
-- for ever. `THROUGH` is the width of that box, so a little past it is the
-- first place that is unambiguously *through*.
local CLEAR = THROUGH + 1.5

local function placeArrival(player: Player)
	local data = TeleportService:GetTeleportData(player)
	if typeof(data) ~= "table" then
		-- Somebody who joined rather than crossed. The spawn pad is right for
		-- them and this is not an error.
		return
	end

	local carried = data :: { Side: number?, Across: number?, Height: number? }
	local side = carried.Side
	local across = carried.Across
	local height = carried.Height
	if side == nil or across == nil or height == nil then
		return
	end

	local character = workspace:FindFirstChild(player.Name)
	if character == nil then
		return
	end

	local root = character:FindFirstChild("HumanoidRootPart") :: BasePart?
	if root == nil then
		return
	end

	-- **Out of the far side, keeping the way you were going.** `Side` is which
	-- way the walker was travelling on the block's thin axis, so continuing it
	-- puts them out of the face they would have come out of.
	root.CFrame = CFrame.lookAt(
		block.Position + Vector3.new(across, height, CLEAR * side),
		block.Position + Vector3.new(across, height, CLEAR * side * 2)
	)
end

-- **Polled rather than hooked, because the body arrives after the player
-- does.** `Players.PlayerAdded` fires from the parenting the engine does in
-- `AdmitArrival`, and the character is loaded by the host a moment later — so a
-- handler that reached for `HumanoidRootPart` on that signal would find nothing
-- on the frame that matters. This asks for one tick, once, per arrival.
local pending: { Player } = {}

Players.PlayerAdded:Connect(function(joined: Instance)
	-- Cast, for `FindFirstChild`'s reason — a signal hands over an `Instance`
	-- and this one only ever fires with a `Player`.
	table.insert(pending, joined :: Player)
end)

-- --- walking through ----------------------------------------------------------

RunService.Heartbeat:Connect(function()
	for index = #pending, 1, -1 do
		local player = pending[index]
		if workspace:FindFirstChild(player.Name) ~= nil then
			placeArrival(player)
			table.remove(pending, index)
		end
	end

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
					-- **What the far side needs to put them down.** Where they
					-- were across the block, how high, and which way they were
					-- going through it — the crossing, in the block's own terms,
					-- which both worlds share because both blocks are placed the
					-- same. The player is removed from this world by the call,
					-- so the next heartbeat does not find them and send them
					-- twice.
					TeleportService:Teleport(DESTINATION, player, {
						From = THIS_WORLD,
						Across = gap.X,
						Height = gap.Y,
						-- **Which side they came from, inverted, because that is
						-- the side they are going out of.** A `CFrame` in a
						-- script has no look vector to ask — see
						-- `script::Values`' `CFrameIndex` — and the approach
						-- side is the better question anyway: the trigger box
						-- is three metres deep and a walker covers a quarter of
						-- that in a tick, so the first frame inside it is
						-- always on the side they came in from.
						Side = if gap.Z > 0 then -1 else 1,
					})
				end
			end
		end
	end
end)
