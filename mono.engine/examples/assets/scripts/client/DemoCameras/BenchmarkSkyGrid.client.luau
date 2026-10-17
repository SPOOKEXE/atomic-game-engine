--!strict

local ballRadius = script:GetAttribute("BallRadius")
local cameraHeight = script:GetAttribute("CameraHeight")
local cameraSpeed = script:GetAttribute("CameraSpeed")
local cameraLookAhead = script:GetAttribute("CameraLookAhead")
local flightPeriodTicks = script:GetAttribute("FlightPeriodTicks")
if type(ballRadius) ~= "number" or type(cameraHeight) ~= "number" or type(cameraSpeed) ~= "number" or
	type(cameraLookAhead) ~= "number" or type(flightPeriodTicks) ~= "number" then
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

local flightLength = cameraSpeed * flightPeriodTicks / 60
local elapsed = 0
local travelled = (elapsed * cameraSpeed) % flightLength - ballRadius
local eye = Vector3.new(0, cameraHeight, travelled)
camera.CFrame = CFrame.lookAt(eye, Vector3.new(0, cameraHeight - 1.5, travelled + cameraLookAhead))
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
	local currentTravelled = (elapsed * cameraSpeed) % flightLength - ballRadius
	local currentEye = Vector3.new(0, cameraHeight, currentTravelled)
	camera.CFrame = CFrame.lookAt(
		currentEye,
		Vector3.new(0, cameraHeight - 1.5, currentTravelled + cameraLookAhead)
	)
end)
