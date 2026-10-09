--!strict
-- The viewer crosses independently of the server-driven hallway walker.
local frame = script:GetAttribute("InitialFrame")
if frame == nil then
	return
end

assert(typeof(frame) == "CFrame", "hallway view must be a CFrame")

local view = Instance.new("Camera", workspace)
view.Name = "Viewer"
view.FieldOfView = 70
view.CFrame = frame :: CFrame
view.CameraSubject = nil
workspace.CurrentCamera = view
view.CameraType = Enum.CameraType.Scriptable
local owned = Instance.new("ObjectValue", script)
owned.Name = "PublishedCamera"
owned.Value = view

if script:GetAttribute("View") == "fly" then
	local RunService = game:GetService("RunService")
	local longLength = script:GetAttribute("LongLength")
	local cameraSpeed = script:GetAttribute("CameraSpeed")
	assert(type(longLength) == "number" and type(cameraSpeed) == "number", "hallway camera lengths must be numbers")
	local cameraHeading = -1
	RunService.Heartbeat:Connect(function(deltaTime: number)
		local current = view.CFrame
		if current.Position.Z <= -(longLength / 2 + 8) then
			cameraHeading = 1
		elseif current.Position.Z >= longLength / 2 + 8 then
			cameraHeading = -1
		end
		local forward = Vector3.new(0, 0, cameraHeading)
		local eye = current.Position + forward * cameraSpeed * deltaTime
		view.CFrame = CFrame.lookAt(eye, eye + forward)
	end)
end
