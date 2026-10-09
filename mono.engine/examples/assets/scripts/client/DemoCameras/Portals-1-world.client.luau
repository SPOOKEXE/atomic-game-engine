--!strict

local frame = script:GetAttribute("CFrame")
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
local published = Instance.new("ObjectValue")
published.Name = "PublishedCamera"
published.Value = camera
published.Parent = script
camera.CameraType = Enum.CameraType.Scriptable

local EAST = Vector3.new(1, 0, 0)
local SOUTH = Vector3.new(0, 0, 1)
local WEST = Vector3.new(-1, 0, 0)
local NORTH = Vector3.new(0, 0, -1)

local HALF = script:GetAttribute("Half")
local PACE = script:GetAttribute("Pace")
if typeof(HALF) ~= "number" or typeof(PACE) ~= "number" then
	return
end

type Route = { Stage: number }

local function heading(route: Route, at: Vector3): Vector3
	if route.Stage == 1 and at.X >= -HALF then
		route.Stage = 2
	elseif route.Stage == 2 and at.X >= HALF then
		route.Stage = 3
	elseif route.Stage == 3 and at.Z >= HALF then
		route.Stage = 4
	elseif route.Stage == 4 and at.X < -HALF / 2 then
		-- Only the portal can move the westbound traveller from the garden's
		-- non-negative X coordinates to the hall at X = -HALF.
		route.Stage = 5
	elseif route.Stage == 5 and at.Z <= -HALF then
		route.Stage = 2
	end

	if route.Stage == 1 or route.Stage == 2 then
		return EAST
	elseif route.Stage == 3 then
		return SOUTH
	elseif route.Stage == 4 then
		return WEST
	else
		return NORTH
	end
end

local route: Route = { Stage = 1 }
game:GetService("RunService").Heartbeat:Connect(function(deltaTime: number)
	local direction = heading(route, camera.CFrame.Position)
	local eye = camera.CFrame.Position + direction * PACE * deltaTime
	camera.CFrame = CFrame.lookAt(eye, eye + direction)
end)
