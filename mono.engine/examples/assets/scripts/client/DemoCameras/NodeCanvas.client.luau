--!strict
-- Client half of the node canvas scene. The scene script authors the graph;
-- each player edits their own PlayerGui copy, so the editing handlers live here.

local Players = game:GetService("Players")
local player = Players.LocalPlayer
if player == nil then
	return
end
-- Keep waiting: a slow join is not an error, and an assert here would kill
-- the script and leave the canvas without its handlers.
local function waitFor(parent: Instance, name: string): Instance
	local found = parent:WaitForChild(name, 10)
	while found == nil do
		found = parent:WaitForChild(name, 10)
	end
	return found :: Instance
end

local playerGui = waitFor(player, "PlayerGui")
local screen = waitFor(playerGui, "NodeCanvasExample")
local canvas = waitFor(screen, "ComputeGraph") :: NodeCanvas

-- Right-click a wire to remove it. The wire is a drawn link, so its signal
-- follows the same picked instance as the port buttons.
local function enableWireRemoval(link: NodeCanvasLink)
	link.MouseButton2Click:Connect(function()
		link:Destroy()
	end)
end

-- Dragging a node moves it, dragging empty canvas pans it and dragging from
-- a port pulls a wire; the engine routes those gestures itself. Clicking an
-- output, then an input, is kept as a fallback for pointers that cannot drag.
-- The canvas refuses mismatched types and cycles either way.
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
