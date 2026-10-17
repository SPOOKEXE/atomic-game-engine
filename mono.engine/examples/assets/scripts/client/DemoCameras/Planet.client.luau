--!strict
-- The viewer owns its orbit; authoritative LOD follows admitted pose feedback.
local RunService = game:GetService("RunService")
local radiusValue = script:GetAttribute("OrbitRadius")
local heightValue = script:GetAttribute("OrbitHeight")
local radius = if typeof(radiusValue) == "number" then radiusValue else 165
local height = if typeof(heightValue) == "number" then heightValue else 34
local camera = Instance.new("Camera")
camera.Name = "PlanetCamera"
camera.FieldOfView = 58
camera.FarPlaneZ = 1500
camera.CFrame = CFrame.lookAt(Vector3.new(0, height, radius), Vector3.zero)
camera.Parent = workspace
camera.CameraSubject = nil
workspace.CurrentCamera = camera
camera.CameraType = Enum.CameraType.Scriptable
local ownership = Instance.new("ObjectValue")
ownership.Name = "PublishedCamera"
ownership.Value = camera
ownership.Parent = script

local remote = game:GetService("ReplicatedStorage"):WaitForChild("DemoViewPose", 10)
assert(remote ~= nil, "the demo view feedback channel did not arrive")
local viewRemote = remote :: RemoteEvent
local HttpService = game:GetService("HttpService")
local sinceFeedback = 0
local function SendView(delta: number)
	sinceFeedback += delta
	if sinceFeedback < 0.1 then return end
	sinceFeedback = 0
	local current = workspace.CurrentCamera :: Camera?
	if current == nil then return end
	local position = current.CFrame.Position
	local look = current.CFrame.LookVector
	viewRemote:FireServer(HttpService:JSONEncode({
		x = position.X, y = position.Y, z = position.Z,
		lx = look.X, ly = look.Y, lz = look.Z, fov = current.FieldOfView,
	}))
end
local tick = 0
RunService.Heartbeat:Connect(function(delta: number)
	tick += 1
	local current = workspace.CurrentCamera :: Camera?
	if current ~= nil and current:Equals(camera) then
		local angle = tick * 0.0025
		camera.CFrame = CFrame.lookAt(Vector3.new(
			math.sin(angle) * radius, height + math.sin(angle * 0.7) * 16, math.cos(angle) * radius
		), Vector3.zero)
	end
	SendView(delta)
end)
