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
camera.ImageWidth = 640
camera.ImageHeight = 360
camera.Parent = root
workspace.CurrentCamera = camera

local function part(name, id, semanticId, partId, position, size)
	local object = Instance.new("Part")
	object.Name = name
	object:SetAttribute("DataFactoryId", id)
	object:SetAttribute("DataFactorySemanticId", semanticId)
	object:SetAttribute("DataFactoryPartId", partId)
	object.Position = position
	object.Size = size
	object.Anchored = true
	object.Parent = root
end

part("VisibleBox", "data-factory-package/visible", "data-factory-package/semantic/box", "data-factory-package/part/visible", Vector3.new(0, 1, 0), Vector3.new(2, 2, 2))
part("OffscreenBox", "data-factory-package/offscreen", "data-factory-package/semantic/box", "data-factory-package/part/offscreen", Vector3.new(8, 1, 0), Vector3.new(4, 2, 2))
part("BehindBox", "data-factory-package/behind", "data-factory-package/semantic/box", "data-factory-package/part/behind", Vector3.new(0, 1, 12), Vector3.new(2, 2, 2))
