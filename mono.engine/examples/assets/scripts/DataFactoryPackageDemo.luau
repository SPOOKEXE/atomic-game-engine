-- Package-safe setup for the external data-factory MCP workflow.
local workspace = game:GetService("Workspace")

local previous = workspace:FindFirstChild("DataFactoryPackageDemo")
if previous ~= nil then
	previous:Destroy()
end
local root = Instance.new("Folder")
root.Name = "DataFactoryPackageDemo"
root.Parent = workspace

local camera = Instance.new("Camera")
camera.Name = "DataFactoryPackageCamera"
camera:SetAttribute("DataFactoryId", "data-factory-package/camera")
camera.CFrame = CFrame.new(0, 2, 8)
camera.ImageWidth = 512
camera.ImageHeight = 288
camera.Parent = root
workspace.CurrentCamera = camera

local function part(name, id, semanticId, partId, position, size, colour)
	local object = Instance.new("Part")
	object.Name = name
	object:SetAttribute("DataFactoryId", id)
	object:SetAttribute("DataFactorySemanticId", semanticId)
	object:SetAttribute("DataFactoryPartId", partId)
	object.Position = position
	object.Size = size
	object.Color = colour
	object.Anchored = true
	object.Parent = root
end

part("VisibleBox", "data-factory-package/visible", "data-factory-package/semantic/box", "data-factory-package/part/visible", Vector3.new(0, 10, 0), Vector3.new(20, 20, 20), Color3.fromRGB(50, 190, 255))
part("OffscreenBox", "data-factory-package/offscreen", "data-factory-package/semantic/box", "data-factory-package/part/offscreen", Vector3.new(8, 1, 0), Vector3.new(4, 2, 2), Color3.fromRGB(255, 120, 54))
part("BehindBox", "data-factory-package/behind", "data-factory-package/semantic/box", "data-factory-package/part/behind", Vector3.new(0, 1, 12), Vector3.new(2, 2, 2), Color3.fromRGB(190, 80, 255))

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Position = Vector3.new(0, -0.5, 0)
floor.Size = Vector3.new(200, 1, 200)
floor.Color = Color3.fromRGB(40, 48, 64)
floor.Anchored = true
floor.Parent = root

local lamp = Instance.new("Part")
lamp.Name = "Lamp"
lamp.Position = Vector3.new(-3, 6, 4)
lamp.Size = Vector3.new(0.4, 0.4, 0.4)
lamp.Color = Color3.fromRGB(255, 235, 190)
lamp.EmissiveTint = lamp.Color
lamp.EmissiveStrength = 5
lamp.Anchored = true
lamp.Parent = root

local bulb = Instance.new("PointLight")
bulb.Name = "Bulb"
bulb.Color = lamp.Color
bulb.Brightness = 260
bulb.Range = 18
bulb.Parent = lamp
