--!strict
-- Client half of the GUI interaction check. The scene script builds the panel;
-- each player clicks their own PlayerGui copy, so this script owns every
-- handler. It first activates both buttons programmatically, so the status
-- line reads PASS before anybody clicks.

local Players = game:GetService("Players")
local player = Players.LocalPlayer
if player == nil then
	return
end
local playerGui = player:WaitForChild("PlayerGui", 10)
assert(playerGui ~= nil, "PlayerGui did not arrive")
local screen = playerGui:WaitForChild("GuiInteractionDemo", 10)
assert(screen ~= nil, "GuiInteractionDemo did not arrive")
local panel = screen:WaitForChild("InteractionPanel", 10)
assert(panel ~= nil, "InteractionPanel did not arrive")

local function find(name: string): Instance
	local found = panel:WaitForChild(name, 10)
	assert(found ~= nil, name .. " did not arrive")
	return found :: Instance
end
local textButton = find("TextButton") :: TextButton
local imageButton = find("ImageButton") :: ImageButton
local textBox = find("TextBox") :: TextBox
local status = find("InteractionStatus") :: TextLabel
local eventStatus = find("EventStatus") :: TextLabel

local textClicks = 0
local imageClicks = 0

local function refreshStatus()
	status.Text = string.format("PASS  TextButton %d   ImageButton %d", textClicks, imageClicks)
end

local function showEvent(name: string)
	eventStatus.Text = name
end

textButton.Activated:Connect(function()
	textClicks += 1
	refreshStatus()
	showEvent("TextButton activated")
end)

imageButton.Activated:Connect(function()
	imageClicks += 1
	refreshStatus()
	showEvent("ImageButton activated")
end)

textButton.MouseEnter:Connect(function()
	showEvent("TextButton hover")
end)

textButton.MouseButton1Down:Connect(function()
	showEvent("TextButton down")
end)

textButton.MouseButton1Up:Connect(function()
	showEvent("TextButton up")
end)

textBox.Focused:Connect(function()
	showEvent("TextBox focused")
end)

textBox.FocusLost:Connect(function()
	showEvent("TextBox focus lost")
end)

-- Both programmatic routes, then the counters are the visible proof they fired.
textButton:EmulateClick()
imageButton:EmulateClick()
assert(textClicks == 1 and imageClicks == 1, "a button did not emulate its click")
showEvent("READY")
