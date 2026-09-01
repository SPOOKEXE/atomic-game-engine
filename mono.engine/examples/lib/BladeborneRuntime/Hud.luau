--!strict

local RunService = game:GetService("RunService")

local Hud = {}

local BLACK = Color3.fromRGB(0, 0, 0)
local WHITE = Color3.fromRGB(255, 255, 255)
local MUTED = Color3.fromRGB(170, 170, 170)

local function label(parent: Instance, name: string, y: number, height: number, colour: Color3): TextLabel
	local text = Instance.new("TextLabel")
	text.Name = name
	text.BackgroundTransparency = 1
	text.Position = UDim2.new(0, 12, 0, y)
	text.Size = UDim2.new(1, -24, 0, height)
	text.TextColor3 = colour
	text.TextSize = 15
	text.TextXAlignment = Enum.TextXAlignment.Left
	text.TextYAlignment = Enum.TextYAlignment.Center
	text.Parent = parent
	return text
end

function Hud.start(player: Instance): () -> ()
	local container = player.PlayerGui
	if container == nil then
		return function() end
	end

	local stale = container:FindFirstChild("BladeborneHud")
	if stale ~= nil then
		stale:Destroy()
	end

	local screen = Instance.new("ScreenGui")
	screen.Name = "BladeborneHud"
	screen.Parent = container

	local strip = Instance.new("Frame")
	strip.Name = "Status"
	strip.BackgroundColor3 = BLACK
	strip.BackgroundTransparency = 0.08
	strip.BorderSizePixel = 0
	strip.Position = UDim2.new(0, 18, 0, 18)
	strip.Size = UDim2.new(0, 430, 0, 58)
	strip.Parent = screen

	local vital = label(strip, "Vital", 4, 26, WHITE)
	local controls = label(strip, "Controls", 29, 20, MUTED)
	controls.Text = "MOUSE 1  LIGHT ATTACK    WASD  MOVE"

	local previous = ""
	local connection = RunService.Heartbeat:Connect(function()
		local character = player.Character
		local humanoid = if character == nil then nil else character:FindFirstChild("Humanoid") :: Humanoid?
		local health = if humanoid == nil then 0 else math.max(0, math.floor(humanoid.Health + 0.5))
		local maximum = if humanoid == nil then 100 else math.max(1, math.floor(humanoid.MaxHealth + 0.5))
		local level = player:GetAttribute("BladeborneLevel")
		local experience = player:GetAttribute("BladeborneExperience")
		local gold = player:GetAttribute("BladeborneGold")
		local nextText = `HP {health}/{maximum}    LV {if typeof(level) == "number" then level else 1}    XP {if typeof(experience) == "number" then experience else 0}    GOLD {if typeof(gold) == "number" then gold else 0}`
		if nextText ~= previous then
			previous = nextText
			vital.Text = nextText
		end
	end)

	return function()
		connection:Disconnect()
		screen:Destroy()
	end
end

return Hud
