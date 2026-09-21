--!strict
-- A visual fixture for text rendering. The capture harness reads its coloured
-- glyph regions at two viewport sizes, so a screenshot proves the labels are
-- composed after layout rather than only appearing in draw-command metadata.

local screen = Instance.new("ScreenGui")
screen.Name = "UiTextVisualDemo"

local localPlayer = game:GetService("Players").LocalPlayer
if localPlayer then
	local playerGui = localPlayer:FindFirstChild("PlayerGui")
	assert(playerGui, "the local player has no PlayerGui")
	screen.Parent = playerGui
else
	screen.Parent = game:GetService("StarterGui")
end

local BLACK = Color3.new(0, 0, 0)
local PANEL = Color3.fromRGB(13, 16, 22)
local BORDER = Color3.fromRGB(76, 92, 115)
local WHITE = Color3.new(1, 1, 1)
local CYAN = Color3.fromRGB(92, 224, 232)
local PURPLE = Color3.fromRGB(190, 132, 255)
local YELLOW = Color3.fromRGB(255, 226, 92)

local panel = Instance.new("Frame")
panel.Name = "TextVisualPanel"
panel.AnchorPoint = Vector2.new(0.5, 0.5)
panel.Position = UDim2.new(0.5, 0, 0.5, 0)
panel.Size = UDim2.new(0.72, 0, 0.62, 0)
panel.BackgroundColor3 = PANEL
panel.BorderSizePixel = 0
panel.Parent = screen

local panelStroke = Instance.new("UIStroke")
panelStroke.Color = BORDER
panelStroke.Thickness = 1
panelStroke.Parent = panel

local title = Instance.new("TextLabel")
title.Name = "TextLabel"
title.Position = UDim2.new(0, 28, 0, 20)
title.Size = UDim2.new(1, -56, 0.16, 0)
title.BackgroundTransparency = 1
title.Text = "TEXT LABEL VISUAL CHECK"
title.TextColor3 = WHITE
title.TextSize = 26
title.Font = Enum.Font.Bold
title.TextXAlignment = Enum.TextXAlignment.Left
title.TextYAlignment = Enum.TextYAlignment.Center
title.Parent = panel

local rich = Instance.new("TextLabel")
rich.Name = "RichTextLabel"
rich.Position = UDim2.new(0, 28, 0.20, 0)
rich.Size = UDim2.new(1, -56, 0.18, 0)
rich.BackgroundTransparency = 1
rich.RichText = true
rich.Text = '<font color="#5CE0E8"><b>CYAN BOLD</b></font>  <font color="#FF9D4E"><i>ORANGE ITALIC</i></font>  <font color="#BE84FF" face="Code">CODE 0123</font>'
rich.TextColor3 = WHITE
rich.TextSize = 21
rich.TextWrapped = true
rich.TextXAlignment = Enum.TextXAlignment.Left
rich.TextYAlignment = Enum.TextYAlignment.Center
rich.Parent = panel

local regular = Instance.new("TextLabel")
regular.Name = "RegularFontLabel"
regular.Position = UDim2.new(0, 28, 0.42, 0)
regular.Size = UDim2.new(0.45, -34, 0.12, 0)
regular.BackgroundTransparency = 1
regular.Text = "Sphinx 01"
regular.TextColor3 = CYAN
regular.TextSize = 18
regular.Font = Enum.Font.Regular
regular.TextXAlignment = Enum.TextXAlignment.Left
regular.Parent = panel

local code = Instance.new("TextLabel")
code.Name = "CodeFontLabel"
code.Position = UDim2.new(0.45, 0, 0.42, 0)
code.Size = UDim2.new(0.55, -28, 0.12, 0)
code.BackgroundTransparency = 1
code.Text = "Sphinx 01"
code.TextColor3 = PURPLE
code.TextSize = 18
code.Font = Enum.Font.Code
code.TextXAlignment = Enum.TextXAlignment.Left
code.Parent = panel

local scaledBox = Instance.new("Frame")
scaledBox.Name = "ScaledTextBox"
scaledBox.Position = UDim2.new(0, 28, 0.61, 0)
scaledBox.Size = UDim2.new(1, -56, 0.28, 0)
scaledBox.BackgroundColor3 = BLACK
scaledBox.BorderSizePixel = 0
scaledBox.Parent = panel

local scaledStroke = Instance.new("UIStroke")
scaledStroke.Color = BORDER
scaledStroke.Thickness = 1
scaledStroke.Parent = scaledBox

local scaled = Instance.new("TextLabel")
scaled.Name = "ScaledTextLabel"
scaled.Size = UDim2.new(1, -24, 1, -20)
scaled.Position = UDim2.new(0, 12, 0, 10)
scaled.BackgroundTransparency = 1
scaled.Text = "TEXTSCALED RESPONDS TO VIEWPORT SIZE"
scaled.TextColor3 = YELLOW
scaled.TextSize = 72
scaled.TextScaled = true
scaled.TextXAlignment = Enum.TextXAlignment.Center
scaled.TextYAlignment = Enum.TextYAlignment.Center
scaled.Parent = scaledBox

assert(title.Text == "TEXT LABEL VISUAL CHECK", "TextLabel did not retain text")
assert(rich.RichText, "RichText did not retain its enabled state")
assert(regular.Font == Enum.Font.Regular, "regular font did not retain its face")
assert(code.Font == Enum.Font.Code, "code font did not retain its face")
assert(scaled.TextScaled, "TextScaled did not retain its enabled state")
