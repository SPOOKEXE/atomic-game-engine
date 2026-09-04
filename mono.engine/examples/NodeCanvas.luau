--!strict
-- A small typed compute graph authored through the retained GUI tree.
-- In Studio, add it through World > New Scene from Example > NodeCanvas.luau,
-- then press Play to inspect the rendered canvas in the viewport.
--
-- Nodes are ordinary GUI frames, ports own stable typed ids, and links are
-- instances under the canvas. This keeps the saved graph independent from a
-- graph executor. A game may observe edits with normal instance signals and
-- send the resulting graph to its own evaluator or ComputeService work.

local PAPER = Color3.fromRGB(238, 242, 248)
local CANVAS = Color3.fromRGB(5, 8, 12)
local GROUP = Color3.fromRGB(17, 39, 54)
local NODE = Color3.fromRGB(18, 25, 35)
local FLOW = Color3.fromRGB(92, 201, 178)
local VALUE = Color3.fromRGB(110, 174, 244)

local screen = Instance.new("ScreenGui")
screen.Name = "NodeCanvasExample"
screen.Parent = game:GetService("StarterGui")

local canvas = Instance.new("NodeCanvas")
canvas.Name = "ComputeGraph"
canvas.Position = UDim2.new(0, 32, 0, 32)
canvas.Size = UDim2.new(0, 920, 0, 580)
canvas.BackgroundColor3 = CANVAS
canvas.BorderSizePixel = 0
canvas.GridVisible = true
canvas.GridSize = 24
canvas.MinimumZoom = 0.5
canvas.MaximumZoom = 1.75
canvas.Parent = screen

local group = Instance.new("NodeCanvasGroup")
group.Name = "NoisePipeline"
group.GroupId = "noise_pipeline"
group.Title = "Noise pipeline"
group.Layout = Enum.NodeGroupLayout.AroundEdge
group.Padding = Vector2.new(18, 30)
group.Position = UDim2.new(0, 80, 0, 100)
group.BackgroundColor3 = GROUP
group.BackgroundTransparency = 0.15
group.BorderSizePixel = 0
group.Parent = canvas

local groupTitle = Instance.new("TextLabel")
groupTitle.Name = "GroupTitle"
groupTitle.Position = UDim2.new(0, 12, 0, 4)
groupTitle.Size = UDim2.new(1, -24, 0, 20)
groupTitle.BackgroundTransparency = 1
groupTitle.Font = Enum.Font.Bold
groupTitle.Text = group.Title
groupTitle.TextColor3 = PAPER
groupTitle.TextSize = 15
groupTitle.TextXAlignment = Enum.TextXAlignment.Left
groupTitle.Parent = group

local function makeNode(parent: Instance, id: string, kind: string, title: string, x: number, y: number): NodeCanvasNode
	local node = Instance.new("NodeCanvasNode")
	node.Name = title
	node.NodeId = id
	node.NodeType = kind
	node.Title = title
	node.Position = UDim2.new(0, x, 0, y)
	node.Size = UDim2.new(0, 190, 0, 112)
	node.MinimumSize = Vector2.new(150, 84)
	node.BackgroundColor3 = NODE
	node.BorderSizePixel = 0
	node.Parent = parent

	local label = Instance.new("TextLabel")
	label.Name = "Title"
	label.Position = UDim2.new(0, 12, 0, 10)
	label.Size = UDim2.new(1, -24, 0, 24)
	label.BackgroundTransparency = 1
	label.Font = Enum.Font.Bold
	label.Text = title
	label.TextColor3 = PAPER
	label.TextSize = 16
	label.TextXAlignment = Enum.TextXAlignment.Left
	label.Parent = node

	return node
end

local function makePort(
	node: NodeCanvasNode,
	id: string,
	valueType: string,
	direction: Enum_NodePortDirection,
	edge: Enum_NodePortEdge,
	x: number,
	y: number,
	colour: Color3
): NodeCanvasPort
	local port = Instance.new("NodeCanvasPort")
	port.Name = id
	port.PortId = id
	port.ValueType = valueType
	port.Direction = direction
	port.Edge = edge
	port.Position = UDim2.new(0, x, 0, y)
	port.Size = UDim2.new(0, 14, 0, 14)
	port.BackgroundColor3 = colour
	port.BorderSizePixel = 0
	port.Parent = node
	return port
end

local source = makeNode(group, "noise", "math.Noise", "Noise", 30, 54)
local sourceOutput = makePort(
	source, "value", "number", Enum.NodePortDirection.Output, Enum.NodePortEdge.Corner, 170, 50, VALUE
)

local transform = makeNode(group, "remap", "math.Remap", "Remap", 280, 54)
transform.InputPortLayout = Enum.InputPortLayout.Separate
-- A schema may expose this compatible pair as its optional pass-through.
-- Set BypassMode to Bypass at runtime to skip the evaluator without breaking
-- the graph's value flow.
transform.BypassInput = "value"
transform.BypassOutput = "value"
transform.BypassMode = Enum.NodeBypassMode.None
local transformInput = makePort(transform, "value", "number", Enum.NodePortDirection.Input, Enum.NodePortEdge.Corner, 6, 50, VALUE)
transformInput.MaxConnections = 1
local transformOutput = makePort(transform, "value", "number", Enum.NodePortDirection.Output, Enum.NodePortEdge.Corner, 170, 50, FLOW)

local display = makeNode(group, "display", "ui.Display", "Preview", 530, 54)
display.InputPortLayout = Enum.InputPortLayout.Squash
local displayInput = makePort(display, "value", "number", Enum.NodePortDirection.Input, Enum.NodePortEdge.Bottom, 6, 50, FLOW)

-- `Connect` refuses a wrong direction, mismatched ValueType, an endpoint in a
-- different canvas, or a cycle. Connecting another output to transformInput
-- replaces this wire because an input holds one value source.
local firstLink = canvas:Connect(sourceOutput, transformInput)
firstLink.Name = "NoiseToRemap"
firstLink.LineColor3 = VALUE
firstLink.LineTransparency = 0.05
firstLink.LineThickness = 3

local secondLink = canvas:Connect(transformOutput, displayInput)
secondLink.Name = "RemapToPreview"
secondLink.LineColor3 = FLOW

-- The graph owns no VM callbacks. Regular instance signals are the callback
-- boundary, so editor movement can decide when to recalculate group geometry
-- or enqueue separate async compute work.
source:GetPropertyChangedSignal("Position"):Connect(function()
	canvas:RefreshGroups()
end)

canvas:RefreshGroups()
