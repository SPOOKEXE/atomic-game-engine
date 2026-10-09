--!strict
-- Switching views is local; the authority keeps both mesh sources shared.
local RunService = game:GetService("RunService")
local function numberAttribute(name: string): number
	local value = script:GetAttribute(name)
	assert(type(value) == "number", "camera attribute must be a number: " .. name)
	return value
end

local function frameAttribute(name: string): CFrame
	local value = script:GetAttribute(name)
	assert(typeof(value) == "CFrame", "camera attribute must be a CFrame: " .. name)
	return value :: CFrame
end

local function makeCamera(name: string, frame: CFrame, fieldOfView: number): Camera
	local camera = Instance.new("Camera", workspace)
	camera.Name = name
	camera.FieldOfView = fieldOfView
	camera.FarPlaneZ = numberAttribute("FarPlaneZ")
	camera.CFrame = frame
	camera.CameraSubject = nil
	local owned = Instance.new("ObjectValue", script)
	owned.Name = "PublishedCamera"
	owned.Value = camera
	return camera
end

local mainCamera = makeCamera("LodMainCamera", frameAttribute("MainFrame"), numberAttribute("MainFieldOfView"))
local observerCamera =
	makeCamera("LodObserverCamera", frameAttribute("ObserverFrame"), numberAttribute("ObserverFieldOfView"))
workspace.CurrentCamera = mainCamera
mainCamera.CameraType = Enum.CameraType.Scriptable

local observing = false
local simulatedSeconds = 0
local observerPeriodSeconds = numberAttribute("ObserverPeriodSeconds")
assert(observerPeriodSeconds > 0, "observer period must be positive")
RunService.Heartbeat:Connect(function(deltaTime: number)
	simulatedSeconds += deltaTime
	local shouldObserve = math.floor(simulatedSeconds / observerPeriodSeconds) % 2 == 1
	if shouldObserve == observing then
		return
	end

	observing = shouldObserve
	local active = if observing then observerCamera else mainCamera
	workspace.CurrentCamera = active
	active.CameraType = Enum.CameraType.Scriptable
	print(if observing then "lod decimation: observer camera" else "lod decimation: main camera")
end)
