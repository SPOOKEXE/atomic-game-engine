--!strict
-- Direct-room reference for the moving front portal-seam capture.
--
-- This scene has the same front camera, moving yellow bar, colours, and frame
-- timeline as PortalSeam.luau, but no aperture. It is the continuous-room
-- reference against which capture-portal-seam.sh compares every front frame.

local WORLD = workspace
local RunService = game:GetService("RunService")

local STRADDLER = Color3.fromRGB(255, 214, 0)
local FLOOR = Color3.fromRGB(40, 60, 90)

local function block(name: string, size: Vector3, at: CFrame, colour: Color3): Part
	local part = Instance.new("Part")
	part.Name = name
	part.Size = size
	part.CFrame = at
	part.Color = colour
	part.Anchored = true
	part.Parent = WORLD
	return part
end

block("Floor", Vector3.new(60, 1, 60), CFrame.new(0, -0.49, 0), FLOOR)
local straddler = block("Straddler", Vector3.new(1, 1, 10), CFrame.new(0, 3, 0), STRADDLER)
local straddlerFrame = straddler.CFrame
local beats = 0
RunService.Heartbeat:Connect(function()
	-- Match PortalSeam's one increment per fixed default script heartbeat. Each
	-- process starts at zero, so equal captured simulation ticks share a pose.
	beats += 1
	straddler.CFrame = straddlerFrame * CFrame.new(math.sin(beats * math.pi / 36) * 1.5, 0, 0)
end)

local VIEW = ""
if VIEW == "front" then
	local view = Instance.new("Camera")
	view.Name = "Viewer"
	view.FieldOfView = 70
	view.CFrame = CFrame.lookAt(Vector3.new(-13, 8, -13), Vector3.new(0, 4, 0))
	view.Parent = WORLD
	WORLD.CurrentCamera = view
end
