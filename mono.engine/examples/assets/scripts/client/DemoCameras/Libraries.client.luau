--!strict

local frame = script:GetAttribute("CFrame")
if typeof(frame) ~= "CFrame" then
	return
end

local camera = Instance.new("Camera")
local name = script:GetAttribute("Name")
camera.Name = if type(name) == "string" then name else "Camera"
camera.CFrame = frame

local fieldOfView = script:GetAttribute("FieldOfView")
if type(fieldOfView) == "number" then
	camera.FieldOfView = fieldOfView
end

local nearPlaneZ = script:GetAttribute("NearPlaneZ")
if type(nearPlaneZ) == "number" then
	camera.NearPlaneZ = nearPlaneZ
end

local farPlaneZ = script:GetAttribute("FarPlaneZ")
if type(farPlaneZ) == "number" then
	camera.FarPlaneZ = farPlaneZ
end

local imageWidth = script:GetAttribute("ImageWidth")
if type(imageWidth) == "number" then
	camera.ImageWidth = imageWidth
end

local imageHeight = script:GetAttribute("ImageHeight")
if type(imageHeight) == "number" then
	camera.ImageHeight = imageHeight
end

camera.CameraSubject = nil
camera.Parent = workspace
workspace.CurrentCamera = camera

local marker = Instance.new("ObjectValue", script)
marker.Name = "PublishedCamera"
marker.Value = camera
