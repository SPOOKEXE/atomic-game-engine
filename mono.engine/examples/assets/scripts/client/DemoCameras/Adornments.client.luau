--!strict
-- Client half of the adornment demo. Pointer input on the interactive
-- SelectionBox only reaches the player's client, so its handlers live here.

local target = workspace:WaitForChild("SelectionBoxTarget", 10)
if target == nil then
	return
end
local selectionBox = target:WaitForChild("SelectionBox", 10) :: any
if selectionBox == nil then
	return
end

selectionBox.OnMouse1Down:Connect(function()
	print("adornments: selection box mouse 1 down")
end)
selectionBox.OnMouse1Changed:Connect(function(x: number, y: number)
	print("adornments: selection box mouse 1 moved", x, y)
end)
selectionBox.OnMouse1Up:Connect(function()
	print("adornments: selection box mouse 1 up")
end)
