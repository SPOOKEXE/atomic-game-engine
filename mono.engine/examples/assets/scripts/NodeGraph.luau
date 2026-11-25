--!strict
-- A node graph built, run and drawn entirely from script with NodeGraph.
-- In Studio, add it through World > New Scene from Example > NodeGraph.luau.
--
-- The left canvas is a NodeGraph.View on a NodeCanvas with the Blueprint
-- preset: drag nodes, pan and pull wires. The right panel is the same graph
-- painted by the script itself from the view's draw commands.
--
-- The server publishes the graph on a Folder. Each player edits their own copy
-- of the canvas; client/DemoCameras/NodeGraph.client.luau follows the published
-- graph there and runs it again as it is edited.

local NG = NodeGraph

local registry = NG.Registry.new()
registry:DefineNode({
	Id = "math.add",
	Label = "Add",
	Inputs = { { Name = "a", Type = "number", Default = 0 }, { Name = "b", Type = "number", Default = 0 } },
	Outputs = { { Name = "sum", Type = "number" } },
	Evaluate = function(ctx, inputs)
		return { sum = inputs.a + inputs.b }
	end,
})
registry:DefineNode({
	Id = "math.scale",
	Label = "Scale",
	Inputs = { { Name = "value", Type = "number", Default = 0 }, { Name = "factor", Type = "number", Default = 2 } },
	Outputs = { { Name = "value", Type = "number" } },
	Evaluate = function(ctx, inputs)
		return { value = inputs.value * inputs.factor }
	end,
})

local graph = NG.Graph.new(registry)
local add = graph:AddNode("math.add", { a = 3, b = 4 }, Vector2.new(40, 60))
local scale = graph:AddNode("math.scale", { factor = 10 }, Vector2.new(300, 60))
graph:Link(add, "sum", scale, "value")

local published = Instance.new("Folder")
published.Name = "PublishedGraph"
published.Parent = workspace
graph:PublishTo(published)

local runner = NG.Runner.new(graph, "Tick")

local screen = Instance.new("ScreenGui")
screen.Name = "NodeGraphExample"
screen.Parent = game:GetService("StarterGui")

local canvas = Instance.new("NodeCanvas")
canvas.Name = "ScriptGraph"
canvas.Position = UDim2.new(0, 24, 0, 24)
canvas.Size = UDim2.new(0, 560, 0, 320)
canvas.Parent = screen

local view = NG.View.new(graph, { Theme = "Blueprint" })
view.Parent = canvas

-- The same graph painted by hand: one Frame per rectangle command.
local painted = Instance.new("Frame")
painted.Name = "PaintedGraph"
painted.Position = UDim2.new(0, 608, 0, 24)
painted.Size = UDim2.new(0, 560, 0, 320)
painted.ClipsDescendants = true
painted.Parent = screen

local custom = NG.View.new(graph, { Theme = "Minimal" })
custom.Size = Vector2.new(560, 320)
custom.Paint = function(commands: { NodeGraphDrawCommand }, _)
	painted:ClearAllChildren()
	for _, command in commands do
		if command.Kind == "Rect" and command.FillAlpha > 0 then
			local box = Instance.new("Frame")
			box.Position = UDim2.new(0, command.Ax, 0, command.Ay)
			box.Size = UDim2.new(0, command.Bx - command.Ax, 0, command.By - command.Ay)
			box.BackgroundColor3 = command.Fill
			box.BackgroundTransparency = 1 - command.FillAlpha
			box.BorderSizePixel = 0
			box.Parent = painted
		end
	end
end

local report = runner:Run()
local result = runner:Output(scale, "value")
print(`NodeGraph example: {report.Ran} nodes ran, scale = {result}`)
