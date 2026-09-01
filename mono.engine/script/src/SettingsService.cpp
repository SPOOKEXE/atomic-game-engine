// The neutral script surface for extending a client's ESC settings menu.
// @tier L9 · shared

#include <engine/gui/SettingsMenu.hpp>
#include <engine/script/ScriptCall.hpp>
#include <engine/script/ServiceSurface.hpp>

#include <array>
#include <string>

namespace engine::script {
	namespace {
		void RequireClient(ScriptCall &call) {
			if (!call.Role().Client) {
				call.Raise("SettingsService is available only to client scripts");
			}
		}

		void SetMenuAction(ScriptCall &call) {
			RequireClient(call);
			const core::Name id(call.AsString(0));
			const std::string label = call.AsString(1);
			if (!gui::ReachSettingsMenuExtensions(call.World()).Set(id, label)) {
				call.Raise(
					"SetMenuAction needs a non-empty name and a label of at most 64 bytes, with room in the "
					"12-action menu limit"
				);
			}
			call.ReturnSignal(SignalKind::SettingsMenuAction, id);
		}

		void RemoveMenuAction(ScriptCall &call) {
			RequireClient(call);
			call.ReturnBoolean(
				gui::ReachSettingsMenuExtensions(call.World()).Remove(core::Name(call.AsString(0)))
			);
		}

		void ClearMenuActions(ScriptCall &call) {
			RequireClient(call);
			gui::ReachSettingsMenuExtensions(call.World()).Clear();
		}

		constexpr std::array<ServiceMethod, 3> METHODS{{
			{"SetMenuAction", SetMenuAction},
			{"RemoveMenuAction", RemoveMenuAction},
			{"ClearMenuActions", ClearMenuActions},
		}};
	}

	const ServiceSurface &SettingsServiceSurface() {
		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "SettingsService";
			surface.Methods = METHODS;
			return surface;
		}();
		return SURFACE;
	}
}
