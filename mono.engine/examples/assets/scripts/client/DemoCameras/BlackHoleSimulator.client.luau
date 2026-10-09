--!strict
-- Each viewer owns its orbit; the accretion geometry runs on the authority.
local RunService = game:GetService("RunService")
local function numberAttribute(name: string): number
	local value = script:GetAttribute(name)
	assert(type(value) == "number", "camera attribute must be a number: " .. name)
	return value
end

local camera = Instance.new("Camera", workspace)
local cameraName = script:GetAttribute("CameraName")
assert(type(cameraName) == "string", "camera name must be a string")
camera.Name = cameraName
camera.FieldOfView = numberAttribute("FieldOfView")
camera.NearPlaneZ = numberAttribute("NearPlaneZ")
camera.FarPlaneZ = numberAttribute("FarPlaneZ")
camera.CameraSubject = nil
workspace.CurrentCamera = camera
camera.CameraType = Enum.CameraType.Scriptable
local owned = Instance.new("ObjectValue", script)
owned.Name = "PublishedCamera"
owned.Value = camera

local elapsedSeconds = 0
local function updateCamera(deltaTime: number)
	elapsedSeconds += math.clamp(deltaTime, 0, 0.1)
	local cameraAngle = elapsedSeconds * 0.075
	local cameraPosition = Vector3.new(math.sin(cameraAngle) * 8, 14, 39 + math.cos(cameraAngle) * 3)
	camera.CFrame = CFrame.lookAt(cameraPosition, Vector3.new(0, 0, 0))
end

updateCamera(0)
RunService.Heartbeat:Connect(updateCamera)
