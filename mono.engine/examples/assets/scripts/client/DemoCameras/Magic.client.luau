--!strict

local orbitCentre = script:GetAttribute("OrbitCentre")
local orbitRadius = script:GetAttribute("OrbitRadius")
local orbitPeriod = script:GetAttribute("OrbitPeriod")
local eyeHeight = script:GetAttribute("EyeHeight")
if typeof(orbitCentre) ~= "Vector3" or type(orbitRadius) ~= "number" or
	type(orbitPeriod) ~= "number" or type(eyeHeight) ~= "number" then
	return
end

local camera = Instance.new("Camera")
local name = script:GetAttribute("Name")
camera.Name = if type(name) == "string" then name else "Camera"

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

local function poseAt(seconds: number): CFrame
	local angle = seconds * (math.pi * 2 / orbitPeriod)
	local offset = Vector3.new(math.cos(angle) * orbitRadius, eyeHeight, math.sin(angle) * orbitRadius)
	return CFrame.lookAt(orbitCentre + offset, orbitCentre)
end

local elapsed = 0
camera.CFrame = poseAt(elapsed)
camera.CameraSubject = nil
camera.Parent = workspace
workspace.CurrentCamera = camera
local published = Instance.new("ObjectValue")
published.Name = "PublishedCamera"
published.Value = camera
published.Parent = script
camera.CameraType = Enum.CameraType.Scriptable

game:GetService("RunService").Heartbeat:Connect(function(deltaTime: number)
	elapsed += deltaTime
	camera.CFrame = poseAt(elapsed)
end)
