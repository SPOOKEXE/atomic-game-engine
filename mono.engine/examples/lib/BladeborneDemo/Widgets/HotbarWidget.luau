--!strict
-- Port of HotbarWidget's 24 slots and Q/W/R plus D/F/G ability clusters.

local Common = require((script :: any).Parent.Parent.HUDCommon)
local Fusion = require((script :: any).Parent.Parent.Fusion)
local Module = {}

local function buildSlot(index: number, row: number, item: any): Frame
	return Common.frame("HotbarSlot" .. tostring(index), { Position = UDim2.new(0, (index - 1) * 44, 0, (row - 1) * 47), Size = UDim2.new(0, 44, 0, 46), BackgroundColor3 = Common.PALETTE.bg0, BackgroundTransparency = 0.04, [Fusion.Children] = { Common.stroke(if item.active then Common.PALETTE.gold else Common.PALETTE.accent, if item.active then 0 else 0.48, 1), Common.label("Keybind", { Position = UDim2.new(0, 4, 0, 2), Size = UDim2.new(0, 16, 0, 11), Text = item.key, TextColor3 = Common.PALETTE.inkDim, Font = Common.FONT_MONO, TextSize = 9 }), Common.label("Glyph", { Size = UDim2.new(1, 0, 1, 0), Text = item.glyph, TextColor3 = Common.PALETTE.gold, Font = Common.FONT_DISPLAY, TextSize = 24, TextXAlignment = Enum.TextXAlignment.Center }) } })
end

function Module.OpenWidget(ctx: any)
	local hotbar = Common.frame("Hotbar", { AnchorPoint = Vector2.new(0.5, 1), Position = UDim2.new(0.5, 0, 1, -14), Size = UDim2.new(0, 528, 0, 94), BackgroundTransparency = 1 })
	for index, item in ctx.Data.hotbar.primary do buildSlot(index, 1, item).Parent = hotbar end
	for index, item in ctx.Data.hotbar.secondary do buildSlot(index, 2, item).Parent = hotbar end
	hotbar.Parent = ctx.Gui
	local buttons: { TextButton } = {}
	for index, spec in ctx.Data.profile.abilities do
		local offset = if index <= 3 then -390 + (index - 1) * 64 else 286 + (index - 4) * 64
		local ability = Common.button("Ability" .. tostring(index), { AnchorPoint = Vector2.new(0.5, 1), Position = UDim2.new(0.5, offset, 1, if index <= 3 then -76 else -68), Size = UDim2.new(0, 56, 0, 56), Text = spec.glyph, TextColor3 = if spec.active then Common.PALETTE.gold else Common.PALETTE.ink, TextSize = 27, BackgroundColor3 = Common.PALETTE.bg0, [Fusion.Children] = { Common.corner(32), Common.stroke(Common.PALETTE.accent, 0.08, 2), Common.label("KeyChip", { AnchorPoint = Vector2.new(0.5, 1), Position = UDim2.new(0.5, 0, 1, 7), Size = UDim2.new(0, 22, 0, 13), Text = spec.key, BackgroundColor3 = Common.PALETTE.bg0, BackgroundTransparency = 0, Font = Common.FONT_MONO, TextSize = 10, TextXAlignment = Enum.TextXAlignment.Center }) } })
		table.insert(buttons, ability)
		ability.Activated:Connect(function()
			for otherIndex, other in buttons do other.TextColor3 = if otherIndex == index then Common.PALETTE.gold else Common.PALETTE.ink end
		end)
		ability.Parent = ctx.Gui
	end
end

return Module
