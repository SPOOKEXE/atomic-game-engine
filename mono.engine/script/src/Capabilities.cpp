#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/script/Capabilities.hpp>

namespace engine::script {

	bool IsGuiInputSignal(SignalKind kind) {
		// The GUI input signals are one contiguous run in `SignalKind`.
		return kind >= SignalKind::GuiActivated && kind <= SignalKind::GuiFocusLost;
	}

	bool
	HasCapability(const ecs::Store &store, const HostRole &role, Capability capability, ecs::Entity subject) {
		switch (capability) {
		case Capability::GuiInput: {
			// Only a server-only chunk is refused. A client, a host that is
			// both, and an edit-time plugin (neither) can all see a screen.
			if (!role.Server || role.Client) return true;
			// A plugin's own widgets are input it owns, whatever role it runs as.
			const ecs::ClassId pluginGui = ecs::Classes::Find(core::Name("PluginGui"));
			return pluginGui.IsValid() && store.Alive(subject) &&
				   (store.IsA(subject, pluginGui) ||
					store.FindFirstAncestorWhichIsA(subject, pluginGui) != ecs::NULL_ENTITY);
		}
		}
		return true;
	}

	std::string SignalCapabilityWarning(
		const ecs::Store &store, const HostRole &role, SignalKind kind, ecs::Entity subject
	) {
		if (!IsGuiInputSignal(kind) || HasCapability(store, role, Capability::GuiInput, subject)) return {};
		const std::string where =
			store.Alive(subject) ? store.GetFullName(subject) : std::string("a GUI object");
		return "a server Script connected a GUI input signal on " + where +
			   ". Player GUI input only reaches the client: handle it in a LocalScript and send it to the "
			   "server with a RemoteEvent";
	}
}
