--!strict
-- Client half of the GUI interaction check. The scene script builds the panel
-- on the server, and in Play each player sees a PlayerGui copy of it. Clicks
-- land on that copy, so only a client script can hear them; this one prints
-- each activation so a click in the Play client view is visible in Output.

local Players = game:GetService("Players")
local player = Players.LocalPlayer
if player == nil then return end
local playerGui = player:WaitForChild("PlayerGui", 10)
assert(playerGui ~= nil, "PlayerGui did not arrive")
local screen = playerGui:WaitForChild("GuiInteractionDemo", 10)
assert(screen ~= nil, "GuiInteractionDemo did not arrive")
local panel = screen:WaitForChild("InteractionPanel", 10)
assert(panel ~= nil, "InteractionPanel did not arrive")

for _, name in { "TextButton", "ImageButton" } do
	local button = panel:WaitForChild(name, 10)
	assert(button ~= nil, name .. " did not arrive")
	local clicks = 0
	button.Activated:Connect(function()
		clicks += 1
		print(string.format("client: %s activated (%d)", name, clicks))
	end)
end
