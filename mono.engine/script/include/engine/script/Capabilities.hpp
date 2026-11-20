#pragma once

// What a running script may do, decided by the kind of script it is.
//
// One check site per capability, so a rule about who may do what lives here
// and not as an `if` beside each method that cares. A caller asks
// `CapabilityWarning` with the running chunk's role; an empty answer means
// the call is fine.
//
// Current rules:
//
// - `GuiInput`: player GUI input (clicks, hover, focus, drags) only ever
//   reaches the client that owns the screen. A game `Script` runs as the
//   server, so a GUI input signal it connects on game UI never fires in a
//   real server. It is told to handle the input in a `LocalScript` and send
//   it to the server with a `RemoteEvent`. A `LocalScript` holds it, and so
//   does a plugin on its own widgets (anything under a `PluginGui`).

#include <engine/ecs/Entity.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/Signals.hpp>

#include <string>

namespace engine::ecs {
	class Store;
}

namespace engine::script {

	// A thing a script may or may not be allowed to do.
	enum class Capability : uint8_t {
		// Receives player GUI input signals on game UI.
		GuiInput,
	};

	// Whether `kind` is fired by player GUI input.
	bool IsGuiInputSignal(SignalKind kind);

	// Whether a chunk running as `role` holds `capability` over `subject`.
	bool
	HasCapability(const ecs::Store &store, const HostRole &role, Capability capability, ecs::Entity subject);

	// The warning for connecting `kind` on `subject` from a chunk running as
	// `role`, or an empty string when that is allowed.
	std::string SignalCapabilityWarning(
		const ecs::Store &store, const HostRole &role, SignalKind kind, ecs::Entity subject
	);
}
