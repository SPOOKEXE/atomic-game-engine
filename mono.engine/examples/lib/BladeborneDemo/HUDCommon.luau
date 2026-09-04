--!strict
-- Engine adapter for Bladeborne's HUDCommon module. The widget-facing surface
-- mirrors the source module while local code owns the one demo ScreenGui.

local Fusion = require((script :: any).Parent.Fusion)

local Module = {}
Module.PALETTE = {
	bg0 = Color3.fromRGB(5, 6, 8),
	bg1 = Color3.fromRGB(14, 16, 20),
	panel = Color3.fromRGB(31, 34, 39),
	ink = Color3.fromRGB(235, 237, 242),
	inkDim = Color3.fromRGB(158, 163, 175),
	accent = Color3.fromRGB(208, 211, 219),
	gold = Color3.fromRGB(246, 187, 61),
	danger = Color3.fromRGB(220, 78, 72),
	success = Color3.fromRGB(105, 230, 168),
	violet = Color3.fromRGB(168, 145, 255),
}
Module.FONT_UI = Enum.Font.Regular
Module.FONT_MONO = Enum.Font.Code
Module.FONT_DISPLAY = Enum.Font.Bold

function Module.new(className: string, properties: { [string]: any }): Instance
	return Fusion.New(className)(properties)
end

function Module.frame(name: string, properties: { [string]: any }): Frame
	properties.Name = name
	properties.BackgroundColor3 = properties.BackgroundColor3 or Module.PALETTE.bg1
	properties.BorderSizePixel = 0
	return Module.new("Frame", properties) :: Frame
end

function Module.label(name: string, properties: { [string]: any }): TextLabel
	properties.Name = name
	properties.BackgroundTransparency = properties.BackgroundTransparency or 1
	properties.Font = properties.Font or Module.FONT_UI
	properties.TextColor3 = properties.TextColor3 or Module.PALETTE.ink
	properties.TextSize = properties.TextSize or 13
	properties.TextXAlignment = properties.TextXAlignment or Enum.TextXAlignment.Left
	properties.TextYAlignment = properties.TextYAlignment or Enum.TextYAlignment.Center
	return Module.new("TextLabel", properties) :: TextLabel
end

function Module.button(name: string, properties: { [string]: any }): TextButton
	properties.Name = name
	properties.BackgroundColor3 = properties.BackgroundColor3 or Module.PALETTE.bg1
	properties.BorderSizePixel = 0
	properties.Font = properties.Font or Module.FONT_DISPLAY
	properties.TextColor3 = properties.TextColor3 or Module.PALETTE.ink
	properties.TextSize = properties.TextSize or 13
	return Module.new("TextButton", properties) :: TextButton
end

function Module.stroke(colour: Color3?, transparency: number?, thickness: number?): UIStroke
	return Module.new("UIStroke", { Color = colour or Module.PALETTE.accent, Transparency = transparency or 0, Thickness = thickness or 1 }) :: UIStroke
end

function Module.corner(radius: number): UICorner
	return Module.new("UICorner", { CornerRadius = UDim.new(0, radius) }) :: UICorner
end

function Module.smoothBar(name: string, width: number, value: number, maximum: number, colour: Color3): Frame
	return Module.frame(name, {
		Size = UDim2.new(0, width, 0, 5), BackgroundColor3 = Module.PALETTE.bg0,
		[Fusion.Children] = {
			Module.stroke(colour, 0.32, 1),
			Module.frame("Fill", { Size = UDim2.new(math.clamp(value / maximum, 0, 1), 0, 1, 0), BackgroundColor3 = colour }),
		},
	})
end

return Module
