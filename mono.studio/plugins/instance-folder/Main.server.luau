--!strict
-- The folder is mirrored as instances under this plugin's `Plugin`, so the
-- Roblox way of reaching a module works unchanged.
local shared = script.Parent:FindFirstChild("Shared") :: Instance
local Config = require(shared:FindFirstChild("Config") :: Instance)

local toolbar = plugin:CreateToolbar(Config.ToolbarName)
local button = toolbar:CreateButton("instance-folder-snap", Config.Tooltip, "", "Snap")
button.Click:Connect(function()
	print("grid size is " .. plugin.GridSize)
end)
