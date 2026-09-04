--!strict
-- Port of game/src/.../Widgets/OnscreenWidget.luau with the replicated player
-- data boundary replaced by the demo fixture.

local Common = require((script :: any).Parent.Parent.HUDCommon)
local Fusion = require((script :: any).Parent.Parent.Fusion)

local Module = {}

function Module.OpenWidget(ctx: any)
	local p = ctx.Data.profile
	local palette = Common.PALETTE
	local root = Common.frame("OnscreenVitals", {
		Position = UDim2.new(0, 18, 0, 68), Size = UDim2.new(0, 478, 0, 112), BackgroundTransparency = 1,
		[Fusion.Children] = {
			Common.frame("Sigil", { Size = UDim2.new(0, 80, 0, 80), BackgroundColor3 = Color3.fromRGB(244, 194, 56), [Fusion.Children] = {
				Common.stroke(palette.accent, 0, 1),
				Common.label("PortraitInitial", { Size = UDim2.new(1, 0, 1, 0), Text = p.initial, TextColor3 = palette.bg0, TextSize = 38, Font = Common.FONT_DISPLAY, TextXAlignment = Enum.TextXAlignment.Center }),
				Common.frame("LevelBadge", { AnchorPoint = Vector2.new(1, 1), Position = UDim2.new(1, 0, 1, 0), Size = UDim2.new(0, 48, 0, 22), BackgroundColor3 = palette.bg0, [Fusion.Children] = { Common.stroke(palette.accent, 0, 1), Common.label("Level", { Size = UDim2.new(1, 0, 1, 0), Text = "LVL " .. tostring(p.level), Font = Common.FONT_MONO, TextSize = 11, TextXAlignment = Enum.TextXAlignment.Center }) } }),
			} }),
			Common.label("PlayerName", { Position = UDim2.new(0, 93, 0, 0), Size = UDim2.new(0, 210, 0, 24), Text = string.upper(p.name), TextSize = 22, Font = Common.FONT_DISPLAY, TextStrokeColor3 = palette.bg0, TextStrokeTransparency = 0.25 }),
			Common.label("Floor", { Position = UDim2.new(0, 94, 0, 23), Size = UDim2.new(0, 240, 0, 13), Text = "FL " .. tostring(p.floor) .. " / " .. p.zone, TextColor3 = palette.inkDim, TextSize = 10, Font = Common.FONT_MONO }),
			Common.label("HPLabel", { Position = UDim2.new(0, 94, 0, 38), Size = UDim2.new(0, 30, 0, 14), Text = "HP", TextColor3 = palette.danger, TextSize = 10, Font = Common.FONT_MONO }),
			Common.label("HPValue", { Position = UDim2.new(0, 425, 0, 38), Size = UDim2.new(0, 52, 0, 14), Text = tostring(p.health) .. "/" .. tostring(p.maxHealth), TextSize = 11, Font = Common.FONT_MONO, TextXAlignment = Enum.TextXAlignment.Right }),
			Common.label("XPLabel", { Position = UDim2.new(0, 94, 0, 56), Size = UDim2.new(0, 30, 0, 14), Text = "XP", TextColor3 = palette.gold, TextSize = 10, Font = Common.FONT_MONO }),
			Common.label("XPValue", { Position = UDim2.new(0, 425, 0, 56), Size = UDim2.new(0, 52, 0, 14), Text = string.format("%.1f%%", p.experience / p.maxExperience * 100), TextSize = 11, Font = Common.FONT_MONO, TextXAlignment = Enum.TextXAlignment.Right }),
		},
	})
	local hp = Common.smoothBar("HPBar", 315, p.health, p.maxHealth, palette.danger)
	hp.Position = UDim2.new(0, 108, 0, 43); hp.Parent = root
	local xp = Common.smoothBar("XPBar", 315, p.experience, p.maxExperience, palette.gold)
	xp.Position = UDim2.new(0, 108, 0, 61); xp.Parent = root
	root.Parent = ctx.Gui
	Common.label("OnscreenCurrency", { Position = UDim2.new(0, 20, 0, 184), Size = UDim2.new(0, 120, 0, 18), Text = "COL  " .. tostring(p.currency), TextColor3 = palette.gold, TextSize = 13, Font = Common.FONT_MONO }).Parent = ctx.Gui
end

return Module
