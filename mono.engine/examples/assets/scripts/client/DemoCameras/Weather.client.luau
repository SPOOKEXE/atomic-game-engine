--!strict

-- The builder publishes view requests on Workspace, which clients replicate.
local requested = workspace:GetAttribute("WeatherCameraFrame")
local frame = if typeof(requested) == "CFrame" then requested else script:GetAttribute("CFrame")
if typeof(frame) ~= "CFrame" then
	return
end

local camera = Instance.new("Camera")
local name = script:GetAttribute("Name")
camera.Name = if typeof(name) == "string" then name else "Camera"
camera.CFrame = frame

local fieldOfView = script:GetAttribute("FieldOfView")
if typeof(fieldOfView) == "number" then
	camera.FieldOfView = fieldOfView
end
local nearPlaneZ = script:GetAttribute("NearPlaneZ")
if typeof(nearPlaneZ) == "number" then
	camera.NearPlaneZ = nearPlaneZ
end
local farPlaneZ = script:GetAttribute("FarPlaneZ")
if typeof(farPlaneZ) == "number" then
	camera.FarPlaneZ = farPlaneZ
end
local imageWidth = script:GetAttribute("ImageWidth")
if typeof(imageWidth) == "number" then
	camera.ImageWidth = imageWidth
end
local imageHeight = script:GetAttribute("ImageHeight")
if typeof(imageHeight) == "number" then
	camera.ImageHeight = imageHeight
end

camera.CameraSubject = nil
camera.Parent = workspace
workspace.CurrentCamera = camera

-- View requests are events so ordinary camera movement remains independent.
local function applyRequestedFrame()
	local next = workspace:GetAttribute("WeatherCameraFrame")
	if typeof(next) == "CFrame" then
		camera.CFrame = next
	end
end
workspace:GetAttributeChangedSignal("WeatherCameraFrame"):Connect(applyRequestedFrame)

local published = Instance.new("ObjectValue")
published.Name = "PublishedCamera"
published.Value = camera
published.Parent = script
