--!strict
-- Client half of the UI lab. The scene script builds the interface; each player
-- interacts with their own PlayerGui copy, so the lab's handlers and state
-- (kept as attributes on the screen) live here.

local BLACK = Color3.new(0, 0, 0)
local WHITE = Color3.new(1, 1, 1)
local SURFACE = Color3.fromRGB(20, 20, 20)
local HOVER = Color3.fromRGB(36, 36, 36)
local BORDER = Color3.fromRGB(90, 90, 90)
local ACCENT = Color3.fromRGB(75, 210, 230)
local MAX_ITEMS = 8

local Players = game:GetService("Players")
local player = Players.LocalPlayer
if player == nil then
	return
end
local playerGui = player:WaitForChild("PlayerGui", 10)
assert(playerGui ~= nil, "PlayerGui did not arrive")
local screen = playerGui:WaitForChild("LabScreen", 10) :: ScreenGui
assert(screen ~= nil, "LabScreen did not arrive")

local function find(name: string): any
	local found = screen:FindFirstChild(name, true)
	assert(found ~= nil, name .. " is missing from the lab")
	return found
end

local feedbackLabel = find("Feedback") :: TextLabel
local clickButton = find("ClickButton") :: TextButton
local clickStroke = find("ClickOutline") :: UIStroke
local imageButton = find("ImageButton") :: ImageButton
local imageStroke = find("ImageOutline") :: UIStroke
local clickCount = find("ClickCount") :: TextLabel
local entry = find("Entry") :: TextBox
local entryStroke = find("EntryOutline") :: UIStroke
local entryState = find("EntryState") :: TextLabel
local scale075 = find("Scale075") :: TextButton
local scale100 = find("Scale100") :: TextButton
local scale150 = find("Scale150") :: TextButton
local scale075Stroke = find("Scale075Outline") :: UIStroke
local scale100Stroke = find("Scale100Outline") :: UIStroke
local scale150Stroke = find("Scale150Outline") :: UIStroke
local scaleSample = find("ScaleSample") :: TextButton
local scaleControl = find("ScaleControl") :: UIScale
local scaleValue = find("ScaleValue") :: TextLabel
local scrollList = find("ScrollList") :: ScrollingFrame
local rows: { TextButton } = {}
for index = 1, 12 do
	rows[index] = find("ScrollRow" .. index) :: TextButton
end
local selected = find("Selected") :: TextLabel
local dragTarget = find("DragTarget") :: Frame
local dragDetector = find("DragDetector") :: UIDragDetector
local toggleVisibility = find("ToggleVisibility") :: TextButton
local toggleStroke = find("ToggleOutline") :: UIStroke
local createItem = find("CreateItem") :: TextButton
local createStroke = find("CreateOutline") :: UIStroke
local destroyItem = find("DestroyItem") :: TextButton
local destroyStroke = find("DestroyOutline") :: UIStroke
local visibilityTarget = find("VisibilityTarget") :: TextLabel
local items = find("Items") :: Frame
local reset = find("Reset") :: TextButton
local resetStroke = find("ResetOutline") :: UIStroke
local status = find("Status") :: TextLabel

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
