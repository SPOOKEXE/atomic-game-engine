--!strict
-- Port of MinimapWidget's square panel, rings and local marker projection.

local Common = require((script :: any).Parent.Parent.HUDCommon)
local Fusion = require((script :: any).Parent.Parent.Fusion)
local Module = {}

function Module.OpenWidget(ctx: any)
	local palette = Common.PALETTE
	local children: { Instance } = { Common.stroke(palette.accent, 0.42, 1) }
	for index, diameter in { 52, 94, 136, 178 } do
		children[#children + 1] = Common.frame("Ring" .. tostring(index), { AnchorPoint = Vector2.new(0.5, 0.5), Position = UDim2.new(0.60, 0, 0.58, 0), Size = UDim2.new(0, diameter, 0, diameter), BackgroundTransparency = 1, [Fusion.Children] = { Common.corner(999), Common.stroke(palette.accent, 0.82, 1) } })
	end
	children[#children + 1] = Common.frame("MapPlayer", { AnchorPoint = Vector2.new(0.5, 0.5), Position = UDim2.new(0.60, 0, 0.58, 0), Size = UDim2.new(0, 13, 0, 13), BackgroundColor3 = Color3.fromRGB(224, 205, 104), [Fusion.Children] = { Common.corner(12), Common.stroke(palette.bg0, 0, 2) } })
	for index, point in { { 0.74, 0.47 }, { 0.80, 0.55 }, { 0.69, 0.65 } } do
		children[#children + 1] = Common.frame("Marker" .. tostring(index), { Position = UDim2.new(point[1], 0, point[2], 0), Size = UDim2.new(0, 7, 0, 7), BackgroundColor3 = palette.danger, [Fusion.Children] = { Common.corner(9) } })
	end
	children[#children + 1] = Common.label("ZoneLabel", { Position = UDim2.new(0, 8, 0, 6), Size = UDim2.new(0, 116, 0, 14), Text = "FL Floor" .. tostring(ctx.Data.profile.floor), Font = Common.FONT_MONO, TextSize = 10 })
	children[#children + 1] = Common.label("ZoomText", { AnchorPoint = Vector2.new(0, 1), Position = UDim2.new(0, 8, 1, -5), Size = UDim2.new(0, 36, 0, 12), Text = "x1.0", TextColor3 = palette.inkDim, Font = Common.FONT_MONO, TextSize = 9 })
	children[#children + 1] = Common.label("ClockText", { AnchorPoint = Vector2.new(1, 1), Position = UDim2.new(1, -8, 1, -5), Size = UDim2.new(0, 40, 0, 12), Text = ctx.Data.profile.time, TextColor3 = palette.inkDim, Font = Common.FONT_MONO, TextSize = 9, TextXAlignment = Enum.TextXAlignment.Right })
	Common.frame("Minimap", { AnchorPoint = Vector2.new(1, 0), Position = UDim2.new(1, -22, 0, 30), Size = UDim2.new(0, 200, 0, 170), BackgroundColor3 = palette.bg0, BackgroundTransparency = 0.22, ClipsDescendants = true, [Fusion.Children] = children }).Parent = ctx.Gui
end

return Module
