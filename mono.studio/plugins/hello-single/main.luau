--!strict
-- A whole plugin in one file. `plugin` is this plugin's `Plugin` instance.
local Selection = game:GetService("Selection")

local toolbar = plugin:CreateToolbar("Hello")
local button = toolbar:CreateButton("hello-count", "Count the selection", "", "Count")

button.Click:Connect(function()
	print("selected: " .. #Selection:Get())
end)
