#pragma once

#include <string>
#include <string_view>

namespace studio {

	// Whether a relative path stays inside the folder it is relative to,
	// checked lexically. Used for a manifest's `main` and `plugin.SaveFolder`.
	bool StaysInside(const std::string &relative);

	// Script names implemented directly by the Studio host surface. Dynamic
	// native bindings cannot use these names in Studio because the fixed host
	// dispatch owns them.
	inline constexpr std::string_view STUDIO_PLUGIN_HOST_NAMES[] = {
		"Notify",
		"GetActiveWorld",
		"Selection.Get",
		"Selection.Set",
		"Selection.Add",
		"Selection.Remove",
		"Selection.SelectionChanged",
		"RenderPipelineService.GetProfiles",
		"RenderPipelineService.GetNodePositions",
		"RenderPipelineService.WhatIsLost",
		"RenderPipelineService.GetNodes",
		"RenderPipelineService.GetNodeKinds",
		"RenderPipelineService.AddNode",
		"RenderPipelineService.MoveNode",
		"RenderPipelineService.SetNodeEnabled",
		"RenderPipelineService.ConnectPorts",
		"RenderPipelineService.DisconnectPort",
		"ChangeHistoryService.TryBeginRecording",
		"ChangeHistoryService.FinishRecording",
		"ChangeHistoryService.IsRecordingInProgress",
		"ChangeHistoryService.GetCanUndo",
		"ChangeHistoryService.GetCanRedo",
		"ChangeHistoryService.Undo",
		"ChangeHistoryService.Redo",
		"ChangeHistoryService.SetWaypoint",
		"ChangeHistoryService.ResetWaypoints",
		"ChangeHistoryService.SetEnabled",
		"ChangeHistoryService.OnUndo",
		"ChangeHistoryService.OnRedo",
		"ChangeHistoryService.OnRecordingStarted",
		"ChangeHistoryService.OnRecordingFinished",
		"GetScriptSource",
		"SetScriptSource",
		"GetScripts",
		"CreateToolbar",
		"CreateToolbarTab",
		"CreateToolbarRow",
		"CreateToolbarColumn",
		"CreateButton",
		"CreateToggle",
		"CreateDropdown",
		"CreateLabel",
		"SetToolCell",
		"SetButtonActive",
		"SetToolVisible",
		"SetToolWidth",
		"SetToolbarVisible",
		"SetToolbarPlacement",
		"CreateWidget",
		"GetWidgetGui",
		"SetWidgetRender",
		"SetWidgetOpen",
		"IsWidgetOpen",
		"SetWidgetColour",
		"SetWidgetDock",
		"SetWidgetSizeConstraints",
		"GetViewportOption",
		"SetViewportOption",
		"AddViewport",
		"OpenScript",
		"Label",
		"Button",
		"Checkbox",
		"Combo",
		"Separator",
		"InputText",
		"InputTextMultiline",
		"SaveFile",
		"OpenFile",
		"SaveFolder",
		"ReadFile",
		"SetClipboard",
		"GetClipboard",
	};

	// Roblox's plugin object model, answered by the same surface as members of
	// the instances under `PluginGuiService`. See `script::HostSurface` for the
	// `@Class:Method`, `@Class.Property` and `@Class!Event` spellings.
	inline constexpr std::string_view STUDIO_PLUGIN_MEMBER_NAMES[] = {
		"DockWidgetPluginGuiInfo.new",
		"@Plugin:CreateToolbar",
		"@Plugin:CreatePluginAction",
		"@Plugin:CreateDockWidgetPluginGui",
		"@Plugin:CreateDockWidgetPluginGuiAsync",
		"@Plugin:GetMouse",
		"@Plugin:GetSetting",
		"@Plugin:SetSetting",
		"@Plugin:Activate",
		"@Plugin:Deactivate",
		"@Plugin:IsActivated",
		"@Plugin:IsActivatedWithExclusiveMouse",
		"@Plugin:GetSelectedRibbonTool",
		"@Plugin:SelectRibbonTool",
		"@Plugin:OpenScript",
		"@Plugin:CreatePluginMenu",
		"@Plugin:SaveFile",
		"@Plugin:OpenFile",
		"@Plugin:SaveFolder",
		"@Plugin:ReadFile",
		"@Plugin:SetClipboard",
		"@Plugin:GetClipboard",
		"@Plugin.GridSize",
		"@Plugin.CollisionEnabled",
		"@Plugin.IsDebuggable",
		"@Plugin!Deactivation",
		"@Plugin!Unloading",
		"@PluginToolbar:CreateButton",
		"@PluginToolbarButton:SetActive",
		"@PluginToolbarButton.Enabled",
		"@PluginToolbarButton.Icon",
		"@PluginToolbarButton.ClickableWhenViewportHidden",
		"@PluginToolbarButton!Click",
		"@PluginAction.ActionId",
		"@PluginAction.Text",
		"@PluginAction.StatusTip",
		"@PluginAction.AllowBinding",
		"@PluginAction!Triggered",
		"@PluginMenu.Title",
		"@PluginMenu.Icon",
		"@PluginMenu:AddAction",
		"@PluginMenu:AddNewAction",
		"@PluginMenu:AddMenu",
		"@PluginMenu:AddSeparator",
		"@PluginMenu:Clear",
		"@PluginMenu:ShowAsync",
		"@PluginMouse.X",
		"@PluginMouse.Y",
		"@PluginMouse.ViewSizeX",
		"@PluginMouse.ViewSizeY",
		"@PluginMouse.Hit",
		"@PluginMouse.Origin",
		"@PluginMouse.Target",
		"@PluginMouse!Button1Down",
		"@PluginMouse!Button1Up",
		"@PluginMouse!Move",
		"@DockWidgetPluginGui.Title",
		"@DockWidgetPluginGui:BindToClose",
		"@DockWidgetPluginGui:SetImageGraph",
		"@DockWidgetPluginGui:GetImageGraph",
		"@DockWidgetPluginGui!ImageGraphChanged",
	};

	constexpr bool IsStudioPluginHostName(std::string_view name) {
		for (const std::string_view reserved : STUDIO_PLUGIN_MEMBER_NAMES) {
			if (reserved == name) {
				return true;
			}
		}
		for (const std::string_view reserved : STUDIO_PLUGIN_HOST_NAMES) {
			if (reserved == name) {
				return true;
			}
		}
		return false;
	}

}
