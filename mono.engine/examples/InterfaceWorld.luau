--!strict
-- One frame that exercises screen, surface, billboard and nested 3D interface
-- rendering together. It is intentionally static so captures are comparable.

local StarterGui = game:GetService("StarterGui")

-- Clear of a baseplate's top face at y = 0: coplanar surfaces do not flicker in
-- this engine, they resolve stably to whichever one the draw order happens to
-- favour. `scripts/demos/zfight-scan.sh` holds the control for that.
local GROUND_LIFT = 0.01

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = Vector3.new(32, 1, 24)
floor.CFrame = CFrame.new(Vector3.new(0, -0.5 + GROUND_LIFT, 0))
floor.Color = Color3.fromRGB(42, 48, 62)
floor.Parent = workspace

local pane = Instance.new("Part")
pane.Name = "SurfacePanel"
pane.Anchored = true
pane.Size = Vector3.new(10, 6, 0.4)
pane.CFrame = CFrame.new(Vector3.new(-5, 4, 0))
pane.Color = Color3.fromRGB(28, 32, 42)
pane.Parent = workspace

local surface = Instance.new("SurfaceGui")
surface.Name = "WorldControls"
surface.Adornee = pane
surface.Face = Enum.NormalId.Back
surface.SizingMode = Enum.SurfaceSizingMode.PixelsPerStud
surface.PixelsPerStud = 64
surface.LightInfluence = 0.55
surface.Brightness = 1.15

-- **A surface gui takes no pointer unless it says so**, which is Roblox's
-- default and the reason the rows below are clickable at all: an interactive
-- surface intercepts every click crossing its plane, including the ones meant
-- for whatever is behind it, so it is opt-in.
surface.Active = true
surface.MaxDistance = 220
surface.ZOffset = 0.02
surface.Parent = pane

local surfaceCard = Instance.new("CanvasGroup")
surfaceCard.Name = "TintedGroup"
surfaceCard.Size = UDim2.new(1, -32, 1, -32)
surfaceCard.Position = UDim2.new(0, 16, 0, 16)
surfaceCard.BackgroundColor3 = Color3.fromRGB(34, 48, 70)
surfaceCard.BorderSizePixel = 0
surfaceCard.GroupColor3 = Color3.fromRGB(220, 238, 255)
surfaceCard.GroupTransparency = 0.05
surfaceCard.Parent = surface

local surfaceCorner = Instance.new("UICorner")
surfaceCorner.CornerRadius = UDim.new(0, 20)
surfaceCorner.Parent = surfaceCard

local surfaceStroke = Instance.new("UIStroke")
surfaceStroke.Color = Color3.fromRGB(90, 190, 255)
surfaceStroke.Thickness = 4

-- The panel is a `CanvasGroup` with no text of its own, so a contextual stroke
-- falls back to outlining the rectangle - which is what the border is for.
surfaceStroke.ApplyStrokeMode = Enum.ApplyStrokeMode.Border
surfaceStroke.Parent = surfaceCard

-- A gradient on the card, and a second one on the outline. The two are the same
-- class parented one level apart, which is what lets an outline fade along its
-- length without a second set of properties.
local cardRamp = Instance.new("UIGradient")
cardRamp.Color = ColorSequence.new(Color3.fromRGB(255, 255, 255), Color3.fromRGB(150, 190, 240))
cardRamp.Rotation = 90
cardRamp.Parent = surfaceCard

local strokeRamp = Instance.new("UIGradient")
strokeRamp.Transparency = NumberSequence.new(0, 0.85)
strokeRamp.Parent = surfaceStroke

local title = Instance.new("TextLabel")
title.Name = "SurfaceTitle"
title.Size = UDim2.new(1, -32, 0, 90)
title.Position = UDim2.new(0, 16, 0, 12)
title.BackgroundTransparency = 1
title.RichText = true
title.Text = '<b>SURFACE GUI</b>\n<font color="#5ABEFF">rounded</font>, lit, <i>clipped</i>' 
title.TextColor3 = Color3.new(1, 1, 1)
title.TextStrokeColor3 = Color3.new(0, 0, 0)
title.TextStrokeTransparency = 0.15
title.TextWrapped = true
title.TextSize = 28
title.Font = Enum.Font.Bold
title.Parent = surfaceCard

local scroller = Instance.new("ScrollingFrame")
scroller.Name = "SurfaceScroller"
scroller.Size = UDim2.new(1, -32, 1, -126)
scroller.Position = UDim2.new(0, 16, 0, 106)
scroller.BackgroundColor3 = Color3.fromRGB(20, 26, 38)
scroller.BorderSizePixel = 0
scroller.CanvasSize = UDim2.new(0, 0, 2, 0)
scroller.CanvasPosition = Vector2.new(0, 70)
scroller.ScrollBarThickness = 12
scroller.ScrollBarImageColor3 = Color3.fromRGB(90, 190, 255)

-- The canvas grows to the rows rather than being a number kept in step with
-- them by hand, and the bar reserves its strip so the list does not shift
-- sideways the first time it overflows.
scroller.AutomaticCanvasSize = Enum.AutomaticSize.Y
scroller.CanvasSize = UDim2.new()
scroller.VerticalScrollBarInset = Enum.ScrollBarInset.Always
scroller.Parent = surfaceCard

local list = Instance.new("UIListLayout")
list.Padding = UDim.new(0, 10)
list.Parent = scroller

for index = 1, 7 do
	local row = Instance.new("TextButton")
	row.Name = "WorldButton" .. tostring(index)
	row.Size = UDim2.new(1, -24, 0, 54)
	row.BackgroundColor3 = index % 2 == 0 and Color3.fromRGB(56, 80, 110)
		or Color3.fromRGB(46, 64, 90)
	row.BorderSizePixel = 0
	row.Text = "interactive row " .. tostring(index)
	row.TextColor3 = Color3.new(1, 1, 1)
	row.TextSize = 21
	row.Parent = scroller

	local corner = Instance.new("UICorner")
	corner.CornerRadius = UDim.new(0, 12)
	corner.Parent = row
end

local marker = Instance.new("Part")
marker.Name = "BillboardMarker"
marker.Anchored = true
marker.Size = Vector3.new(2.5, 2.5, 2.5)
marker.CFrame = CFrame.new(Vector3.new(10, 2, -1))
marker.Color = Color3.fromRGB(230, 120, 76)
marker.Parent = workspace

local billboard = Instance.new("BillboardGui")
billboard.Name = "MarkerLabel"
billboard.Adornee = marker
billboard.Size = UDim2.new(4, 0, 1.4, 0)
billboard.StudsOffset = Vector3.new(0, 2.8, 0)
billboard.MaxDistance = 80
billboard.LightInfluence = 0.25
billboard.AlwaysOnTop = false

-- Stepped, so the label's text stops re-fitting on every frame a player walks -
-- it changes size in visible jumps and holds still between them.
billboard.DistanceStep = 4
billboard.Brightness = 1.1
billboard.SizeOffset = Vector2.new(0, -0.25)
billboard.Parent = marker

local bubble = Instance.new("TextLabel")
bubble.Size = UDim2.new(1, 0, 1, 0)
bubble.BackgroundColor3 = Color3.fromRGB(232, 238, 248)
bubble.BorderSizePixel = 0
bubble.Text = "BILLBOARD GUI"
bubble.TextColor3 = Color3.fromRGB(24, 30, 42)
bubble.TextSize = 24
bubble.Font = Enum.Font.Bold
bubble.Parent = billboard

local bubbleCorner = Instance.new("UICorner")
bubbleCorner.CornerRadius = UDim.new(0, 14)
bubbleCorner.Parent = bubble

local screen = Instance.new("ScreenGui")
screen.Name = "InterfaceWorldOverlay"
screen.Parent = StarterGui

local previewPane = Instance.new("Part")
previewPane.Name = "ViewportPanel"
previewPane.Anchored = true
previewPane.Size = Vector3.new(6, 3.5, 0.4)
previewPane.CFrame = CFrame.new(Vector3.new(5, 4, 0))
previewPane.Color = Color3.fromRGB(18, 22, 32)
previewPane.Parent = workspace

local previewSurface = Instance.new("SurfaceGui")
previewSurface.Name = "ViewportSurface"
previewSurface.Adornee = previewPane
previewSurface.Face = Enum.NormalId.Back
previewSurface.CanvasSize = Vector2.new(640, 360)
previewSurface.Parent = previewPane

local viewport = Instance.new("ViewportFrame")
viewport.Name = "NestedScene"
viewport.Size = UDim2.new(1, 0, 1, 0)
viewport.BackgroundColor3 = Color3.fromRGB(14, 18, 28)
viewport.BorderSizePixel = 0
viewport.Ambient = Color3.fromRGB(120, 136, 166)
viewport.LightColor = Color3.fromRGB(255, 220, 180)
viewport.LightDirection = Vector3.new(-1, -1, -1)
viewport.Parent = previewSurface

local viewportCorner = Instance.new("UICorner")
viewportCorner.CornerRadius = UDim.new(0, 16)
viewportCorner.Parent = viewport

local viewportStroke = Instance.new("UIStroke")
viewportStroke.Color = Color3.fromRGB(100, 190, 255)
viewportStroke.Thickness = 2
viewportStroke.Parent = viewport

local previewCamera = Instance.new("Camera")
previewCamera.Name = "PreviewCamera"
previewCamera.CFrame = CFrame.lookAt(Vector3.new(7, 5, 9), Vector3.new(0, 0, 0))
previewCamera.Parent = viewport
viewport.CurrentCamera = previewCamera

local previewFloor = Instance.new("Part")
previewFloor.Name = "PreviewFloor"
previewFloor.Anchored = true
previewFloor.Size = Vector3.new(9, 0.5, 7)
previewFloor.CFrame = CFrame.new(Vector3.new(0, -1.5, 0))
previewFloor.Color = Color3.fromRGB(54, 62, 82)
previewFloor.Parent = viewport

local previewCube = Instance.new("Part")
previewCube.Name = "PreviewCube"
previewCube.Anchored = true
previewCube.Size = Vector3.new(2.5, 2.5, 2.5)
previewCube.CFrame = CFrame.new(Vector3.new(0, 0, 0))
previewCube.Color = Color3.fromRGB(96, 190, 238)
previewCube.Parent = viewport

local caption = Instance.new("TextLabel")
caption.Name = "ViewportCaption"
caption.Size = UDim2.new(1, 0, 0, 30)
caption.Position = UDim2.new(0, 0, 1, -30)
caption.BackgroundColor3 = Color3.fromRGB(12, 16, 24)
caption.BackgroundTransparency = 0.2
caption.BorderSizePixel = 0
caption.Text = "VIEWPORT FRAME"
caption.TextColor3 = Color3.new(1, 1, 1)
caption.TextSize = 16
caption.Font = Enum.Font.Bold
caption.ZIndex = 2
caption.Parent = viewport

local camera = Instance.new("Camera")
camera.Name = "InterfaceWorldCamera"
camera.CFrame = CFrame.lookAt(Vector3.new(0, 8, 22), Vector3.new(0, 3, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera
