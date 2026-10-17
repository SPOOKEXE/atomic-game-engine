--!strict
-- Named viewport cameras and the optional capture eye belong to this viewer.
local function frameAttribute(name: string): CFrame
	local value = script:GetAttribute(name)
	assert(typeof(value) == "CFrame", "camera attribute must be a CFrame: " .. name)
	return value :: CFrame
end

local function makeCamera(name: string, frame: CFrame): Camera
	local camera = Instance.new("Camera", workspace)
	camera.Name = name
	camera.FieldOfView = 70
	camera.CFrame = frame
	camera.CameraSubject = nil
	local owned = Instance.new("ObjectValue", script)
	owned.Name = "PublishedCamera"
	owned.Value = camera
	return camera
end

local cameras: { [string]: Camera } = {}
if script:GetAttribute("NamedCameras") then
	for _, name in { "long-north", "long-south", "short-north", "short-south" } do
		cameras[name] = makeCamera(name, frameAttribute(name))
	end
end

local frame = script:GetAttribute("InitialFrame")
if frame ~= nil then
	assert(typeof(frame) == "CFrame", "tunnel view must be a CFrame")
	local which = script:GetAttribute("View")
	assert(type(which) == "string", "tunnel view name must be a string")
	local view = cameras[which]
	if view == nil then
		view = makeCamera("Viewer", frame :: CFrame)
	end
	workspace.CurrentCamera = view
	view.CameraType = Enum.CameraType.Scriptable
end
