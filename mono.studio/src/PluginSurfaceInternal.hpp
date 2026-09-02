#pragma once

#include <string_view>

namespace studio {

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
	};

	constexpr bool IsStudioPluginHostName(std::string_view name) {
		for (const std::string_view reserved : STUDIO_PLUGIN_HOST_NAMES) {
			if (reserved == name) {
				return true;
			}
		}
		return false;
	}

}
