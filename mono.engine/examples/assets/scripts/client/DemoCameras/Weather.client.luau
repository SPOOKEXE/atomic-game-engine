--!strict

local templates = game:GetService("StarterPlayer"):FindFirstChild("StarterPlayerScripts")
local requests = if templates ~= nil then templates:FindFirstChild("DemoCamera") else nil
local frame = if requests ~= nil then requests:GetAttribute("CFrame") else script:GetAttribute("CFrame")
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

-- The builder edits the replicated template after this client-local script was cloned.
-- View requests are events so ordinary camera movement remains independent.
if requests ~= nil then
	local cameraRequests = requests
	local function applyRequestedFrame()
		local requested = cameraRequests:GetAttribute("CFrame")
		if typeof(requested) == "CFrame" then
			camera.CFrame = requested
		end
	end
	cameraRequests:GetAttributeChangedSignal("CFrame"):Connect(applyRequestedFrame)
	applyRequestedFrame()
end

local published = Instance.new("ObjectValue")
published.Name = "PublishedCamera"
published.Value = camera
published.Parent = script
