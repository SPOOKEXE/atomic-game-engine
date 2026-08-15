// One `ServiceSurface` as a JavaScript object.
//
// **The twin of `LuauServiceSurface.cpp`, and it was inline in
// `ServiceCatalogue.cpp` until v0.18.** The argument for keeping it there was
// that the catalogue is the one file allowed to have met both VMs, which is true
// and did not follow: `ServiceSurface.hpp` only *forward-declares* `lua_State`,
// so a JavaScript translation unit reading it compiles against no Luau at all.
// What the arrangement actually cost was findability - the Luau installer was a
// file of its own and its twin was fifty lines under a heading, which is the
// asymmetry that lets one of a pair drift.
//
// **Signals first, so a method of the same name wins**, which is the order
// `InstallService` fixes for the same reason.
//
// `ServiceSurface::LuauMethods` is deliberately not installed: those are the
// rows a service has not moved across, and building a JavaScript member for a
// `lua_CFunction` is not a thing that can be done.
//
// @tier L9 · shared
// @since v0.18

#include "JsBindings.hpp"

namespace engine::script {

	void InstallJsService(JSContext *context, JSValueConst global, const ServiceSurface &surface) {
		if (surface.Name == nullptr) {
			return;
		}

		JSValue service = JS_NewObject(context);

		for (const ServiceSignal &signal : surface.Signals) {
			// No subject: a service signal is the world's, not any
			// instance's - which is what `NULL_ENTITY` means here. The name
			// filter is what tells `UserInputService`'s five apart, since
			// they share one `SignalKind`.
			JS_SetPropertyStr(
				context,
				service,
				signal.Name,
				MakeJsSignal(
					context,
					signal.Kind,
					ecs::NULL_ENTITY,
					signal.Property == nullptr ? core::Name{} : core::Name(signal.Property)
				)
			);
		}

		InstallJsServiceMethods(context, service, surface.Methods);

		// **After the methods, so an accessor wins a name a method also
		// claims** - the order `InstallService` fixes for the signals, for
		// the same reason. Nothing is in both lists today.
		InstallJsServiceProperties(context, service, surface.Name, surface.Properties);

		// **Sealed when it has properties, so a write to a name it does not
		// have is refused rather than kept.** The Luau twin is a userdata and
		// a userdata has no fields, so `SoundService.AmbientReverb = 1` raises
		// there; on an extensible object it would land as a new property and a
		// script would read its own typo back forever. Chunks run under
		// `JS_EVAL_FLAG_STRICT`, which is what turns the refused add into a
		// thrown `TypeError` rather than a silent no-op.
		//
		// Only the property-bearing services, because only those have a
		// userdata on the other side to agree with.
		if (!surface.Properties.empty()) {
			JS_PreventExtensions(context, service);
		}

		// **One object, and this line is why.** `GetService` looks the name
		// up in the globals, so the object set here is the same one
		// `game.GetService(name)` hands back rather than a second built to
		// look like it.
		JS_SetPropertyStr(context, global, surface.Name, service);
	}
}
