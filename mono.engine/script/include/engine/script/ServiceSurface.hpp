#pragma once

// What a surface service *is*, said without naming a VM.
//
// **`ServiceCatalogue.hpp` answers "which services exist" and this answers "what
// is on one".** Both were VM-shaped once and both cost the same thing: a service
// described in `lua_CFunction`s can only be built by Luau, so every JavaScript
// service was hand-written and five of them were simply never written -
// `ContentService`, `CollectionService`, `HttpService`, `CrossWorldService` and
// `ContextActionService` were reachable from one language and not the other,
// with the catalogue naming the gap and nothing able to close it.
//
// So a service becomes data the same way a method did: a name, a list of
// `ServiceMethod` rows, a list of `ServiceProperty` rows and a list of signals.
// Each adapter reads one of these once - `InstallService` in `scriptluau`
// builds the Luau table, `InstallJsService` in `scriptjs` the JavaScript object
// - and a member added to a surface is a member in both languages in the same
// commit.
//
// **The property list is what closed the last two**, and it closed them because
// the two VMs disagree about which half is hard. Luau needs a *userdata* to
// defeat `safeenv`'s `GETIMPORT` caching but could get by with one catch-all
// `__index` string-comparing a field name; JavaScript has native accessors that
// run on every read and no caching problem at all, but registers one *per name*
// - so the names had to become data before either language could stop being the
// only one. See `ServiceProperty`.
//
// **`lua_State` is forward-declared and Luau is not included**, which is what
// lets a service file include this and stay free of a VM. `lua_CFunction` is
// `int (*)(lua_State *)` and nothing more, so the Luau-shaped rows below cost a
// forward declaration rather than a dependency.
//
// **The three functions that build one of these live in the adapter that has the
// VM**, not here: `InstallService` and its two metamethods moved to
// `scriptluau/src/LuauBindings.hpp` when the VM boundary became a module
// boundary. This module links no VM at all, so a declaration taking a
// `lua_State *` would be one nothing here could ever define.
//
// @tier L9 · shared
// @since v0.16

#include <engine/script/ScriptCall.hpp>
#include <engine/script/Signals.hpp>

#include <span>

struct lua_State;

namespace engine::script {

	// A Luau C function, without Luau.
	//
	// Spelled out rather than included, for the reason the header comment gives.
	// Assignment from a real `lua_CFunction` is assignment between one type and
	// itself.
	using LuauFunction = int (*)(lua_State *state);

	// One method on a service's table that is written per language.
	//
	// **It was the migration's other half and it is down to one service.** The
	// list was named so that it would read as a debt: `ServiceMethod` is a
	// `ScriptMethod`, a service described once is built by both VMs, and anything
	// still spelled as a `lua_CFunction` was a method JavaScript did not have.
	// `RunService`, `TweenService`, `Debris`, the bus four and
	// `ContextActionService`'s two reporting methods each sat here and each
	// moved; the last of them needed `ScriptCall::Await` and
	// `ReturnBoundAction`, which is what a row leaving this span usually costs.
	//
	// **`BreakpointService` is what is left, and it is not a debt.** Its methods
	// read the runtime's `Debugger` and arm `lua_callbacks()->debugstep`, and
	// `Debugger::Add` refuses a JavaScript chunk outright - so there is no
	// JavaScript half to write, which is why its catalogue row says
	// `ServiceLanguages::Luau` and why this span is the honest place for its four
	// methods. See `DEFERRED.md` D00106.
	//
	// A row moved from this span to `Methods` is still a row JavaScript gains in
	// the same commit; there is simply nothing left that should move.
	struct LuauServiceMethod {
		// What a script calls it. Only the Luau adapter reads this list, so the
		// name is not one any other language has to answer to.
		const char *Name;

		// The body, in Luau's own currency. A row here is a row that could not
		// be written as a `ServiceMethod`, which today means one that reaches
		// for `lua_callbacks`.
		LuauFunction Function;
	};

	// One signal exposed as a field on a service's table.
	//
	// **A field and not a method**, which is Roblox's shape and is why this is
	// its own list: `RunService.Heartbeat:Connect(f)` reads a value and calls a
	// method *on that value*, where `RunService:IsServer()` calls a method on the
	// service. The two cannot share a list because they are not built the same
	// way - a signal is a handle onto `SignalTable` and carries no context.
	//
	// **These crossed languages before the methods did**, which is why
	// `ServiceSignal` needed no change: each VM already had one way to build a
	// signal object over the one shared connection table.
	struct ServiceSignal {
		// The field the signal is installed under, and therefore what
		// `RunService.Heartbeat` resolves to.
		const char *Name;

		// Which `SignalTable` list the handle points at. Several names may share
		// one kind - `Property` below is what tells those apart.
		SignalKind Kind;

		// What a `PropertyChanged` connection filters on, or null.
		//
		// **Five signals over one kind, told apart by name**, which is exactly
		// how `GetAttributeChangedSignal` reuses `PropertyChanged`:
		// `UserInputService`'s `InputBegan`, `InputEnded`, `InputChanged`,
		// `WindowFocused` and `WindowFocusReleased` are one `SignalKind` with a
		// `NULL_ENTITY` subject, and the pump fires the one whose name matches.
		// Without this they would all be the same connection list and every
		// listener would hear every edge.
		const char *Property = nullptr;
	};

	// Everything a surface service is, as data.
	//
	// **Data rather than a base class**, for the reason the container half is a
	// table of rows: what varies between services is a name and three lists, and
	// the code that turns those into a global is the same code every time. A
	// service that needs something genuinely new adds a field here, once, where
	// every service can see it - rather than a private copy of the loop that
	// nothing else can learn from.
	struct ServiceSurface {
		// What the global is called, and therefore what
		// `game:GetService(name)` finds. `RunService::GetService` looks in the
		// globals before it looks at the tree, so naming it here is the whole of
		// making it resolvable - and it is why the global and the service are
		// **one** table rather than two objects a script could tell apart.
		const char *Name = nullptr;

		// The methods written once, which **both** languages install.
		//
		// This is what makes a `ServiceSurface` a description of a service
		// rather than of a Luau one.
		std::span<const ServiceMethod> Methods;

		// The methods only Luau has, which is `BreakpointService`'s four.
		//
		// **A second span rather than a second surface**, so a service that is
		// part way across is one description with a visible remainder - and so
		// the catalogue's language mask stays a statement about the *service*
		// while a per-method gap is stated where the methods are.
		// `TeleportService::GetTeleportData` was exactly that gap with no way to
		// say so, and it is a neutral row now; see `LuauServiceMethod` for what
		// is left and why it is not a gap at all.
		std::span<const LuauServiceMethod> LuauMethods;

		// Pushed as fields before the methods, so a service may name a signal
		// and a method the same thing and get the method. Nothing does; the
		// order is stated so that if anything ever does, it is decided here
		// rather than by which loop ran last.
		std::span<const ServiceSignal> Signals;

		// The live properties, written once, which **both** languages install.
		//
		// **A list and not a catch-all `__index`, and that is what let the last
		// two services cross.** `ServiceProperty` carries the whole argument;
		// the half that belongs here is what a non-empty list does to the Luau
		// object.
		//
		// **A service with properties *is* a userdata, and that is forced rather
		// than stylistic.** `luaL_sandbox` enables Luau's `safeenv`, which lets
		// the compiler turn a constant global and a constant field into a
		// `GETIMPORT` - resolved **once** and cached in the closure. On a table,
		// the first read of a property wins forever, so a property that changes
		// reads as one that does not. It was found by watching `__index` fire for
		// the first read of `UserInputService.MouseBehavior` and not for the
		// second, with no raw key on the table to explain it.
		//
		// A userdata's field access is never an import, so every read through a
		// local goes to `__index` - which is the form a Roblox script is written
		// in anyway, since `game:GetService` is a method call and cannot be an
		// import. `DEFERRED.md` D00030 records the edge that remains: the same
		// property read off a *bare global* still caches.
		//
		// **JavaScript has the opposite shape and none of that problem.**
		// `JS_DefinePropertyGetSet` runs on every read, so the service stays a
		// plain object with an accessor per row - which is exactly why the names
		// had to become data: an accessor is registered per name, and a single
		// catch-all could not supply them.
		//
		// So a property-bearing service needs `Tag` and `MethodsKey` as well,
		// because a userdata has no fields to hold its methods in.
		std::span<const ServiceProperty> Properties;

		// The userdata tag, from `LuauTags.hpp`. Required when
		// `Properties` is non-empty and ignored otherwise.
		int Tag = 0;

		// Where the method table is stashed for `__index` to find, since a
		// userdata cannot carry one. Required when `Properties` is non-empty and
		// ignored otherwise.
		const char *MethodsKey = nullptr;
	};
}
