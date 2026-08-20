--!strict
-- Two worlds, one pane each, and a camera standing where a player spawns.
--
-- **The smallest scene that can be captured to prove a cross-world pane shows
-- the right half of the world it names.** `ImmersivePortals.luau` is the demo
-- and needs the studio - two runs, a play link, a client. This one needs
-- `client --worlds 2 --script CrossWorldSeam.luau`, which builds both worlds in
-- one process from one file and renders the first, so a still can be taken of
-- exactly the thing the report is about.
--
-- ## What it is arranged to catch
--
-- A hole's camera is placed by `scene::SeamMapping`: one map per pane, carrying
-- its front hemisphere to the far pane's back one and its back to the far pane's
-- front. `AimSurfaceCameras` used to compose its own instead, out of a source
-- frame that **flipped with the viewer's side** - which is the pre-v0.15 shape
-- and is two maps for one pane. A same-world hole stopped noticing when the
-- recursive pass took its picture over. A cross-world pane still draws from
-- there, so it kept the bug, and every scene like this one spawns you *behind*
-- the pane, which is the side the two maps differ on.
--
-- So the check is a colour:
--
--   * the camera stands on the **pad side**, looking at the pane;
--   * the pane must show the **other** world's pad and near marker;
--   * that world is the other colour, so a correct picture has a slab of the
--     wrong colour inside the glass and a matching one outside it.
--
-- With the map wrong the camera lands on the other side of the far pane looking
-- the other way, and the pane shows the far half of that world instead: no pad,
-- no near marker, just the floor and the one marker behind the block. That is
-- exactly the report - "the other world does not replicate objects, only the
-- floor".
--
-- ## What it turned out to catch as well, and it is one flag apart
--
-- Run the same scene twice:
--
--     client --worlds 1 --script CrossWorldSeam.luau --frames 40 --capture a.bmp
--     client --worlds 2 --view-spacing 0 --script CrossWorldSeam.luau --frames 40 --capture b.bmp
--
-- With **one** world the pane renders: its destination names a world that is not
-- there, so it falls back to showing its own - and the pad and the marker are
-- plainly in the glass. With **two** the same pane draws its own material, flat,
-- and so does the plain mirror beside it. Two panes, two slots, both blank, and
-- the only thing that changed is how many worlds are being composited.
--
-- **And the difference is one branch, measured in pixels.** Sample the pane in
-- both stills: with one world it is `(11, 13, 21)` and `(59, 42, 28)` - the
-- sub-render's own sky and this world's floor, so an image. With two it is a
-- uniform `(141, 145, 167)`, which is the pane's material lit, so no image at
-- all.
--
-- The renderer takes a different path for exactly that reason. With no
-- destination present `SurfaceView::InstanceCount` is zero and the surface pass
-- draws `plan.Reflected` - this world's own rows - which works. With the
-- destination present it is six, and the pass draws **only the foreign range**
-- and skips the plan. That branch is what yields an empty texture, and it is the
-- one line to instrument next.
--
-- ## And what the sampling turned out not to be
--
-- Instrumented and removed: the pane's sampling matrix is **identical** in the
-- run that works and the run that does not - stored and fresh agree, and both
-- world counts agree. The far world's rows *are* uploaded at the right slots and
-- *are* drawn into the surface texture, one call, seventy-two triangles.
--
-- So the picture is not a bad projection of a good texture. It is a uniform
-- texture. Whatever those seventy-two triangles produce from that camera fills
-- the hole with one colour.
--
-- **The leading explanation, unconfirmed:** both worlds are laid out the same
-- way, and the stand-in sits a fraction behind the block. So the surface camera
-- ends up looking at the *far world's own portal block*, from about the same
-- distance the frustum is fitted at, and that block is the same ten by eight the
-- frustum covers. It fills the hole exactly. `ImmersivePortals.luau` has the same
-- layout and the same stand-in offset, which is why its pane reads as a flat slab
-- rather than as a room.
--
-- If that is it, the fix is the scene's and not the engine's - a destination
-- stand-in has to be placed where the far world's own pane is not, or the far
-- pane has to be kept out of the picture by tag. Moving the stand-in aside here
-- did not change the still, so it is not proven and should be measured before it
-- is believed.
--
-- So whatever is left is in the multi-world path rather than in the seam: the
-- camera is right (`scene/tests/SurfaceCameras.cpp` pins it against
-- `SeamMapping` from both sides), the sampling is right (the same file projects
-- the pane's four corners through `ViewProjection · SurfaceMapping` and requires
-- them inside `0..1`), and the far world's rows arrive
-- (`client/tests/Presentation.cpp`). Every cross-world scene has two worlds by
-- definition, which is why this was only ever seen through a portal.
--
-- ## Which world is which
--
-- `--worlds 2` names them `client.world` and `client.world.1`, and the same file
-- runs in both - the arrangement `Mirrors-4-worlds.luau` settled, because one
-- file cannot drift out of agreement with itself.

local workspace = game:GetService("Workspace")

local FIRST = "client.world"
local SECOND = "client.world.1"

local THIS_WORLD = game.JobId
local IS_SECOND = THIS_WORLD == SECOND

local DESTINATION = if IS_SECOND then FIRST else SECOND

-- Warm for the first, cool for the second, and every part in a world takes its
-- own shade of it. A still is read by colour here rather than by position.
local TONE = if IS_SECOND then Color3.fromRGB(60, 140, 255) else Color3.fromRGB(255, 140, 40)
local FLOOR_TONE = if IS_SECOND then Color3.fromRGB(24, 34, 58) else Color3.fromRGB(58, 40, 24)

local BLOCK_SIZE = Vector3.new(10, 8, 0.4)
local BLOCK_AT = Vector3.new(0, 4, 0)

local function block(name: string, size: Vector3, at: CFrame, colour: Color3): Part
	local part = Instance.new("Part")
	part.Name = name
	part.Size = size
	part.CFrame = at
	part.Color = colour
	part.Anchored = true
	part.Parent = workspace
	return part
end

-- Clear of a baseplate's top face at y = 0: coplanar surfaces do not flicker in
-- this engine, they resolve stably to whichever one the draw order happens to
-- favour. `scripts/demos/zfight-scan.sh` holds the control for that.
local GROUND_LIFT = 0.01

block("Floor", Vector3.new(80, 2, 80), CFrame.new(0, -1 + GROUND_LIFT, 0), FLOOR_TONE)

-- **The pad, on the pane's back side**, which is where a player spawns and
-- therefore where the camera stands. It is also the thing that was missing from
-- the picture, so it is the largest coloured surface in the scene.
block("SpawnLocation", Vector3.new(12, 1, 12), CFrame.new(0, 0.5, 14), TONE)

-- **Two markers, one each side of the block, at different heights**, so a still
-- says *which half* of the far world is in the glass rather than only that
-- something is. Near is high and behind the pad; far is low and behind the
-- block.
block("NearMarker", Vector3.new(3, 3, 3), CFrame.new(0, 9, 7), TONE)
block("FarMarker", Vector3.new(3, 3, 3), CFrame.new(0, 2, -7), TONE)

-- --- the hole ----------------------------------------------------------------

-- **Both worlds build one, in the same place**, which is how a pair is authored
-- and is exactly the arrangement that used to blank the hole: the far world's
-- slab stands where the surface camera looks, it is the same ten by eight the
-- frustum is fitted to, and it filled the picture edge to edge. A pane is no
-- longer drawn into the image of a hole that leads back to it - the same rule a
-- mirror has always had about itself.
local pane = block("PortalBlock", BLOCK_SIZE, CFrame.new(BLOCK_AT), Color3.new(0.85, 0.85, 0.9))
pane.CanCollide = false

-- Where the camera stands, in this world's coordinates. Just behind the block,
-- so the map is nearly the identity and the hole reads as an opening - the same
-- arrangement `ImmersivePortals.luau` uses and the one the bug hid behind.
local beyond = block("Beyond", BLOCK_SIZE, CFrame.new(BLOCK_AT + Vector3.new(0, 0, -0.6)), pane.Color)
beyond.Transparency = 1
beyond.CanCollide = false

-- **Both worlds get one, because a pair has two mouths** - and because the far
-- one is the thing that used to fill this one's picture. A pane is no longer
-- copied into the image of a hole that leads back to it, and this is the scene
-- that says so: without the rule the glass is one flat colour, with it the far
-- room is in it.
do
	local portal = Instance.new("Portal")
	portal.Name = "Hole"
	portal.Face = Enum.NormalId.Front
	portal.SurfaceSize = Vector3.new(1024, 512)
	portal.ImageTransparency = 0
	portal.Destination = beyond
	portal.DestinationWorld = DESTINATION
	portal.Parent = pane
end

-- **A plain mirror beside it.** Slots are handed out by `AimSurfaceCameras` in
-- entity order, so this takes the next one rather than naming it. Same pass, same camera code,
-- same sampling - the only difference is that a mirror draws *this* world's rows
-- and the hole draws another world's appended range. So a frame where the mirror
-- works and the hole does not says the difference is the foreign range, and one
-- where neither works says it is the surface pass. A discriminator costs one part.
if not IS_SECOND then
	local glass = block("MirrorPane", BLOCK_SIZE, CFrame.new(Vector3.new(-13, 4, 0)), Color3.new(0.8, 0.8, 0.85))
	glass.CanCollide = false

	local mirror = Instance.new("SurfaceCamera")
	mirror.Name = "Glass"
	mirror.Face = Enum.NormalId.Front
	mirror.SurfaceSize = Vector3.new(1024, 512)
	mirror.ImageTransparency = 0
	mirror.Parent = glass
end

-- --- where you stand ---------------------------------------------------------
--
-- **On the pad, looking at the block**, which is where a player spawns and the
-- one viewpoint the report is about. Only the first world gets a camera: the
-- client renders the world it was given first and the second exists to be looked
-- into.
if not IS_SECOND then
	local view = Instance.new("Camera")
	view.Name = "Viewer"
	view.FieldOfView = 70
	view.CFrame = CFrame.lookAt(Vector3.new(0, 5, 16), Vector3.new(0, 4, 0))
	view.Parent = workspace
	workspace.CurrentCamera = view
end

print("cross-world seam: this is '" .. THIS_WORLD .. "', the hole leads to '" .. DESTINATION .. "'")
