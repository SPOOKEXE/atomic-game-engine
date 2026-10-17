--!strict
-- Ground-following tour samples are shared data; view ownership stays local.
local RunService = game:GetService("RunService")
local Players = game:GetService("Players")
local guide = game:GetService("ReplicatedStorage"):WaitForChild("DemoFlightGuide", 10)
assert(guide ~= nil, "the terrain flight guide did not arrive")
local flightGuide = guide :: CFrameValue
local camera = Instance.new("Camera")
camera.Name = "Flyer"
camera.FieldOfView = 62
local farValue = script:GetAttribute("FarPlaneZ")
camera.FarPlaneZ = if typeof(farValue) == "number" then farValue else 768
camera.CFrame = flightGuide.Value
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
local spawned: Vector3? = nil
local flying = true
RunService.Heartbeat:Connect(function(delta: number)
	local player = Players.LocalPlayer :: Player?
	local character = if player ~= nil then player.Character else nil
	local root = if character ~= nil then character:FindFirstChild("HumanoidRootPart") :: BasePart? else nil
	local current = workspace.CurrentCamera :: Camera?
	if flying and root ~= nil and character ~= nil then
		if spawned == nil then
			spawned = root.Position
		elseif (root.Position - spawned).Magnitude > 0.25 then
			flying = false
			if current ~= nil and current:Equals(camera) then
				local humanoid = character:FindFirstChild("Humanoid")
				if humanoid ~= nil then camera.CameraSubject = humanoid end
				camera.CameraType = Enum.CameraType.Custom
			end
		end
	end
	if flying and current ~= nil and current:Equals(camera) then camera.CFrame = flightGuide.Value end
	SendView(delta)
end)
