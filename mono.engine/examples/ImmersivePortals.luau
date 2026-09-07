--!strict
-- Load this scene in immersive-portals-demo-1 and immersive-portals-demo-2.
-- Both rooms share one coordinate frame. Crossing changes the world and its
-- colours while preserving the traveller's position, heading and momentum.
-- The engine owns swept crossing, transfer admission and camera continuation.

local workspace = game:GetService("Workspace")

local DEMO_ONE = "immersive-portals-demo-1"
local DEMO_TWO = "immersive-portals-demo-2"
local IS_SECOND = string.find(game.JobId, DEMO_TWO, 1, true) ~= nil
local DESTINATION = IS_SECOND and DEMO_ONE or DEMO_TWO
local BRICK_COLOUR = IS_SECOND and Color3.new(0.20, 0.55, 0.95) or Color3.new(0.95, 0.45, 0.12)

local FLOOR_SIZE = 60
local FLOOR_THICKNESS = 2
local GROUND_LIFT = 0.01
local BLOCK_SIZE = Vector3.new(10, 8, 0.4)
local BLOCK_AT = Vector3.new(0, 4, 0)

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Size = Vector3.new(FLOOR_SIZE, FLOOR_THICKNESS, FLOOR_SIZE)
floor.Position = Vector3.new(0, -FLOOR_THICKNESS / 2 + GROUND_LIFT, 0)
floor.Color = IS_SECOND and Color3.new(0.16, 0.18, 0.22) or Color3.new(0.22, 0.20, 0.17)
floor.Anchored = true
floor.Parent = workspace

local spawn = Instance.new("Part")
spawn.Name = "SpawnLocation"
spawn.Size = Vector3.new(10, 1, 10)
spawn.Position = Vector3.new(0, 0.5, 14)
spawn.Color = BRICK_COLOUR
spawn.Anchored = true
spawn.Parent = workspace

-- Leave the authored pane collidable so this demo exercises OpenPortals.
-- The engine makes a linked pane a trigger while preserving its aperture.
local block = Instance.new("Part")
block.Name = "PortalBlock"
block.Size = BLOCK_SIZE
block.Position = BLOCK_AT
block.Color = Color3.new(0.85, 0.85, 0.9)
block.Anchored = true
block.Parent = workspace

-- Destination is a local geometric stand-in; DestinationWorld selects the
-- remote room. Opposed front normals cancel the seam map's half-turn. Moving
-- the stand-in by one thickness makes both face centres coincide at z = -0.2.
-- The resulting identity map works through either side and on the return trip.
local beyond = Instance.new("Part")
beyond.Name = "Beyond"
beyond.Size = BLOCK_SIZE
beyond.CFrame = CFrame.new(BLOCK_AT + Vector3.new(0, 0, -BLOCK_SIZE.Z)) * CFrame.Angles(0, math.pi, 0)
beyond.Color = block.Color
beyond.Transparency = 1
beyond.CanCollide = false
beyond.Anchored = true
beyond.Parent = workspace

local portal = Instance.new("Portal")
portal.Name = "Hole"
portal.Face = Enum.NormalId.Front
portal.SurfaceSize = Vector3.new(1024, 512)
portal.ImageTransparency = 0
portal.Destination = beyond
portal.DestinationWorld = DESTINATION
portal.Parent = block

-- Matching markers on both sides identify the room from either approach.
-- They sit above the walking route so the doorway remains clear.
local brick = Instance.new("Part")
brick.Name = IS_SECOND and "CoolBrick" or "WarmBrick"
brick.Size = Vector3.new(3, 3, 3)
brick.Position = BLOCK_AT + Vector3.new(0, 3.5, 7)
brick.Color = BRICK_COLOUR
brick.Anchored = true
brick.Parent = workspace

local far = Instance.new("Part")
far.Name = brick.Name .. "Far"
far.Size = brick.Size
far.Position = BLOCK_AT + Vector3.new(0, 3.5, -7)
far.Color = BRICK_COLOUR
far.Anchored = true
far.Parent = workspace

workspace:KeepWorldAwake("immersive portals: the other end must be live")
