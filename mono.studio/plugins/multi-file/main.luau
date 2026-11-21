--!strict
-- `./` starts at this file's folder; the modules below require each other the
-- same way, and `scriptcheck` follows the same paths.
local Counter = require("./core/Counter")

local toolbar = plugin:CreateToolbar("Multi File")
local button = toolbar:CreateButton("multi-file-bump", "Count presses", "", "Bump")
button.Click:Connect(function()
	print(Counter.Bump())
end)
