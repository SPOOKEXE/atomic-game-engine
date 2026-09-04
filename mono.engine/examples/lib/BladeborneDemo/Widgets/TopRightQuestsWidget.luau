--!strict
-- Port of TopRightQuestsWidget's tracked quest card using fixture ActiveQuests.

local Common = require((script :: any).Parent.Parent.HUDCommon)
local Fusion = require((script :: any).Parent.Parent.Fusion)
local Module = {}

function Module.OpenWidget(ctx: any)
	local quest = ctx.Data.profile.quest
	local palette = Common.PALETTE
	Common.frame("TopRightQuestsRail", {
		AnchorPoint = Vector2.new(1, 0), Position = UDim2.new(1, -22, 0, 204), Size = UDim2.new(0, 202, 0, 132), BackgroundTransparency = 1,
		[Fusion.Children] = {
			Common.frame("QuestCard", { Size = UDim2.new(1, 0, 1, 0), BackgroundColor3 = palette.bg0, BackgroundTransparency = 0.2, [Fusion.Children] = {
				Common.stroke(palette.accent, 0.45, 1),
				Common.label("Kicker", { Position = UDim2.new(0, 10, 0, 8), Size = UDim2.new(1, -20, 0, 13), Text = "TRACKED QUEST", Font = Common.FONT_MONO, TextSize = 10 }),
				Common.label("QuestName", { Position = UDim2.new(0, 10, 0, 27), Size = UDim2.new(1, -20, 0, 19), Text = quest.name, TextSize = 14, Font = Common.FONT_DISPLAY }),
				Common.label("QuestBody", { Position = UDim2.new(0, 10, 0, 52), Size = UDim2.new(1, -20, 0, 40), Text = quest.body, TextColor3 = palette.inkDim, TextSize = 10, TextWrapped = true, TextYAlignment = Enum.TextYAlignment.Top }),
				Common.label("QuestObjective", { Position = UDim2.new(0, 10, 0, 101), Size = UDim2.new(1, -20, 0, 22), Text = quest.objective .. "      " .. quest.distance, Font = Common.FONT_MONO, TextSize = 10, TextWrapped = true }),
			} }),
		},
	}).Parent = ctx.Gui
end

return Module
