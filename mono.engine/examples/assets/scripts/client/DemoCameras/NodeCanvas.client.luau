--!strict
-- Client half of the node canvas scene. The scene script authors the graph;
-- each player edits their own PlayerGui copy, so the editing handlers live here.

local Players = game:GetService("Players")
local player = Players.LocalPlayer
if player == nil then
	return
end
local playerGui = player:WaitForChild("PlayerGui", 10)
assert(playerGui ~= nil, "PlayerGui did not arrive")
local screen = playerGui:WaitForChild("NodeCanvasExample", 10)
assert(screen ~= nil, "NodeCanvasExample did not arrive")
local canvas = screen:WaitForChild("ComputeGraph", 10) :: NodeCanvas
assert(canvas ~= nil, "ComputeGraph did not arrive")

-- Right-click a wire to remove it. The wire is a drawn link, so its signal
-- follows the same picked instance as the port buttons.
local function enableWireRemoval(link: NodeCanvasLink)
	link.MouseButton2Click:Connect(function()
		link:Destroy()
	end)
end

-- Click an output, then an input, to route a validated wire through the
-- canvas. The canvas still refuses mismatched types and cycles.
local pendingOutput: NodeCanvasPort? = nil

local function connectFrom(port: NodeCanvasPort)
	port.MouseButton1Click:Connect(function()
		if port.Direction == Enum.NodePortDirection.Output then
			pendingOutput = port
		elseif pendingOutput then
			local link = canvas:Connect(pendingOutput, port)
			enableWireRemoval(link)
			pendingOutput = nil
		end
	end)
end

for _, descendant in canvas:GetDescendants() do
	if descendant:IsA("NodeCanvasPort") then
		connectFrom(descendant :: NodeCanvasPort)
	elseif descendant:IsA("NodeCanvasLink") then
		enableWireRemoval(descendant :: NodeCanvasLink)
	end
end

-- The graph owns no VM callbacks. Regular instance signals are the callback
-- boundary, so a moved node decides when to recalculate group geometry.
for _, descendant in canvas:GetDescendants() do
	if descendant:IsA("NodeCanvasNode") then
		descendant:GetPropertyChangedSignal("Position"):Connect(function()
			canvas:RefreshGroups()
		end)
	end
end
