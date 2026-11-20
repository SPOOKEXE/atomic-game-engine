--!strict
-- Client half of the interface scene. The scene script builds the panel; each
-- player clicks their own PlayerGui copy, so every handler lives here.
--
-- The swatches are the proof that a generated element connects: they are made
-- in a loop by the scene and nothing distinguishes them from the two named
-- buttons.

local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
local UserInputService = game:GetService("UserInputService")

local player = Players.LocalPlayer
if player == nil then
	return
end
local playerGui = player:WaitForChild("PlayerGui", 10)
assert(playerGui ~= nil, "PlayerGui did not arrive")
local screen = playerGui:WaitForChild("InterfaceExample", 10) :: ScreenGui
assert(screen ~= nil, "InterfaceExample did not arrive")

local function child(parent: Instance, name: string): Instance
	local found = parent:WaitForChild(name, 10)
	assert(found ~= nil, name .. " did not arrive")
	return found :: Instance
end

local card = child(screen, "Card") :: Frame
local header = child(card, "Header")
local palette = child(card, "Palette")
local actions = child(card, "Actions")
local field = child(card, "Field")
local clock = child(header, "Clock") :: TextLabel
local close = child(header, "Close") :: TextButton
local hint = child(actions, "Hint") :: TextLabel
local confirm = child(actions, "Confirm") :: TextButton
local cancel = child(actions, "Cancel") :: TextButton
local box = child(field, "Entry") :: TextBox
local status = child(field, "EntryStatus") :: TextLabel

-- Colours come from the panel itself: each swatch's fill, and the accent the
-- Confirm button is filled with.
local swatches: { TextButton } = {}
for index = 1, 6 do
	table.insert(swatches, child(palette, "Swatch" .. tostring(index)) :: TextButton)
end
local accent = confirm.BackgroundColor3

local picked = swatches[1].BackgroundColor3
local clicks = 0

local function refresh()
	hint.Text = string.format("%d click%s · picked", clicks, clicks == 1 and "" or "s")
	card.BackgroundColor3 = picked
end

for index, swatch in swatches do
	swatch.Activated:Connect(function()
		-- The line `just client-smoke` reads back to prove a click landed.
		print("interface: swatch " .. tostring(index) .. " activated")
		picked = swatch.BackgroundColor3
		clicks += 1
		refresh()
	end)
end

confirm.Activated:Connect(function()
	clicks += 1
	-- The card takes the accent, the one colour nothing else sets it to.
	picked = accent
	refresh()
end)

cancel.Activated:Connect(function()
	clicks = 0
	picked = swatches[1].BackgroundColor3
	refresh()
end)

box.Focused:Connect(function()
	local held = UserInputService:GetFocusedTextBox()
	status.Text = string.format("focused · service says %s", held and held.Name or "nil")
end)

-- `enterPressed` is true when Return released the box, false when a press
-- somewhere else did.
box.FocusLost:Connect(function(enterPressed: boolean)
	status.Text = string.format("released · enterPressed=%s · text %q", tostring(enterPressed), box.Text)
end)

-- A disabled collector is not laid out at all, so closing costs nothing per
-- frame afterwards. Nothing under it receives input either, so it cannot be
-- reopened from inside itself.
close.Activated:Connect(function()
	screen.Enabled = false
end)

-- `MouseEnter` carries `(x, y)` in canvas pixels, which is the Roblox signature.
confirm.MouseEnter:Connect(function(x: number, y: number)
	hint.Text = string.format("confirm @ %d, %d", x, y)
end)

confirm.MouseLeave:Connect(function()
	refresh()
end)

refresh()

-- A tenth of a second, not a frame. The compiled list is kept while its
-- signature holds, so writing the clock every frame would rebuild every frame.
local elapsed = 0
local shown = -1
RunService.Heartbeat:Connect(function(delta: number)
	elapsed += delta
	local tenths = math.floor(elapsed * 10)
	if tenths ~= shown then
		shown = tenths
		clock.Text = string.format("%.1f s", tenths / 10)
	end
end)
