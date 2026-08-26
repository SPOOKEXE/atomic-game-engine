// The catalogue, walked into a JavaScript context.
//
// **`LuauServices.cpp`'s twin**, and the reason both exist is that "which
// services exist" is one list rather than one per language. `script`'s
// `ServiceCatalogue` holds that list as data with no VM in it; this file turns a
// row into a property on the global object.
//
// @tier L10 · shared

#include "JsBindings.hpp"

#include <engine/script/ServiceCatalogue.hpp>
#include <engine/script/ServiceSurface.hpp>

namespace engine::script {

	void InstallJsServices(JSContext *context, JSValueConst global, ServiceAvailability phase) {
		// **No mailbox registration here, unlike the Luau walk.**
		// `OpenJsBindings` already calls `world::RegisterMailboxTypes()` before
		// anything constructs a `Postbox`, for the reason that walk states at
		// length. A second call would be harmless and would also be a second
		// place that rule is remembered.
		for (const ServiceRow &row : ServiceRows(phase)) {
			// **A surface or nothing at all**, which is the whole of this walk:
			// there is no per-language installer on this side, so a row this
			// language does not bind installs nothing and the refusal happens
			// where a script asks for it by name.
			if (row.Surface != nullptr) {
				InstallJsService(context, global, row.Surface());
			}
		}
	}
}
