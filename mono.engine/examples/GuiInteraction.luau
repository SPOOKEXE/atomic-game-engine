--!strict
-- A live, visual check for the basic 2D controls and programmatic activation.
--
-- The regular Interface scene exercises layout and physical text entry in a
-- large composition. This keeps the interaction contract small enough that a
-- screenshot and the on-screen counters answer the narrower question: can
-- labels and images render, and do both button classes call their handlers?

local screen = Instance.new("ScreenGui")
screen.Name = "GuiInteractionDemo"

local localPlayer = game:GetService("Players").LocalPlayer
if localPlayer then
	local playerGui = localPlayer:FindFirstChild("PlayerGui")
	assert(playerGui, "the local player has no PlayerGui")
	screen.Parent = playerGui
else
	screen.Parent = game:GetService("StarterGui")
end

local BLACK = Color3.new(0, 0, 0)
local WHITE = Color3.new(1, 1, 1)
local MUTED = Color3.fromRGB(145, 155, 170)
local PANEL = Color3.fromRGB(13, 16, 22)
local BORDER = Color3.fromRGB(76, 92, 115)
local CYAN = Color3.fromRGB(92, 224, 232)
local GREEN = Color3.fromRGB(86, 226, 154)

local panel = Instance.new("Frame")
panel.Name = "InteractionPanel"
panel.AnchorPoint = Vector2.new(0.5, 0.5)
panel.Position = UDim2.new(0.5, 0, 0.5, 0)
panel.Size = UDim2.new(0, 620, 0, 410)
panel.BackgroundColor3 = PANEL
panel.BorderSizePixel = 0
panel.Parent = screen

local panelStroke = Instance.new("UIStroke")
panelStroke.Color = BORDER
panelStroke.Thickness = 1
panelStroke.Parent = panel

local title = Instance.new("TextLabel")
title.Name = "TextLabel"
title.Position = UDim2.new(0, 24, 0, 20)
title.Size = UDim2.new(1, -48, 0, 34)
title.BackgroundTransparency = 1
title.Text = "GUI INTERACTION CHECK"
title.TextColor3 = WHITE
title.TextSize = 24
title.TextXAlignment = Enum.TextXAlignment.Left
title.Parent = panel

local subtitle = Instance.new("TextLabel")
subtitle.Name = "LabelDetail"
subtitle.Position = UDim2.new(0, 24, 0, 58)
subtitle.Size = UDim2.new(1, -48, 0, 24)
subtitle.BackgroundTransparency = 1
subtitle.Text = "TextLabel, ImageLabel, TextBox, TextButton, ImageButton"
subtitle.TextColor3 = MUTED
subtitle.TextSize = 15
subtitle.TextXAlignment = Enum.TextXAlignment.Left
subtitle.Parent = panel

local divider = Instance.new("Frame")
divider.Name = "Divider"
divider.Position = UDim2.new(0, 24, 0, 94)
divider.Size = UDim2.new(1, -48, 0, 1)
divider.BackgroundColor3 = BORDER
divider.BorderSizePixel = 0
divider.Parent = panel

local imageLabel = Instance.new("ImageLabel")
imageLabel.Name = "ImageLabel"
imageLabel.Position = UDim2.new(0, 24, 0, 118)
imageLabel.Size = UDim2.new(0, 144, 0, 144)
imageLabel.BackgroundColor3 = BLACK
imageLabel.BorderSizePixel = 0
imageLabel.Image = "engine.Checker"
imageLabel.Parent = panel

local imageStroke = Instance.new("UIStroke")
imageStroke.Color = BORDER
imageStroke.Thickness = 1
imageStroke.Parent = imageLabel

local imageCaption = Instance.new("TextLabel")
imageCaption.Name = "ImageLabelCaption"
imageCaption.Position = UDim2.new(0, 24, 0, 270)
imageCaption.Size = UDim2.new(0, 144, 0, 20)
imageCaption.BackgroundTransparency = 1
imageCaption.Text = "ImageLabel: checker"
imageCaption.TextColor3 = MUTED
imageCaption.TextSize = 13
imageCaption.Parent = panel

local status = Instance.new("TextLabel")
status.Name = "InteractionStatus"
status.Position = UDim2.new(0, 194, 0, 118)
status.Size = UDim2.new(1, -218, 0, 32)
status.BackgroundTransparency = 1
status.TextColor3 = GREEN
status.TextSize = 18
status.TextXAlignment = Enum.TextXAlignment.Left
status.Parent = panel

local textButton = Instance.new("TextButton")
textButton.Name = "TextButton"
textButton.Position = UDim2.new(0, 194, 0, 164)
textButton.Size = UDim2.new(0, 260, 0, 52)
textButton.BackgroundColor3 = Color3.fromRGB(28, 46, 62)
textButton.BorderSizePixel = 0
textButton.Text = "TEXT BUTTON"
textButton.TextColor3 = WHITE
textButton.TextSize = 18
textButton.Parent = panel

local textButtonStroke = Instance.new("UIStroke")
textButtonStroke.Color = CYAN
textButtonStroke.Thickness = 1
textButtonStroke.Parent = textButton

local imageButton = Instance.new("ImageButton")
imageButton.Name = "ImageButton"
imageButton.Position = UDim2.new(0, 470, 0, 164)
imageButton.Size = UDim2.new(0, 126, 0, 52)
imageButton.BackgroundColor3 = BLACK
imageButton.BorderSizePixel = 0
imageButton.Image = "engine.Checker"
imageButton.Parent = panel

local imageButtonStroke = Instance.new("UIStroke")
imageButtonStroke.Color = CYAN
imageButtonStroke.Thickness = 1
imageButtonStroke.Parent = imageButton

local textBox = Instance.new("TextBox")
textBox.Name = "TextBox"
textBox.Position = UDim2.new(0, 194, 0, 236)
textBox.Size = UDim2.new(1, -218, 0, 42)
textBox.BackgroundColor3 = BLACK
textBox.BorderSizePixel = 0
textBox.ClearTextOnFocus = false
textBox.PlaceholderText = "TextBox accepts input"
textBox.Text = "TextBox component"
textBox.TextColor3 = WHITE
textBox.TextSize = 16
textBox.TextXAlignment = Enum.TextXAlignment.Left
textBox.Parent = panel

local textBoxStroke = Instance.new("UIStroke")
textBoxStroke.Color = BORDER
textBoxStroke.Thickness = 1
textBoxStroke.Parent = textBox

local detail = Instance.new("TextLabel")
detail.Name = "Detail"
detail.Position = UDim2.new(0, 194, 0, 294)
detail.Size = UDim2.new(1, -218, 0, 56)
detail.BackgroundTransparency = 1
detail.Text = "Each button starts with one programmatic activation. Click either control to increment its own count."
detail.TextColor3 = MUTED
detail.TextSize = 14
detail.TextWrapped = true
detail.TextXAlignment = Enum.TextXAlignment.Left
detail.TextYAlignment = Enum.TextYAlignment.Top
detail.Parent = panel

local textClicks = 0
local imageClicks = 0

local function refreshStatus()
	status.Text = string.format("PASS  TextButton %d   ImageButton %d", textClicks, imageClicks)
end

textButton.Activated:Connect(function()
	textClicks += 1
	refreshStatus()
end)

imageButton.Activated:Connect(function()
	imageClicks += 1
	refreshStatus()
end)

-- The scene fails loudly if a property reaches the wrong component. The
-- counters below are then the visible proof that both programmatic routes fired.
assert(title.Text == "GUI INTERACTION CHECK", "TextLabel did not retain text")
assert(imageLabel.Image == "engine.Checker", "ImageLabel did not retain image")
assert(textButton.Text == "TEXT BUTTON", "TextButton did not retain text")
assert(imageButton.Image == "engine.Checker", "ImageButton did not retain image")
assert(textBox.Text == "TextBox component", "TextBox did not retain text")

textButton:EmulateClick()
imageButton:EmulateClick()
assert(textClicks == 1 and imageClicks == 1, "a button did not emulate its click")
