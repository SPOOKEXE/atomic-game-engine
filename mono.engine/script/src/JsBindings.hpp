#pragma once

// What a JavaScript script can hold and what it can touch.
//
// The JavaScript twin of `Bindings.hpp`, and deliberately the same shape: value
// types, an `Instance` constructor, and property access that switches on
// `PropertyType` and never on a property's name.
//
// **The two files share a rule rather than code.** They marshal into different
// VMs with different object models — Luau has metatables, JavaScript has
// prototypes and accessors — so a common implementation would be an abstraction
// over two runtimes' internals. What is actually shared is the thing that
// matters: one class table, one descriptor list, one set of conversions.

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Store.hpp>

#include <quickjs.h>
#include <string>

namespace engine::script {

	// Installs `Instance`, `Vector3`, `Color3` and `print` on the global object,
	// and hangs this runtime's world off the context.
	//
	// @param context The JS context.
	// @param store   The world instances are created in.
	void OpenJsBindings(JSContext *context, ecs::Store &store);

	// Releases what `OpenJsBindings` attached. Called before the context is
	// freed.
	void CloseJsBindings(JSContext *context);

	// Calls every connected Heartbeat function with `delta`.
	//
	// @return An error message when one threw, or empty.
	std::string PumpJsHeartbeat(JSContext *context, float delta);

	// Dispatches this tick's deliveries to their subscribers.
	std::string PumpJsDeliveries(JSContext *context, ecs::Store &store);

	// Wrapping and unwrapping the value types.
	JSValue MakeVector3(JSContext *context, const core::Vector3 &value);
	JSValue MakeColor3(JSContext *context, const core::Color3 &value);
	JSValue MakeCFrame(JSContext *context, const core::CFrame &value);

	core::Vector3 *AsVector3(JSContext *context, JSValueConst value);
	core::Color3 *AsColor3(JSContext *context, JSValueConst value);
	core::CFrame *AsCFrame(JSContext *context, JSValueConst value);
}
