--!strict
-- Client half of the node graph scene. It follows the graph the server
-- published, draws it on this player's own canvas and runs it as it is edited.

local Players = game:GetService("Players")
local player = Players.LocalPlayer
if player == nil then
	return
end

-- Keep waiting: a slow join is not an error.
local function waitFor(parent: Instance, name: string): Instance
	local found = parent:WaitForChild(name, 10)
	while found == nil do
		found = parent:WaitForChild(name, 10)
	end
	return found :: Instance
end

local screen = waitFor(waitFor(player, "PlayerGui"), "NodeGraphExample")
local canvas = waitFor(screen, "ScriptGraph")
local published = waitFor(workspace, "PublishedGraph")

-- Both sides define the same node types; only the document travels.
local NG = NodeGraph
local registry = NG.Registry.new()
registry:DefineNode({
	Id = "math.add",
	Label = "Add",
	Inputs = { { Name = "a", Type = "number", Default = 0 }, { Name = "b", Type = "number", Default = 0 } },
	Outputs = { { Name = "sum", Type = "number" } },
	Evaluate = function(_, inputs)
		return { sum = inputs.a + inputs.b }
	end,
})
registry:DefineNode({
	Id = "math.scale",
	Label = "Scale",
	Inputs = { { Name = "value", Type = "number", Default = 0 }, { Name = "factor", Type = "number", Default = 2 } },
	Outputs = { { Name = "value", Type = "number" } },
	Evaluate = function(_, inputs)
		return { value = inputs.value * inputs.factor }
	end,
})

local graph = NG.Graph.subscribe(registry, published)
local view = NG.View.new(graph, { Theme = "Blueprint" })
view.Parent = canvas

local result = Instance.new("TextLabel")
result.Name = "Result"
result.Position = UDim2.new(0, 24, 0, 352)
result.Size = UDim2.new(0, 560, 0, 28)
result.BackgroundColor3 = Color3.new(0, 0, 0)
result.TextColor3 = Color3.new(1, 1, 1)
result.Parent = screen

-- A run reads the canvas edits made since the last one.
local runner = NG.Runner.new(graph, "Tick")
while canvas.Parent ~= nil do
	runner:Run()
	for _, node in graph:GetNodes() do
		if node.Def == "math.scale" then
			result.Text = `{node.Id} = {runner:Output(node.Id, "value")}`
		end
	end
	task.wait(0.25)
end
