--!strict

-- H hides and shows the weather control panel. It is a fixed size, and in a
-- small Play viewport it otherwise covers the sky the demo is about.
local UserInputService = game:GetService("UserInputService")
local Players = game:GetService("Players")
UserInputService.InputBegan:Connect(function(input: InputObject, processed: boolean)
	if processed or input.KeyCode ~= Enum.KeyCode.H then
		return
	end
	local player = Players.LocalPlayer
	local playerGui = if player ~= nil then player:FindFirstChild("PlayerGui") else nil
	local controls = if playerGui ~= nil then playerGui:FindFirstChild("Weather Controls") else nil
	if controls ~= nil and controls:IsA("ScreenGui") then
		local screen = controls :: ScreenGui
		screen.Enabled = not screen.Enabled
	end
end)

-- Forward panel clicks to the server: the handlers live in the scene script,
-- which never sees this client's copy of the panel.
task.spawn(function()
	local remote = game:GetService("ReplicatedStorage"):WaitForChild("WeatherControl", 10)
	local player = Players.LocalPlayer
	local playerGui = if player ~= nil then player:WaitForChild("PlayerGui", 10) else nil
	local controls = if playerGui ~= nil then playerGui:WaitForChild("Weather Controls", 10) else nil
	if remote == nil or controls == nil or controls:GetAttribute("ServerHandled") == true then
		return
	end
	local forward = remote :: RemoteEvent
	-- Text the server writes reaches this copy through the attributes the
	-- scene script mirrors it on, keyed by the control's name.
	for _, descendant in controls:GetDescendants() do
		if descendant:IsA("TextLabel") or descendant:IsA("TextButton") then
			local label = descendant :: any
			local key = "WeatherPanel" .. string.gsub(descendant.Name, "%W", "")
			local function pull()
				local text = workspace:GetAttribute(key)
				if typeof(text) == "string" and label.Text ~= text then
					label.Text = text
				end
			end
			pull()
			workspace:GetAttributeChangedSignal(key):Connect(pull)
		end
	end
	for _, descendant in controls:GetDescendants() do
		if descendant:IsA("TextButton") then
			local control = descendant :: TextButton
			control.Activated:Connect(function()
				forward:FireServer(control.Name)
			end)
		end
	end
end)

-- The builder publishes view requests on Workspace, which clients replicate.
local requested = workspace:GetAttribute("WeatherCameraFrame")
local frame = if typeof(requested) == "CFrame" then requested else script:GetAttribute("CFrame")
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

-- View requests are events so ordinary camera movement remains independent.
local function applyRequestedFrame()
	local next = workspace:GetAttribute("WeatherCameraFrame")
	if typeof(next) == "CFrame" then
		camera.CFrame = next
	end
end
workspace:GetAttributeChangedSignal("WeatherCameraFrame"):Connect(applyRequestedFrame)

local published = Instance.new("ObjectValue")
published.Name = "PublishedCamera"
published.Value = camera
published.Parent = script
