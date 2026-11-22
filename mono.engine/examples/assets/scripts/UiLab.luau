--!strict
-- A live, scrollable GUI scene for checking layout and interaction at different window sizes.

local BLACK = Color3.new(0, 0, 0)
local WHITE = Color3.new(1, 1, 1)
local SURFACE = Color3.fromRGB(20, 20, 20)
local BORDER = Color3.fromRGB(90, 90, 90)
local MUTED = Color3.fromRGB(168, 168, 168)
local ACCENT = Color3.fromRGB(75, 210, 230)

local screen = Instance.new("ScreenGui")
screen.Name = "LabScreen"
screen.IgnoreGuiInset = false

-- This script authors the interface. The client script connects its handlers
-- to the player's copy.
local localPlayer = game:GetService("Players").LocalPlayer
if localPlayer then
	local playerGui = localPlayer:FindFirstChild("PlayerGui")
	assert(playerGui, "the local player has no PlayerGui")
	screen.Parent = playerGui
else
	screen.Parent = game:GetService("StarterGui")
end

local backdrop = Instance.new("Frame")
backdrop.Name = "Backdrop"
backdrop.Size = UDim2.new(1, 0, 1, 0)
backdrop.BackgroundColor3 = BLACK
backdrop.BorderSizePixel = 0
backdrop.Parent = screen

local content = Instance.new("ScrollingFrame")
content.Name = "Content"
content.AnchorPoint = Vector2.new(0.5, 0)
content.Position = UDim2.new(0.5, 0, 0, 0)
content.Size = UDim2.new(0.94, 0, 1, 0)
content.CanvasSize = UDim2.new(0, 0, 0, 1200)
content.ScrollingDirection = Enum.ScrollingDirection.Y
content.ScrollBarThickness = 4
content.ScrollBarImageColor3 = BORDER
content.BackgroundColor3 = BLACK
content.BorderSizePixel = 0
content.ClipsDescendants = true
content.Parent = screen

local contentLimit = Instance.new("UISizeConstraint")
contentLimit.Name = "ContentWidthLimit"
contentLimit.MaxSize = Vector2.new(620, 4096)
contentLimit.MinSize = Vector2.new(0, 0)
contentLimit.Parent = content

local function label(name: string, caption: string, y: number, height: number, parent: Instance): TextLabel
	local item = Instance.new("TextLabel")
	item.Name = name
	item.Position = UDim2.new(0, 0, 0, y)
	item.Size = UDim2.new(1, -8, 0, height)
	item.BackgroundTransparency = 1
	item.Text = caption
	item.TextColor3 = WHITE
	item.TextSize = 16
	item.TextXAlignment = Enum.TextXAlignment.Left
	item.Parent = parent
	return item
end

local function outline(name: string, parent: Instance): UIStroke
	local stroke = Instance.new("UIStroke")
	stroke.Name = name
	stroke.Color = BORDER
	stroke.Thickness = 1
	stroke.Parent = parent
	return stroke
end

local function button(
	name: string, caption: string, x: number, y: number, width: number, height: number
): TextButton
	local item = Instance.new("TextButton")
	item.Name = name
	item.Position = UDim2.new(x, 0, 0, y)
	item.Size = UDim2.new(width, -6, 0, height)
	item.BackgroundColor3 = SURFACE
	item.BorderSizePixel = 0
	item.AutoButtonColor = false
	item.Text = caption
	item.TextColor3 = WHITE
	item.TextSize = 15
	item.Parent = content
	return item
end

local title = label("Title", "UI LAB", 12, 32, content)
title.TextSize = 25
local feedbackLabel = label("Feedback", "ready", 48, 24, content)
feedbackLabel.TextColor3 = ACCENT
feedbackLabel.TextSize = 14

local divider = Instance.new("Frame")
divider.Name = "TitleDivider"
divider.Position = UDim2.new(0, 0, 0, 78)
divider.Size = UDim2.new(1, -8, 0, 1)
divider.BackgroundColor3 = BORDER
divider.BorderSizePixel = 0
divider.Parent = content

local clickButton = button("ClickButton", "TEXT BUTTON", 0, 92, 0.6, 42)
outline("ClickOutline", clickButton)

local imageButton = Instance.new("ImageButton")
imageButton.Name = "ImageButton"
imageButton.Position = UDim2.new(0.6, 0, 0, 92)
imageButton.Size = UDim2.new(0.4, -8, 0, 42)
imageButton.BackgroundColor3 = SURFACE
imageButton.BorderSizePixel = 0
imageButton.AutoButtonColor = false
imageButton.Image = "engine.Checker"
imageButton.ImageColor3 = WHITE
imageButton.Parent = content
outline("ImageOutline", imageButton)

local clickCount = label("ClickCount", "text 0  /  image 0", 141, 22, content)
clickCount.TextColor3 = MUTED
clickCount.TextSize = 14

local entryTitle = label("EntryTitle", "TEXT INPUT", 176, 18, content)
entryTitle.TextSize = 14
local entry = Instance.new("TextBox")
entry.Name = "Entry"
entry.Position = UDim2.new(0, 0, 0, 200)
entry.Size = UDim2.new(1, -8, 0, 42)
entry.BackgroundColor3 = SURFACE
entry.BorderSizePixel = 0
entry.ClearTextOnFocus = false
entry.PlaceholderText = "Type and press Enter"
entry.PlaceholderColor3 = MUTED
entry.Text = ""
entry.TextColor3 = WHITE
entry.TextSize = 16
entry.TextXAlignment = Enum.TextXAlignment.Left
entry.Parent = content
outline("EntryOutline", entry)
local entryState = label("EntryState", "idle", 249, 22, content)
entryState.TextColor3 = MUTED
entryState.TextSize = 14

local scaleTitle = label("ScaleTitle", "SCALE", 284, 18, content)
scaleTitle.TextSize = 14
local scale075 = button("Scale075", "75%", 0, 309, 0.333, 36)
local scale100 = button("Scale100", "100%", 0.333, 309, 0.333, 36)
local scale150 = button("Scale150", "150%", 0.666, 309, 0.334, 36)
outline("Scale075Outline", scale075)
outline("Scale100Outline", scale100)
outline("Scale150Outline", scale150)

local scaleSample = Instance.new("TextButton")
scaleSample.Name = "ScaleSample"
scaleSample.AnchorPoint = Vector2.new(0.5, 0)
scaleSample.Position = UDim2.new(0.5, 0, 0, 363)
scaleSample.Size = UDim2.new(0, 164, 0, 46)
scaleSample.BackgroundColor3 = ACCENT
scaleSample.BorderSizePixel = 0
scaleSample.AutoButtonColor = false
scaleSample.Text = "SAMPLE"
scaleSample.TextColor3 = BLACK
scaleSample.TextSize = 16
scaleSample.Parent = content
local scaleControl = Instance.new("UIScale")
scaleControl.Name = "ScaleControl"
scaleControl.Scale = 1
scaleControl.Parent = scaleSample
local scaleValue = label("ScaleValue", "scale 1.00", 435, 20, content)
scaleValue.TextColor3 = MUTED
scaleValue.TextSize = 14

local aspectTitle = label("AspectTitle", "ASPECT 16:9", 469, 18, content)
aspectTitle.TextSize = 14
local aspectSample = Instance.new("Frame")
aspectSample.Name = "AspectSample"
aspectSample.AnchorPoint = Vector2.new(0.5, 0)
aspectSample.Position = UDim2.new(0.5, 0, 0, 496)
-- The authored height is deliberately wrong so the constraint visibly owns it.
aspectSample.Size = UDim2.new(0, 196, 0, 140)
aspectSample.BackgroundColor3 = SURFACE
aspectSample.BorderSizePixel = 0
aspectSample.Parent = content
outline("AspectOutline", aspectSample)
local aspectConstraint = Instance.new("UIAspectRatioConstraint")
aspectConstraint.Name = "AspectConstraint"
aspectConstraint.AspectRatio = 16 / 9
aspectConstraint.DominantAxis = Enum.DominantAxis.Width
aspectConstraint.Parent = aspectSample
local aspectText = label("AspectText", "16:9 PREVIEW", 0, 110, aspectSample)
aspectText.Size = UDim2.new(1, 0, 1, 0)
aspectText.TextXAlignment = Enum.TextXAlignment.Center
aspectText.TextColor3 = MUTED

local listTitle = label("ListTitle", "SCROLL  /  SELECT", 613, 18, content)
listTitle.TextSize = 14
local scrollList = Instance.new("ScrollingFrame")
scrollList.Name = "ScrollList"
scrollList.Position = UDim2.new(0, 0, 0, 638)
scrollList.Size = UDim2.new(1, -8, 0, 140)
scrollList.CanvasSize = UDim2.new(0, 0, 0, 12 * 34)
scrollList.ScrollingDirection = Enum.ScrollingDirection.Y
scrollList.ScrollBarThickness = 4
scrollList.ScrollBarImageColor3 = ACCENT
scrollList.BackgroundColor3 = SURFACE
scrollList.BorderSizePixel = 0
scrollList.ClipsDescendants = true
scrollList.Parent = content
local rows: {TextButton} = {}
for index = 1, 12 do
	local row = Instance.new("TextButton")
	row.Name = "ScrollRow" .. index
	row.Position = UDim2.new(0, 0, 0, (index - 1) * 34)
	row.Size = UDim2.new(1, -8, 0, 32)
	row.BackgroundColor3 = SURFACE
	row.BorderSizePixel = 0
	row.AutoButtonColor = false
	row.Text = string.format("%02d  SELECT ROW", index)
	row.TextColor3 = WHITE
	row.TextSize = 14
	row.TextXAlignment = Enum.TextXAlignment.Left
	row.Parent = scrollList
	rows[index] = row
end
local selected = label("Selected", "selected: none", 783, 20, content)
selected.TextColor3 = MUTED
selected.TextSize = 14

local dragTitle = label("DragTitle", "DRAG", 813, 18, content)
dragTitle.TextSize = 14
local dragArea = Instance.new("Frame")
dragArea.Name = "DragArea"
dragArea.Position = UDim2.new(0, 0, 0, 838)
dragArea.Size = UDim2.new(1, -8, 0, 80)
dragArea.BackgroundColor3 = SURFACE
dragArea.BorderSizePixel = 0
dragArea.ClipsDescendants = true
dragArea.Parent = content
local dragTarget = Instance.new("Frame")
dragTarget.Name = "DragTarget"
dragTarget.Position = UDim2.new(0, 12, 0, 19)
dragTarget.Size = UDim2.new(0, 106, 0, 42)
dragTarget.BackgroundColor3 = ACCENT
dragTarget.BorderSizePixel = 0
dragTarget.Parent = dragArea
local dragText = label("DragText", "DRAG ME", 0, 42, dragTarget)
dragText.Size = UDim2.new(1, 0, 1, 0)
dragText.TextXAlignment = Enum.TextXAlignment.Center
dragText.TextColor3 = BLACK
local dragDetector = Instance.new("UIDragDetector")
dragDetector.Name = "DragDetector"
dragDetector.DragStyle = Enum.UIDragDetectorDragStyle.TranslatePlane
dragDetector.ResponseStyle = Enum.UIDragDetectorDragStyleResponse.Offset
dragDetector.BoundingUI = dragArea
dragDetector.Parent = dragTarget

local dynamicTitle = label("DynamicTitle", "TREE CONTROLS", 936, 18, content)
dynamicTitle.TextSize = 14
local toggleVisibility = button("ToggleVisibility", "HIDE / SHOW", 0, 961, 0.5, 36)
outline("ToggleOutline", toggleVisibility)
local createItem = button("CreateItem", "CREATE", 0.5, 961, 0.25, 36)
outline("CreateOutline", createItem)
local destroyItem = button("DestroyItem", "DELETE", 0.75, 961, 0.25, 36)
outline("DestroyOutline", destroyItem)
local visibilityTarget = label("VisibilityTarget", "VISIBILITY TARGET", 1005, 20, content)
visibilityTarget.TextColor3 = ACCENT
visibilityTarget.TextSize = 14
local items = Instance.new("Frame")
items.Name = "Items"
items.Position = UDim2.new(0, 0, 0, 1033)
items.Size = UDim2.new(1, -8, 0, 72)
items.BackgroundColor3 = BLACK
items.BorderSizePixel = 0
items.Parent = content
local reset = button("Reset", "RESET ALL", 0, 1120, 1, 38)
outline("ResetOutline", reset)
local status = label("Status", "", 1165, 20, content)
status.TextColor3 = MUTED
status.TextSize = 11

-- Player input only reaches the client, so every handler and the lab's state
-- live in client/DemoCameras/UiLab.client.luau, on the player's copy of it.
