--!strict
-- Port of MusicWidget's now-playing pill. Audio playback stays outside this UI demo.

local Common = require((script :: any).Parent.Parent.HUDCommon)
local Fusion = require((script :: any).Parent.Parent.Fusion)
local Module = {}

function Module.OpenWidget(ctx: any)
	Common.frame("MusicPill", { AnchorPoint = Vector2.new(1, 1), Position = UDim2.new(1, -22, 1, -16), Size = UDim2.new(0, 190, 0, 50), BackgroundColor3 = Common.PALETTE.bg0, BackgroundTransparency = 0.12, [Fusion.Children] = { Common.stroke(Common.PALETTE.violet, 0.55, 1), Common.label("SongId", { Position = UDim2.new(0, 10, 0, 7), Size = UDim2.new(1, -20, 0, 11), Text = "NOW PLAYING 12857315259747", TextColor3 = Common.PALETTE.inkDim, Font = Common.FONT_MONO, TextSize = 8 }), Common.label("SongTitle", { Position = UDim2.new(0, 10, 0, 20), Size = UDim2.new(1, -20, 0, 16), Text = "Cozy Tavern Fireplace", TextSize = 12 }), Common.label("Duration", { AnchorPoint = Vector2.new(1, 1), Position = UDim2.new(1, -10, 1, -5), Size = UDim2.new(0, 76, 0, 11), Text = "00:00 / 02:56", TextColor3 = Common.PALETTE.inkDim, Font = Common.FONT_MONO, TextSize = 8, TextXAlignment = Enum.TextXAlignment.Right }) } }).Parent = ctx.Gui
end

return Module
