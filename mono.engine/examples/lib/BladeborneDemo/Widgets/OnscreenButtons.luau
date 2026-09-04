--!strict
-- Port of OnscreenButtons' left-hand quick-tab cluster.

local Common = require((script :: any).Parent.Parent.HUDCommon)
local Fusion = require((script :: any).Parent.Parent.Fusion)
local Module = {}

function Module.OpenWidget(ctx: any, tabWidget: any)
	local root = Common.frame("LeftHexButtons", { Position = UDim2.new(0, 20, 0.5, -62), Size = UDim2.new(0, 50, 0, 220), BackgroundTransparency = 1 })
	for index, spec in { { "Character", "C", "STATS" }, { "Inventory", "I", "BAG" }, { "Skills", "K", "SKILL" }, { "Social", "S", "PARTY" } } do
		local quick = Common.button(spec[1], { Position = UDim2.new(0, 0, 0, (index - 1) * 56), Size = UDim2.new(0, 46, 0, 44), Text = spec[2], TextSize = 18, BackgroundColor3 = Common.PALETTE.bg0, [Fusion.Children] = { Common.corner(5), Common.stroke(Common.PALETTE.accent, 0.4, 1), Common.label("KeybindLabel", { AnchorPoint = Vector2.new(0.5, 0), Position = UDim2.new(0.5, 0, 1, 2), Size = UDim2.new(0, 54, 0, 11), Text = spec[3], TextColor3 = Common.PALETTE.inkDim, Font = Common.FONT_MONO, TextSize = 7, TextXAlignment = Enum.TextXAlignment.Center }) } })
		quick.Activated:Connect(function() tabWidget.OpenTargetTabByName(spec[1]) end)
		quick.Parent = root
	end
	root.Parent = ctx.Gui
end

return Module
