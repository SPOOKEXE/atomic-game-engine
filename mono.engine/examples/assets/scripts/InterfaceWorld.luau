--!strict
-- Physical panels are shared; each player's companion builds the interactive UI.
local GROUND_LIFT = 0.01

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = Vector3.new(32, 1, 24)
floor.CFrame = CFrame.new(Vector3.new(0, -0.5 + GROUND_LIFT, 0))
floor.Color = Color3.fromRGB(42, 48, 62)
floor.Parent = workspace

local pane = Instance.new("Part")
pane.Name = "SurfacePanel"
pane.Anchored = true
pane.Size = Vector3.new(10, 6, 0.4)
pane.CFrame = CFrame.new(Vector3.new(-5, 4, 0))
pane.Color = Color3.fromRGB(28, 32, 42)
pane.Parent = workspace

local marker = Instance.new("Part")
marker.Name = "BillboardMarker"
marker.Anchored = true
marker.Size = Vector3.new(2.5, 2.5, 2.5)
marker.CFrame = CFrame.new(Vector3.new(10, 2, -1))
marker.Color = Color3.fromRGB(230, 120, 76)
marker.Parent = workspace

local previewPane = Instance.new("Part")
previewPane.Name = "ViewportPanel"
previewPane.Anchored = true
previewPane.Size = Vector3.new(6, 3.5, 0.4)
previewPane.CFrame = CFrame.new(Vector3.new(5, 4, 0))
previewPane.Color = Color3.fromRGB(18, 22, 32)
previewPane.Parent = workspace

local templates = game:GetService("StarterPlayer"):FindFirstChild("StarterPlayerScripts")
assert(templates ~= nil, "the demo camera template folder is missing")
local demoCamera = templates:FindFirstChild("DemoCamera")
assert(demoCamera ~= nil, "the demo camera companion is missing")
demoCamera:SetAttribute("CFrame", CFrame.lookAt(Vector3.new(0, 8, 22), Vector3.new(0, 3, 0)))
