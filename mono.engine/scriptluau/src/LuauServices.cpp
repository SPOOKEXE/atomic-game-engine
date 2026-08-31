// The catalogue, walked into a Luau state.
//
// **The half of `script::ServiceCatalogue` that has met the VM.** That table is
// data - a name, an availability, a language mask and one `ServiceSurface`
// accessor per row - and this file is the one that turns a row into a global. Its
// twin is `scriptjs/src/JsServices.cpp`, and the two exist so that "which
// services exist" is one list rather than one per language, which it was until
// v0.15: Luau bound nine and JavaScript five, four were reachable from one
// language only, and nothing in the build said so.
//
// @tier L10 · shared

#include "LuauBindings.hpp"

#include <engine/script/ServiceCatalogue.hpp>
#include <engine/script/ServiceSurface.hpp>
#include <engine/world/Postbox.hpp>

#include <cstring>

namespace engine::script {

	void InstallLuauServices(lua_State *state, ServiceAvailability phase, ScriptCapabilities access) {
		// **The mailbox types, before the services that need them, and this is
		// not a formality.** A `Postbox` is a view over two resources, and
		// reading one on a store that never registered them mints them under the
		// *compiler's* spelling - `engine::world::Inbox` rather than
		// `world.Inbox`. Nothing fails at that moment. What fails is the next
		// `Universe` to register them properly, which aborts with "a type has one
		// name", in whichever test order happened to reach it first.
		//
		// Idempotent, so the studio phase calling it again costs a hash lookup -
		// and `OpenJsBindings` makes the same call for the same reason, which is
		// why the JavaScript walk has none.
		if (phase == ServiceAvailability::Always) {
			world::RegisterMailboxTypes();
		}

		for (const ServiceRow &row : ServiceRows(phase)) {
			if (!Permits(row.Definition, access)) {
				continue;
			}

			// **A surface first, because a surface is how both languages get
			// it.** A row this language does not bind installs nothing at all;
			// the refusal happens where a script asks for it by name, which is
			// the only place a script can tell the difference.
			if (row.Surface != nullptr) {
				InstallService(state, row.Surface());
				continue;
			}

			// **The one row no neutral description can express, dispatched by
			// name.** `BreakpointService` arms `lua_callbacks()->debugstep` and
			// reads the runtime's `Debugger` to decide whether to install at
			// all, which is not a method, a property or a signal. A function
			// pointer on the row would have put a `lua_State *` back in a header
			// `scriptjs` also reads, which is the coupling the module split
			// exists to refuse - so the catalogue names the service and this
			// file knows what to do about it.
			//
			// A second name here would be a second claim that a service cannot
			// describe itself, and it needs the same kind of argument written
			// beside it.
			if (std::strcmp(row.Definition.Name, "BreakpointService") == 0) {
				OpenBreakpointService(state);
			}
		}
	}
}
