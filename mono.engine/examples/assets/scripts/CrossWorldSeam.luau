--!strict
-- Two coloured worlds share one bidirectional opening, with a mirror beside it.
--
-- Run `client --worlds 2 --view-spacing 0 --script CrossWorldSeam.luau`.
-- Both worlds occupy the same coordinates; view spacing would separate their
-- geometry and move the destination mouth away from the source opening.
--
-- The default camera stands at positive Z, on the spawn-pad side. Through the
-- hole it sees the other world's floor and FarMarker at negative Z. A camera
-- at negative Z sees the other world's pad and NearMarker at positive Z.
-- The first world is orange and the destination is blue, so the opening must
-- change palette while the neighbouring mirror retains this world's colours.
--
-- Each invisible destination stand-in has its Front face coincident with the
-- other world's actual mouth, with the opposite normal. This makes the seam
-- map preserve camera placement and lets captures identify the return entrance.

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

-- The spawn pad sits on the positive-Z side near the default camera.
-- It appears through the opening from negative Z, along with the high NearMarker.
block("SpawnLocation", Vector3.new(12, 1, 12), CFrame.new(0, 0.5, 14), TONE)

-- **Two markers, one each side of the block, at different heights**, so a still
-- says *which half* of the far world is in the glass rather than only that
-- something is. Near is high and behind the pad; far is low and behind the
-- block.
block("NearMarker", Vector3.new(3, 3, 3), CFrame.new(0, 9, 7), TONE)
block("FarMarker", Vector3.new(3, 3, 3), CFrame.new(0, 2, -7), TONE)

-- --- the hole ----------------------------------------------------------------

-- Both worlds build their actual mouth in the same place. Captures omit the
-- reciprocal entrance so its opaque slab cannot cover the destination room.
local pane = block("PortalBlock", BLOCK_SIZE, CFrame.new(BLOCK_AT), Color3.new(0.85, 0.85, 0.9))
pane.CanCollide = false

-- Opposed Front normals cancel the seam map's half-turn. One slab thickness
-- places the stand-in's Front face on the real mouth at z = -0.2.
local beyond = block(
	"Beyond",
	BLOCK_SIZE,
	CFrame.new(BLOCK_AT + Vector3.new(0, 0, -BLOCK_SIZE.Z)) * CFrame.Angles(0, math.pi, 0),
	pane.Color
)
beyond.Transparency = 1
beyond.CanCollide = false

-- Each mouth names the other world while using its local stand-in to describe
-- the far face's placement.
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

-- The neighbouring mirror shows this world's palette. Its surface slot is
-- assigned independently from the portal by AimSurfaceCameras.
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
	local view = assert(
		assert(game:GetService("StarterPlayer"):FindFirstChild("StarterPlayerScripts")):FindFirstChild("DemoCamera"),
		"the demo camera companion is missing"
	)
	view:SetAttribute("Name", "Viewer")
	view:SetAttribute("FieldOfView", 70)
	view:SetAttribute("CFrame", CFrame.lookAt(Vector3.new(0, 5, 16), Vector3.new(0, 4, 0)))
end

print("cross-world seam: this is '" .. THIS_WORLD .. "', the hole leads to '" .. DESTINATION .. "'")
