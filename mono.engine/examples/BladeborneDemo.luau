--!strict
-- Bladeborne HUD bootstrap. UI ownership stays in the same widget modules as
-- BLADEBORNE_UNIFIED/game/src; this scene only supplies local player fixtures.

local Modules = script :: any
local Fusion = require(Modules.BladeborneDemo.Fusion)
local Data = require(Modules.BladeborneDemo.DemoData)
local Common = require(Modules.BladeborneDemo.HUDCommon)
local OnscreenWidget = require(Modules.BladeborneDemo.Widgets.OnscreenWidget)
local OnscreenButtons = require(Modules.BladeborneDemo.Widgets.OnscreenButtons)
local MinimapWidget = require(Modules.BladeborneDemo.Widgets.MinimapWidget)
local TopRightQuestsWidget = require(Modules.BladeborneDemo.Widgets.TopRightQuestsWidget)
local HotbarWidget = require(Modules.BladeborneDemo.Widgets.HotbarWidget)
local MusicWidget = require(Modules.BladeborneDemo.Widgets.MusicWidget)
local TabWidget = require(Modules.BladeborneDemo.Widgets.TabWidget)
local StarterGui = game:GetService("StarterGui")

local gui = Common.new("ScreenGui", { Name = "BladeborneHUD", ResetOnSpawn = false }) :: ScreenGui
gui.Parent = StarterGui

local world = Common.frame("WorldBackground", { Size = UDim2.new(1, 0, 1, 0), BackgroundColor3 = Color3.fromRGB(42, 49, 52), ZIndex = 0 })
world.Parent = gui
local ground = Common.frame("Ground", { Position = UDim2.new(0, 0, 0.42, 0), Size = UDim2.new(1, 0, 0.58, 0), BackgroundColor3 = Color3.fromRGB(67, 75, 77), ZIndex = 0 })
ground.Parent = world
for index = 1, 17 do
	Common.frame("GridH" .. tostring(index), { Position = UDim2.new(0, 0, index * 0.06, 0), Size = UDim2.new(1, 0, 0, 1), BackgroundColor3 = Color3.fromRGB(42, 48, 51), BackgroundTransparency = 0.25 }).Parent = ground
end
for index = 1, 28 do
	Common.frame("GridV" .. tostring(index), { Position = UDim2.new(index / 28, 0, 0, 0), Size = UDim2.new(0, 1, 1, 0), BackgroundColor3 = Color3.fromRGB(42, 48, 51), BackgroundTransparency = 0.25 }).Parent = ground
end

local context = { Gui = gui, Data = Data, Fusion = Fusion, HUDCommon = Common }
local tabWidget = TabWidget.Build(context)
OnscreenWidget.OpenWidget(context)
OnscreenButtons.OpenWidget(context, tabWidget)
MinimapWidget.OpenWidget(context)
TopRightQuestsWidget.OpenWidget(context)
HotbarWidget.OpenWidget(context)
MusicWidget.OpenWidget(context)
