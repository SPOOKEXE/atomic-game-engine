--!strict
-- A live, scrollable GUI scene for checking layout and interaction at different window sizes.

local BLACK = Color3.new(0, 0, 0)
local WHITE = Color3.new(1, 1, 1)
local SURFACE = Color3.fromRGB(20, 20, 20)
local HOVER = Color3.fromRGB(36, 36, 36)
local BORDER = Color3.fromRGB(90, 90, 90)
local MUTED = Color3.fromRGB(168, 168, 168)
local ACCENT = Color3.fromRGB(75, 210, 230)
local MAX_ITEMS = 8

local screen = Instance.new("ScreenGui")
screen.Name = "LabScreen"
screen.IgnoreGuiInset = false

-- Studio authors the template; a client connects handlers to its live PlayerGui tree.
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
local clickStroke = outline("ClickOutline", clickButton)

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
local imageStroke = outline("ImageOutline", imageButton)

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
local entryStroke = outline("EntryOutline", entry)
local entryState = label("EntryState", "idle", 249, 22, content)
entryState.TextColor3 = MUTED
entryState.TextSize = 14

local scaleTitle = label("ScaleTitle", "SCALE", 284, 18, content)
scaleTitle.TextSize = 14
local scale075 = button("Scale075", "75%", 0, 309, 0.333, 36)
local scale100 = button("Scale100", "100%", 0.333, 309, 0.333, 36)
local scale150 = button("Scale150", "150%", 0.666, 309, 0.334, 36)
local scale075Stroke = outline("Scale075Outline", scale075)
local scale100Stroke = outline("Scale100Outline", scale100)
local scale150Stroke = outline("Scale150Outline", scale150)

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
local toggleStroke = outline("ToggleOutline", toggleVisibility)
local createItem = button("CreateItem", "CREATE", 0.5, 961, 0.25, 36)
local createStroke = outline("CreateOutline", createItem)
local destroyItem = button("DestroyItem", "DELETE", 0.75, 961, 0.25, 36)
local destroyStroke = outline("DestroyOutline", destroyItem)
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
local resetStroke = outline("ResetOutline", reset)
local status = label("Status", "", 1165, 20, content)
status.TextColor3 = MUTED
status.TextSize = 11

local createdItems: {TextButton} = {}

local function countOf(name: string): number
	local value = screen:GetAttribute(name)
	if type(value) == "number" then
		return value
	end
	return 0
end

local function show(message: string)
	feedbackLabel.Text = message
	screen:SetAttribute("status", message)
	local submitted = ""
	local submittedValue = screen:GetAttribute("submitted")
	if type(submittedValue) == "string" then
		submitted = submittedValue
	end
	status.Text = string.format(
		"textClicks=%d imageClicks=%d sampleClicks=%d created=%d scale=%.2f submitted=%q " ..
			"selectedRow=%d visible=%s focused=%s dragStarts=%d dragEnds=%d submitCount=%d event=%q",
		countOf("textClicks"), countOf("imageClicks"), countOf("sampleClicks"), countOf("created"),
		countOf("scale"), submitted,
		countOf("selectedRow"), tostring(screen:GetAttribute("visible") == true),
		tostring(screen:GetAttribute("focused") == true), countOf("dragStarts"), countOf("dragEnds"),
		countOf("submitCount"), message
	)
end

local function refreshClicks()
	clickCount.Text = string.format("text %d  /  image %d", countOf("textClicks"), countOf("imageClicks"))
end

local function setScale(value: number)
	scaleControl.Scale = value
	screen:SetAttribute("scale", value)
	scaleValue.Text = string.format("scale %.2f", value)
	scale075Stroke.Color = if value == 0.75 then ACCENT else BORDER
	scale100Stroke.Color = if value == 1 then ACCENT else BORDER
	scale150Stroke.Color = if value == 1.5 then ACCENT else BORDER
	show(string.format("scale %.2f", value))
end

local function selectRow(index: number)
	local previous = countOf("selectedRow")
	if previous > 0 then
		rows[previous].BackgroundColor3 = SURFACE
	end
	rows[index].BackgroundColor3 = HOVER
	screen:SetAttribute("selectedRow", index)
	selected.Text = string.format("selected: %02d", index)
	show(string.format("row %02d selected", index))
end

local function createChild()
	local count = #createdItems
	if count >= MAX_ITEMS then
		show("item limit 8")
		return
	end
	local index = count + 1
	local item = Instance.new("TextButton")
	item.Name = "CreatedItem" .. index
	item.Position = UDim2.new(((index - 1) % 4) * 0.25, 0, 0, math.floor((index - 1) / 4) * 36)
	item.Size = UDim2.new(0.25, -4, 0, 32)
	item.BackgroundColor3 = SURFACE
	item.BorderSizePixel = 0
	item.Text = string.format("ITEM %d", index)
	item.TextColor3 = WHITE
	item.TextSize = 13
	item.Parent = items
	item.Activated:Connect(function()
		show(item.Name .. " activated")
	end)
	createdItems[index] = item
	screen:SetAttribute("created", index)
	show(string.format("item %d created", index))
end

local function destroyChild()
	local count = #createdItems
	if count == 0 then
		show("no item to delete")
		return
	end
	local item = table.remove(createdItems, count)
	assert(item, "item count disagrees with created items")
	item:Destroy()
	screen:SetAttribute("created", count - 1)
	show(string.format("item %d deleted", count))
end

local function resetAll()
	entry:VirtualUnfocus()
	screen:SetAttribute("textClicks", 0)
	screen:SetAttribute("imageClicks", 0)
	screen:SetAttribute("sampleClicks", 0)
	screen:SetAttribute("submitted", "")
	screen:SetAttribute("submitCount", 0)
	screen:SetAttribute("focused", false)
	screen:SetAttribute("selectedRow", 0)
	screen:SetAttribute("visible", true)
	screen:SetAttribute("dragStarts", 0)
	screen:SetAttribute("dragEnds", 0)
	refreshClicks()
	clickButton.BackgroundColor3 = SURFACE
	clickButton.TextColor3 = WHITE
	clickStroke.Color = BORDER
	imageButton.BackgroundColor3 = SURFACE
	imageButton.ImageColor3 = WHITE
	imageStroke.Color = BORDER
	entry.Text = ""
	entryState.Text = "idle"
	entryStroke.Color = BORDER
	for index = 1, 12 do
		rows[index].BackgroundColor3 = SURFACE
	end
	selected.Text = "selected: none"
	scrollList.CanvasPosition = Vector2.new(0, 0)
	visibilityTarget.Visible = true
	setScale(1)
	dragTarget.Position = UDim2.new(0, 12, 0, 19)
	while #createdItems > 0 do
		local item = table.remove(createdItems)
		assert(item, "item count disagrees with created items")
		item:Destroy()
	end
	screen:SetAttribute("created", 0)
	show("ready")
end

local function feedback(control: GuiObject, stroke: UIStroke)
	control.MouseEnter:Connect(function()
		stroke.Color = ACCENT
		control.BackgroundColor3 = HOVER
		show(control.Name .. " hover")
	end)
	control.MouseLeave:Connect(function()
		stroke.Color = BORDER
		control.BackgroundColor3 = SURFACE
	end)
	control.MouseButton1Down:Connect(function()
		control.BackgroundColor3 = WHITE
		show(control.Name .. " pressed")
	end)
	control.MouseButton1Up:Connect(function()
		control.BackgroundColor3 = HOVER
	end)
end

feedback(clickButton, clickStroke)
feedback(imageButton, imageStroke)
feedback(toggleVisibility, toggleStroke)
feedback(createItem, createStroke)
feedback(destroyItem, destroyStroke)
feedback(reset, resetStroke)

clickButton.MouseButton1Down:Connect(function()
	clickButton.TextColor3 = BLACK
end)
clickButton.MouseButton1Up:Connect(function()
	clickButton.TextColor3 = WHITE
end)
clickButton.MouseLeave:Connect(function()
	clickButton.TextColor3 = WHITE
end)
imageButton.MouseEnter:Connect(function()
	imageButton.ImageColor3 = ACCENT
end)
imageButton.MouseLeave:Connect(function()
	imageButton.ImageColor3 = WHITE
end)
imageButton.MouseButton1Down:Connect(function()
	imageButton.ImageColor3 = BLACK
end)
imageButton.MouseButton1Up:Connect(function()
	imageButton.ImageColor3 = ACCENT
end)

clickButton.Activated:Connect(function()
	screen:SetAttribute("textClicks", countOf("textClicks") + 1)
	refreshClicks()
	show("text button activated")
end)

imageButton.Activated:Connect(function()
	screen:SetAttribute("imageClicks", countOf("imageClicks") + 1)
	refreshClicks()
	show("image button activated")
end)

scaleSample.Activated:Connect(function()
	screen:SetAttribute("sampleClicks", countOf("sampleClicks") + 1)
	show("scaled sample activated")
end)

entry.Focused:Connect(function()
	screen:SetAttribute("focused", true)
	entryStroke.Color = ACCENT
	entryState.Text = "focused"
	show("entry focused")
end)

entry.FocusLost:Connect(function(enterPressed: boolean)
	screen:SetAttribute("focused", false)
	entryStroke.Color = BORDER
	if enterPressed then
		screen:SetAttribute("submitted", entry.Text)
		screen:SetAttribute("submitCount", countOf("submitCount") + 1)
		entryState.Text = "submitted: " .. entry.Text
		show("entry submitted")
	else
		entryState.Text = "focus lost"
		show("entry focus lost")
	end
end)

scale075.Activated:Connect(function()
	setScale(0.75)
end)
scale100.Activated:Connect(function()
	setScale(1)
end)
scale150.Activated:Connect(function()
	setScale(1.5)
end)

for index = 1, 12 do
	rows[index].Activated:Connect(function()
		selectRow(index)
	end)
end

dragDetector.DragStart:Connect(function()
	screen:SetAttribute("dragStarts", countOf("dragStarts") + 1)
	show("drag started")
end)
dragDetector.DragEnd:Connect(function()
	screen:SetAttribute("dragEnds", countOf("dragEnds") + 1)
	show("drag ended")
end)

toggleVisibility.Activated:Connect(function()
	local visible = screen:GetAttribute("visible") ~= false
	visibilityTarget.Visible = not visible
	screen:SetAttribute("visible", not visible)
	show(if visible then "target hidden" else "target visible")
end)
createItem.Activated:Connect(createChild)
destroyItem.Activated:Connect(destroyChild)
reset.Activated:Connect(resetAll)

resetAll()
