--!strict
-- Port of TabWidget and TabWidget/CharacterTab. Other rail choices retain the
-- shared shell while Character builds the data-backed attributes and equipment.

local Common = require((script :: any).Parent.Parent.HUDCommon)
local Fusion = require((script :: any).Parent.Parent.Fusion)
local Module = {}

local function statCard(name: string, value: string, bonus: string, x: number, y: number): Frame
	return Common.frame("Attribute" .. name, { Position = UDim2.new(0, x, 0, y), Size = UDim2.new(0, 130, 0, 82), BackgroundColor3 = Common.PALETTE.bg0, BackgroundTransparency = 0.13, [Fusion.Children] = { Common.stroke(Common.PALETTE.accent, 0.45, 1), Common.label("Tag", { Position = UDim2.new(0, 12, 0, 9), Size = UDim2.new(0, 60, 0, 15), Text = name, Font = Common.FONT_MONO, TextSize = 12 }), Common.label("Bonus", { AnchorPoint = Vector2.new(1, 0), Position = UDim2.new(1, -10, 0, 9), Size = UDim2.new(0, 32, 0, 15), Text = bonus, TextColor3 = Common.PALETTE.success, Font = Common.FONT_MONO, TextSize = 12, TextXAlignment = Enum.TextXAlignment.Right }), Common.label("Value", { Position = UDim2.new(0, 14, 0, 31), Size = UDim2.new(0, 80, 0, 36), Text = value, Font = Common.FONT_DISPLAY, TextSize = 34 }) } })
end

function Module.Build(ctx: any)
	local overlay = Common.frame("TabWidget", { Size = UDim2.new(1, 0, 1, 0), BackgroundColor3 = Common.PALETTE.bg0, BackgroundTransparency = 0.28, Visible = false, ZIndex = 20 })
	overlay.Parent = ctx.Gui
	local panel = Common.frame("CharacterTab", { AnchorPoint = Vector2.new(0.5, 0), Position = UDim2.new(0.48, 0, 0, 88), Size = UDim2.new(0, 754, 0, 476), BackgroundColor3 = Common.PALETTE.bg0, BackgroundTransparency = 0.08, ZIndex = 21, [Fusion.Children] = { Common.stroke(Common.PALETTE.accent, 0.2, 1) } })
	panel.Parent = overlay
	for index, tab in { "CHARACTER", "EQUIP", "ATTRIBUTES", "INVENTORY", "QUESTS", "SKILLS", "CRAFTING", "FORGE", "ACHIEVEMENTS", "BOOSTERS", "LEADERBOARD", "SOCIAL", "SETTINGS" } do
		Common.label("Rail" .. tostring(index), { Position = UDim2.new(0, 18, 0, 72 + (index - 1) * 29), Size = UDim2.new(0, 200, 0, 22), Text = "+  " .. tab, Font = Common.FONT_MONO, TextSize = 10, ZIndex = 21 }).Parent = overlay
	end
	local p = ctx.Data.profile
	Common.frame("IdentityCard", { Position = UDim2.new(0, 12, 0, 12), Size = UDim2.new(0, 324, 0, 86), BackgroundColor3 = Common.PALETTE.bg0, BackgroundTransparency = 0.14, ZIndex = 22, [Fusion.Children] = { Common.stroke(Common.PALETTE.accent, 0.42, 1), Common.label("Name", { Position = UDim2.new(0, 12, 0, 10), Size = UDim2.new(1, -24, 0, 24), Text = p.name, Font = Common.FONT_DISPLAY, TextSize = 20, ZIndex = 22 }), Common.label("Class", { Position = UDim2.new(0, 12, 0, 38), Size = UDim2.new(1, -24, 0, 16), Text = "BEATER / LV " .. tostring(p.level), TextColor3 = Common.PALETTE.inkDim, Font = Common.FONT_MONO, TextSize = 10, ZIndex = 22 }), Common.label("Mastery", { Position = UDim2.new(0, 12, 0, 58), Size = UDim2.new(1, -24, 0, 16), Text = "Class mastery                         Rank S / 94%", Font = Common.FONT_MONO, TextSize = 10, ZIndex = 22 }) } }).Parent = panel
	for index, spec in { { "STR", "5", "+4" }, { "DEX", "0", "+2" }, { "AGI", "3", "+0" }, { "VIT", "0", "+0" }, { "INT", "0", "+0" }, { "LUK", "0", "+0" } } do
		statCard(spec[1], spec[2], spec[3], 340 + ((index - 1) % 3) * 138, 18 + math.floor((index - 1) / 3) * 92).Parent = panel
	end
	for index, item in { "L HAND  Wooden Sword", "R HAND  Wooden Sword", "HEAD  -", "CHEST  -", "LEGS  -", "BOOTS  -" } do
		local column = (index - 1) % 2
		local row = math.floor((index - 1) / 2)
		Common.frame("Equipment" .. tostring(index), { Position = UDim2.new(0, 340 + column * 208, 0, 208 + row * 80), Size = UDim2.new(0, 198, 0, 72), BackgroundColor3 = Common.PALETTE.bg0, BackgroundTransparency = 0.13, ZIndex = 22, [Fusion.Children] = { Common.stroke(Common.PALETTE.accent, 0.35, 1), Common.label("Item", { Position = UDim2.new(0, 70, 0, 15), Size = UDim2.new(0, 118, 0, 34), Text = item, Font = Common.FONT_MONO, TextSize = 11, TextWrapped = true, ZIndex = 23 }) } }).Parent = panel
	end
	local close = Common.button("CloseButton", { AnchorPoint = Vector2.new(1, 0), Position = UDim2.new(1, -18, 0, 16), Size = UDim2.new(0, 105, 0, 20), Text = "ESC - CLOSE", BackgroundTransparency = 1, Font = Common.FONT_MONO, TextSize = 11, ZIndex = 22 })
	close.Parent = overlay
	close.Activated:Connect(function() overlay.Visible = false end)
	return {
		OpenTargetTabByName = function(_name: string) overlay.Visible = true end,
		OpenWidget = function() overlay.Visible = true end,
	}
end

return Module
