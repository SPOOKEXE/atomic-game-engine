// Everything a JavaScript author reaches for once the property surface exists.
//
// **The second consumer for every binding, which is the point of having two
// VMs.** `JsBindings.cpp` is the property surface — `Instance.new`, accessors,
// `Enum`, `workspace` — and this is signals, the instance methods, `task`, the
// datatype vocabulary, the clock and the store services. Split because the two
// halves are reviewed differently and one file would have been four thousand
// lines.
//
// Nothing here decides an *order*. `SignalTable`, `ChangeQueue` and `TaskQueue`
// are the same types the Luau side uses, so which handler runs first, what a
// disconnect during a fire does and which tick a wait resumes on are answered
// once for both languages. What this file supplies is the callables and the
// spellings.
//
// ## The one genuinely different mechanism
//
// **A suspended script is a `Promise`, not a coroutine.** Luau has coroutines
// and JavaScript does not; what JavaScript has is `await`, and a promise
// resolved by the host at a barrier is exactly the same contract —
// `docs/retired/SCRIPT_CONCURRENCY.md` §1's "a script may only resume from something
// the barrier delivers in a deterministic order". `JS_ExecutePendingJob` is what
// makes that true rather than aspirational: the host drives the microtask
// queue, so a reaction runs at a point the engine picked. A JS engine without
// that API would have had to be rejected on rule 5 alone.

#include "JsBindings.hpp"
#include "Subtree.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Random.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/core/types/NumberRange.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/core/types/Rect.hpp>
#include <engine/core/types/Sequence.hpp>
#include <engine/core/types/TweenInfo.hpp>
#include <engine/core/types/UDim.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Ownership.hpp>
#include <engine/script/Datatypes.hpp>
#include <engine/world/Postbox.hpp>

#include <cmath>
#include <iterator>
#include <string>
#include <vector>

namespace engine::script {

	namespace {
		using core::Name;
		using ecs::Entity;
		using ecs::Store;
		using world::BusKind;
		using world::BusStatus;
		using world::Postbox;
		using world::Ticket;

		// --- small helpers ---------------------------------------------------

		double Number(JSContext *context, int argc, JSValueConst *argv, int index, double fallback = 0.0) {
			if (index >= argc) {
				return fallback;
			}

			double value = fallback;
			if (JS_ToFloat64(context, &value, argv[index]) != 0) {
				return fallback;
			}
			return value;
		}

		// One userdata of a plain value type.
		//
		// Every datatype below is trivially copyable and needs the same two
		// operations, so they are written once rather than eleven times. The
		// class id is what makes the check safe: `Vector2` and `UDim` are both
		// two floats, so a check on shape would pass for either.
		template <class T> JSValue Make(JSContext *context, JSClassID id, const T &value) {
			JSValue object = JS_NewObjectClass(context, static_cast<int>(id));
			if (JS_IsException(object)) {
				return object;
			}
			JS_SetOpaque(object, new T(value));
			return object;
		}

		template <class T> T *As(JSContext *context, JSValueConst value, JSClassID id) {
			auto *held = static_cast<T *>(JS_GetOpaque2(context, value, id));
			if (held == nullptr) {
				// The class check throws. Cleared, because most callers are
				// asking rather than asserting.
				JS_FreeValue(context, JS_GetException(context));
			}
			return held;
		}

		// The finaliser every value class shares.
		template <class T> void FreePayload(JSRuntime *, JSValue value) {
			JSClassID id = 0;
			delete static_cast<T *>(JS_GetAnyOpaque(value, &id));
		}

		std::string ExceptionOf(JSContext *context, const char *fallback) {
			JSValue thrown = JS_GetException(context);

			std::string message;
			if (const char *text = JS_ToCString(context, thrown); text != nullptr) {
				message = text;
				JS_FreeCString(context, text);
			}
			JS_FreeValue(context, thrown);
			return message.empty() ? fallback : message;
		}

		// --- signals ---------------------------------------------------------

		// What a signal object carries: which signal, and whose.
		//
		// Three fields and no list — the connections live in `SignalTable`, so
		// two scripts that reached the same signal by different routes hold the
		// same thing rather than two objects that behave alike.
		struct SignalPayload {
			SignalKind Kind = SignalKind::Heartbeat;
			Entity Subject;
			Name Property;
		};

		SignalPayload *AsSignal(JSContext *context, JSValueConst value) {
			return As<SignalPayload>(context, value, JsOf(context).SignalClass);
		}

		ConnectionId *AsConnection(JSContext *context, JSValueConst value) {
			return As<ConnectionId>(context, value, JsOf(context).ConnectionClass);
		}

		// Starts recording what a tree signal needs, on the first connection.
		//
		// **The same two mechanisms the Luau side installs, and neither is
		// optional here.** Four of the five are recorded and delivered at the
		// barrier, so the store has to be told to record; `DescendantRemoving`
		// is dispatched from inside the store before the removal, so the store
		// has to be given somewhere to dispatch to.
		//
		// Late rather than with the runtime, for the reason `.Changed` observes
		// late: the removal fan-out walks the leaving subtree and every
		// ancestor above it, and a world nobody asked must not pay for it.
		void WatchJsTreeFor(JSContext *context, SignalKind kind) {
			JsContext &bound = JsOf(context);

			if (kind == SignalKind::ChildAdded || kind == SignalKind::ChildRemoved ||
				kind == SignalKind::DescendantAdded || kind == SignalKind::AncestryChanged) {
				bound.World->ObserveTree();
				return;
			}

			if (kind != SignalKind::DescendantRemoving || bound.RemovingHooked) {
				return;
			}
			bound.RemovingHooked = true;

			bound.World->OnDescendantRemoving([context](Entity ancestor, Entity leaving) {
				JSValue subject = MakeJsInstance(context, leaving);

				// **Logged rather than returned**, because there is nowhere to
				// return it to: this runs underneath `Store::SetParent`, called
				// from wherever in the engine chose to move something. A
				// handler that throws must not take the reparent with it.
				const std::string failed =
					FireJsSignal(context, SignalKind::DescendantRemoving, ancestor, 1, &subject);
				if (!failed.empty()) {
					ENGINE_WARN("[script] a DescendantRemoving handler failed: {}", failed);
				}

				JS_FreeValue(context, subject);
			});
		}

		JSValue ConnectTo(JSContext *context, JSValueConst self, int argc, JSValueConst *argv, bool once) {
			JsContext &bound = JsOf(context);

			SignalPayload *signal = AsSignal(context, self);
			if (signal == nullptr) {
				return JS_ThrowTypeError(context, "not a signal");
			}
			if (argc < 1 || !JS_IsFunction(context, argv[0])) {
				return JS_ThrowTypeError(context, "Connect needs a function");
			}

			// **`.Changed` starts observing on the first connection**, not when
			// the instance is made. Observing a component is an archetype move,
			// and paying it for every part in a scene because one of them might
			// be watched later is what makes a feature not worth having.
			if (signal->Kind == SignalKind::Changed || signal->Kind == SignalKind::PropertyChanged) {
				bound.Changes.Watch(*bound.World, signal->Subject);
			}
			WatchJsTreeFor(context, signal->Kind);

			const ConnectionId id = bound.Signals.Connect(
				signal->Kind, signal->Subject, Retain(context, argv[0]), signal->Property
			);
			if (once) {
				bound.Signals.MarkOnce(id);
			}
			return MakeJsConnection(context, id);
		}

		JSValue SignalConnect(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			return ConnectTo(context, self, argc, argv, false);
		}

		JSValue SignalOnce(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			return ConnectTo(context, self, argc, argv, true);
		}

		JSValue ConnectionDisconnect(JSContext *context, JSValueConst self, int, JSValueConst *) {
			const ConnectionId *id = AsConnection(context, self);
			if (id == nullptr) {
				return JS_ThrowTypeError(context, "not a connection");
			}

			CallbackRef released = 0;
			if (JsOf(context).Signals.Disconnect(*id, released)) {
				// Released after the table has forgotten it, so a fire in
				// progress cannot reach a ref that no longer resolves.
				Release(context, released);
			}

			// Disconnecting twice is not an error, for the reason the Luau side
			// gives: a cleanup path runs whether or not something else already
			// ran, and making the ordinary case throw pushes every author into a
			// try/catch.
			return JS_UNDEFINED;
		}

		JSValue ConnectionConnected(JSContext *context, JSValueConst self) {
			const ConnectionId *id = AsConnection(context, self);
			if (id == nullptr) {
				return JS_ThrowTypeError(context, "not a connection");
			}
			return JS_NewBool(context, JsOf(context).Signals.Connected(*id));
		}

		// `a.Equals(b)` on a signal, because `===` on two objects is identity
		// and cannot be overloaded. Luau spells this `a == b` through `__eq`.
		JSValue SignalEquals(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			const SignalPayload *left = AsSignal(context, self);
			if (left == nullptr || argc < 1) {
				return JS_ThrowTypeError(context, "Equals needs a signal");
			}

			const SignalPayload *right = AsSignal(context, argv[0]);
			if (right == nullptr) {
				return JS_NewBool(context, 0);
			}
			return JS_NewBool(
				context,
				left->Kind == right->Kind && left->Subject == right->Subject &&
					left->Property == right->Property
			);
		}

		// --- instance methods ------------------------------------------------
		//
		// Every one of these is a call `Store` already had and a script could
		// not spell. They sit on a **shared prototype behind every class
		// prototype**, so a scene of five hundred parts holds one copy rather
		// than five hundred.

		Entity SelfEntity(JSContext *context, JSValueConst self) {
			return JsEntityOf(context, self);
		}

		JSValue InstanceIsA(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			const Entity instance = SelfEntity(context, self);
			if (instance == ecs::NULL_ENTITY || argc < 1) {
				return JS_ThrowTypeError(context, "IsA needs a class name");
			}

			const char *className = JS_ToCString(context, argv[0]);
			if (className == nullptr) {
				return JS_EXCEPTION;
			}

			const ecs::ClassId wanted = ecs::Classes::Find(Name(className));
			JS_FreeCString(context, className);

			// False rather than a throw for an unregistered class, matching
			// Roblox: a script testing for a class this game does not register
			// is asking a question with a correct answer, and it is "no".
			if (!wanted.IsValid()) {
				return JS_NewBool(context, 0);
			}
			return JS_NewBool(context, ecs::Classes::IsA(JsOf(context).World->ClassOf(instance), wanted));
		}

		// Forgets every listener on an instance and on everything under it, so a
		// `.Changed` connection on a destroyed row does not fire against a dead
		// handle forever.
		void ForgetInstance(JSContext *context, Entity instance) {
			JsContext &bound = JsOf(context);

			ForgetSubtree(
				*bound.World, bound.Signals, bound.Changes, instance, [context](CallbackRef reference) {
					Release(context, reference);
				}
			);
		}

		JSValue InstanceDestroy(JSContext *context, JSValueConst self, int, JSValueConst *) {
			const Entity instance = SelfEntity(context, self);
			if (instance == ecs::NULL_ENTITY) {
				return JS_ThrowTypeError(context, "not an instance");
			}

			JsContext &bound = JsOf(context);

			// The whole subtree — `DestroyInstance` takes every descendant, so a
			// listener anywhere under here would survive the row it was watching.
			ForgetInstance(context, instance);

			bound.World->DestroyInstance(instance);
			return JS_UNDEFINED;
		}

		JSValue InstanceClone(JSContext *context, JSValueConst self, int, JSValueConst *) {
			const Entity instance = SelfEntity(context, self);
			if (instance == ecs::NULL_ENTITY) {
				return JS_ThrowTypeError(context, "not an instance");
			}
			return MakeJsInstance(context, JsOf(context).World->CloneInstance(instance));
		}

		JSValue InstanceGetChildren(JSContext *context, JSValueConst self, int, JSValueConst *) {
			const Entity instance = SelfEntity(context, self);
			if (instance == ecs::NULL_ENTITY) {
				return JS_ThrowTypeError(context, "not an instance");
			}

			JSValue array = JS_NewArray(context);
			uint32_t index = 0;
			JsOf(context).World->EachChild(instance, [&](Entity child) {
				JS_SetPropertyUint32(context, array, index++, MakeJsInstance(context, child));
			});
			return array;
		}

		JSValue InstanceGetDescendants(JSContext *context, JSValueConst self, int, JSValueConst *) {
			const Entity instance = SelfEntity(context, self);
			if (instance == ecs::NULL_ENTITY) {
				return JS_ThrowTypeError(context, "not an instance");
			}

			Store &store = *JsOf(context).World;
			JSValue array = JS_NewArray(context);
			uint32_t written = 0;

			EachDescendant(store, instance, [&](Entity descendant) {
				JS_SetPropertyUint32(context, array, written++, MakeJsInstance(context, descendant));
			});
			return array;
		}

		JSValue InstanceFindFirstChild(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			const Entity instance = SelfEntity(context, self);
			if (instance == ecs::NULL_ENTITY || argc < 1) {
				return JS_ThrowTypeError(context, "FindFirstChild needs a name");
			}

			const char *name = JS_ToCString(context, argv[0]);
			if (name == nullptr) {
				return JS_EXCEPTION;
			}

			// **The second argument, which Luau's copy of this also ignored.**
			// `FindFirstChild("Humanoid", true)` answered the non-recursive
			// question and said nothing about it.
			const bool recursive = argc > 1 && JS_ToBool(context, argv[1]) > 0;

			const Entity found = JsOf(context).World->FindFirstChild(instance, name, recursive);
			JS_FreeCString(context, name);

			return found == ecs::NULL_ENTITY ? JS_NULL : MakeJsInstance(context, found);
		}

		// The class named by an argument, or an invalid id.
		ecs::ClassId JsClassArgument(JSContext *context, JSValueConst value) {
			const char *name = JS_ToCString(context, value);
			if (name == nullptr) {
				return ecs::ClassId{};
			}

			const ecs::ClassId klass = ecs::Classes::Find(core::Name(name));
			JS_FreeCString(context, name);
			return klass;
		}

		// The four class-keyed lookups, which differ only in which one they call.
		//
		// **One function and a selector rather than four near-copies**, because
		// the argument handling is the half that goes wrong — a `JS_FreeCString`
		// missed on one path is a leak nobody sees — and four copies of it is
		// four places to miss it.
		enum class JsLookup { ChildOfClass, ChildWhichIsA, AncestorOfClass, AncestorWhichIsA };

		template <JsLookup Kind>
		JSValue InstanceClassLookup(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			const Entity instance = SelfEntity(context, self);
			if (instance == ecs::NULL_ENTITY || argc < 1) {
				return JS_ThrowTypeError(context, "this needs a class name");
			}

			const ecs::ClassId klass = JsClassArgument(context, argv[0]);
			Store &store = *JsOf(context).World;

			Entity found = ecs::NULL_ENTITY;
			if constexpr (Kind == JsLookup::ChildOfClass) {
				found = store.FindFirstChildOfClass(instance, klass);
			} else if constexpr (Kind == JsLookup::ChildWhichIsA) {
				const bool recursive = argc > 1 && JS_ToBool(context, argv[1]) > 0;
				found = store.FindFirstChildWhichIsA(instance, klass, recursive);
			} else if constexpr (Kind == JsLookup::AncestorOfClass) {
				found = store.FindFirstAncestorOfClass(instance, klass);
			} else {
				found = store.FindFirstAncestorWhichIsA(instance, klass);
			}

			return found == ecs::NULL_ENTITY ? JS_NULL : MakeJsInstance(context, found);
		}

		JSValue
		InstanceFindFirstAncestor(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			const Entity instance = SelfEntity(context, self);
			if (instance == ecs::NULL_ENTITY || argc < 1) {
				return JS_ThrowTypeError(context, "FindFirstAncestor needs a name");
			}

			const char *name = JS_ToCString(context, argv[0]);
			if (name == nullptr) {
				return JS_EXCEPTION;
			}

			const Entity found = JsOf(context).World->FindFirstAncestor(instance, name);
			JS_FreeCString(context, name);

			return found == ecs::NULL_ENTITY ? JS_NULL : MakeJsInstance(context, found);
		}

		JSValue InstanceGetFullName(JSContext *context, JSValueConst self, int, JSValueConst *) {
			const Entity instance = SelfEntity(context, self);
			if (instance == ecs::NULL_ENTITY) {
				return JS_ThrowTypeError(context, "GetFullName needs an instance");
			}

			const std::string full = JsOf(context).World->GetFullName(instance);
			return JS_NewStringLen(context, full.data(), full.size());
		}

		JSValue InstanceIsDescendantOf(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			const Entity instance = SelfEntity(context, self);
			if (instance == ecs::NULL_ENTITY || argc < 1) {
				return JS_ThrowTypeError(context, "IsDescendantOf needs an instance");
			}

			JsContext &bound = JsOf(context);

			// No case for the workspace any more, and losing it is the point:
			// this used to be true for every live instance in the world, because
			// the world was every root's ancestor. It is now the real subtree
			// question — the same one the render gate asks — so a script and the
			// renderer cannot disagree about whether something is in the scene.
			return JS_NewBool(context, bound.World->IsDescendantOf(instance, JsEntityOf(context, argv[0])));
		}

		// The other half of the Luau pair in `Instances.cpp`, spelled the way
		// this language spells it: `null` rather than `nil`, and a missing
		// argument means the same thing as passing it.
		JSValue InstanceSetNetworkOwner(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			const Entity instance = SelfEntity(context, self);
			if (instance == ecs::NULL_ENTITY) {
				return JS_ThrowTypeError(context, "not an instance");
			}

			// **The absent cases are named rather than derived.** `JsEntityOf`
			// answers a null entity for *anything* that is not an instance, so
			// letting it decide would turn `SetNetworkOwner(5)` into a silent
			// hand-back to the server — the failure this whole method exists to
			// make visible.
			const bool toTheServer = argc < 1 || JS_IsNull(argv[0]) != 0 || JS_IsUndefined(argv[0]) != 0;
			const Entity player = toTheServer ? ecs::NULL_ENTITY : JsEntityOf(context, argv[0]);

			if (!toTheServer && player == ecs::NULL_ENTITY) {
				return JS_ThrowTypeError(context, "SetNetworkOwner expects a Player or null");
			}

			if (!scene::SetNetworkOwner(*JsOf(context).World, instance, player)) {
				return JS_ThrowTypeError(context, "SetNetworkOwner expects a Player or null");
			}
			return JS_UNDEFINED;
		}

		JSValue InstanceGetNetworkOwner(JSContext *context, JSValueConst self, int, JSValueConst *) {
			const Entity instance = SelfEntity(context, self);
			if (instance == ecs::NULL_ENTITY) {
				return JS_ThrowTypeError(context, "not an instance");
			}

			const Entity owner = scene::NetworkOwnerOf(*JsOf(context).World, instance);
			return owner == ecs::NULL_ENTITY ? JS_NULL : MakeJsInstance(context, owner);
		}

		JSValue InstanceClearAllChildren(JSContext *context, JSValueConst self, int, JSValueConst *) {
			const Entity instance = SelfEntity(context, self);
			if (instance == ecs::NULL_ENTITY) {
				return JS_ThrowTypeError(context, "not an instance");
			}

			JsContext &bound = JsOf(context);

			// Collected first: `DestroyInstance` unlinks from the sibling list
			// the walk is standing in, so destroying inside `EachChild` would
			// visit whatever moved into the slot — or nothing.
			std::vector<Entity> children;
			bound.World->EachChild(instance, [&](Entity child) { children.push_back(child); });

			for (const Entity child : children) {
				ForgetInstance(context, child);
				bound.World->DestroyInstance(child);
			}
			return JS_UNDEFINED;
		}

		JSValue
		InstancePropertyChangedSignal(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			const Entity instance = SelfEntity(context, self);
			if (instance == ecs::NULL_ENTITY || argc < 1) {
				return JS_ThrowTypeError(context, "GetPropertyChangedSignal needs a name");
			}

			const char *field = JS_ToCString(context, argv[0]);
			if (field == nullptr) {
				return JS_EXCEPTION;
			}

			// Refused for a property that does not exist, which is the one place
			// a typo in a signal name is still catchable: a signal that silently
			// never fired would be indistinguishable from a value that never
			// changed.
			bool known = false;
			for (const ecs::PropertyDescriptor &property : JsOf(context).World->PropertiesOf(instance)) {
				if (property.Name == Name(field)) {
					known = true;
					break;
				}
			}

			if (!known) {
				JSValue error =
					JS_ThrowTypeError(context, "'%s' is not a valid member of this instance", field);
				JS_FreeCString(context, field);
				return error;
			}

			JSValue signal = MakeJsSignal(context, SignalKind::PropertyChanged, instance, Name(field));
			JS_FreeCString(context, field);
			return signal;
		}

		// `instance.Changed` — a getter, because it takes no arguments and
		// Roblox spells it as a property.
		JSValue InstanceChanged(JSContext *context, JSValueConst self) {
			const Entity instance = SelfEntity(context, self);
			if (instance == ecs::NULL_ENTITY) {
				return JS_ThrowTypeError(context, "not an instance");
			}
			return MakeJsSignal(context, SignalKind::Changed, instance);
		}

		// The tree's own signals, as getters for the same reason `Changed` is
		// one: Roblox spells them as properties and they take no arguments.
		//
		// **One template rather than five near-copies.** Each differs only in
		// which kind it names, and five hand-written bodies is five places for
		// the null check to be forgotten.
		template <SignalKind Kind> JSValue InstanceTreeSignal(JSContext *context, JSValueConst self) {
			const Entity instance = SelfEntity(context, self);
			if (instance == ecs::NULL_ENTITY) {
				return JS_ThrowTypeError(context, "not an instance");
			}
			return MakeJsSignal(context, Kind, instance);
		}

		// --- the codec bridge ------------------------------------------------

		bool ObjectToScriptValue(
			JSContext *context, JSValueConst value, ScriptValue &out, uint32_t depth, CodecStatus &why
		);

		// --- task ------------------------------------------------------------

		// A promise, with its resolver kept for the barrier to call.
		//
		// `JS_NewPromiseCapability` hands back the promise and the two functions
		// that settle it. The resolver is retained through the same `CallbackRef`
		// machinery a connection uses, so `TaskQueue` — which knows nothing about
		// JavaScript — can name it.
		JSValue MakePendingPromise(JSContext *context, CallbackRef &resolver) {
			JSValue settle[2];
			JSValue promise = JS_NewPromiseCapability(context, settle);
			if (JS_IsException(promise)) {
				return promise;
			}

			resolver = Retain(context, settle[0]);
			JS_FreeValue(context, settle[0]);
			JS_FreeValue(context, settle[1]);
			return promise;
		}

		// How many ticks a duration in seconds rounds to.
		//
		// **Up, and never to zero**, for the reason the Luau side gives:
		// `task.wait(0)` resumes on the next tick rather than inside this one,
		// and a wait that resumed in the same beat would make a loop over it an
		// infinite loop inside one tick.
		uint64_t TicksFor(const JsContext &bound, double seconds) {
			const float delta = bound.World->Time().Delta;
			if (seconds <= 0.0 || delta <= 0.0f) {
				return 1;
			}

			const double ticks = std::ceil(seconds / static_cast<double>(delta));
			return ticks < 1.0 ? 1 : static_cast<uint64_t>(ticks);
		}

		JSValue TaskWait(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			JsContext &bound = JsOf(context);
			const uint64_t ticks = TicksFor(bound, Number(context, argc, argv, 0, 0.0));

			CallbackRef resolver = 0;
			JSValue promise = MakePendingPromise(context, resolver);
			if (JS_IsException(promise)) {
				return promise;
			}

			bound.Tasks.Delay(resolver, bound.World->Time().Tick + ticks);
			bound.WaitTicks.insert_or_assign(resolver, ticks);
			return promise;
		}

		JSValue TaskSpawn(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			if (argc < 1 || !JS_IsFunction(context, argv[0])) {
				return JS_ThrowTypeError(context, "task.spawn needs a function");
			}

			// **Synchronous up to the first suspension**, which is what Roblox's
			// `task.spawn` is. There is no thread to make: an async function
			// called here runs to its first `await` and returns a promise the
			// host's job queue will drive.
			JSValue result = JS_Call(context, argv[0], JS_UNDEFINED, argc - 1, argv + 1);
			if (JS_IsException(result)) {
				return result;
			}
			return result;
		}

		JSValue TaskDefer(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			if (argc < 1 || !JS_IsFunction(context, argv[0])) {
				return JS_ThrowTypeError(context, "task.defer needs a function");
			}

			JsContext &bound = JsOf(context);
			bound.Tasks.Defer(Retain(context, argv[0]));
			return JS_UNDEFINED;
		}

		JSValue TaskDelay(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			if (argc < 2 || !JS_IsFunction(context, argv[1])) {
				return JS_ThrowTypeError(context, "task.delay needs a duration and a function");
			}

			JsContext &bound = JsOf(context);
			const uint64_t ticks = TicksFor(bound, Number(context, argc, argv, 0, 0.0));

			const CallbackRef reference = Retain(context, argv[1]);
			bound.Tasks.Delay(reference, bound.World->Time().Tick + ticks);
			return JS_NewInt32(context, reference);
		}

		// `task.cancel(handle)` — the integer `task.delay` returned.
		//
		// **A number rather than a thread object**, because JavaScript has no
		// thread to hand back. Luau returns the coroutine; here the handle is
		// the same integer the queue is keyed on, which is the honest shape
		// rather than a wrapper object that would only ever hold it.
		JSValue TaskCancel(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			if (argc < 1) {
				return JS_ThrowTypeError(context, "task.cancel needs a handle");
			}

			int32_t handle = 0;
			if (JS_ToInt32(context, &handle, argv[0]) != 0) {
				return JS_EXCEPTION;
			}

			JsContext &bound = JsOf(context);
			const bool cancelled = bound.Tasks.Cancel(handle);
			if (cancelled) {
				bound.WaitTicks.erase(handle);
				Release(context, handle);
			}
			return JS_NewBool(context, cancelled);
		}

		// --- the clock -------------------------------------------------------

		JSValue ClockTime(JSContext *context, JSValueConst, int, JSValueConst *) {
			return JS_NewFloat64(context, JsOf(context).World->Time().Elapsed);
		}

		JSValue ClockTick(JSContext *context, JSValueConst, int, JSValueConst *) {
			return JS_NewFloat64(context, static_cast<double>(JsOf(context).World->Time().Tick));
		}

		JSValue DateTimeNow(JSContext *context, JSValueConst, int, JSValueConst *) {
			return JS_ThrowTypeError(
				context,
				"DateTime.now() does not exist here: a world's clock is simulated, and a script "
				"branching on wall time produces a run that does not replay. Use "
				"DateTime.fromSimulated() or DateTime.fromUnixTimestamp(n)"
			);
		}

		JSValue MakeDateTime(JSContext *context, double seconds) {
			JSValue object = JS_NewObject(context);
			JS_SetPropertyStr(context, object, "UnixTimestamp", JS_NewFloat64(context, seconds));
			JS_SetPropertyStr(
				context, object, "UnixTimestampMillis", JS_NewFloat64(context, seconds * 1000.0)
			);
			JS_PreventExtensions(context, object);
			return object;
		}

		JSValue DateTimeFromSimulated(JSContext *context, JSValueConst, int, JSValueConst *) {
			return MakeDateTime(context, JsOf(context).World->Time().Elapsed);
		}

		JSValue DateTimeFromUnix(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			return MakeDateTime(context, Number(context, argc, argv, 0, 0.0));
		}

		// --- typeOf and warn -------------------------------------------------

		// `typeOf(value)` — **not `typeof`**, which is a JavaScript keyword and
		// cannot be rebound. Luau's is an ordinary global reading a `__type`
		// metafield; this is a function, and the difference is the language's.
		JSValue TypeOf(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			if (argc < 1) {
				return JS_NewString(context, "nil");
			}

			JsContext &bound = JsOf(context);
			const struct {
				JSClassID Class;
				const char *Name;
			} CLASSES[] = {
				{bound.InstanceClass, "Instance"},
				{bound.Vector3Class, "Vector3"},
				{bound.Color3Class, "Color3"},
				{bound.CFrameClass, "CFrame"},
				{bound.Vector2Class, "Vector2"},
				{bound.UDimClass, "UDim"},
				{bound.UDim2Class, "UDim2"},
				{bound.RectClass, "Rect"},
				{bound.Region3Class, "Region3"},
				{bound.NumberRangeClass, "NumberRange"},
				{bound.NumberSequenceClass, "NumberSequence"},
				{bound.ColorSequenceClass, "ColorSequence"},
				{bound.TweenInfoClass, "TweenInfo"},
				{bound.RayClass, "Ray"},
				{bound.RandomClass, "Random"},
				{bound.SignalClass, "RBXScriptSignal"},
				{bound.ConnectionClass, "RBXScriptConnection"},
				{bound.EnumItemClass, "EnumItem"},
				{bound.RaycastParamsClass, "RaycastParams"},
			};

			for (const auto &entry : CLASSES) {
				if (JS_GetOpaque(argv[0], entry.Class) != nullptr) {
					return JS_NewString(context, entry.Name);
				}
			}

			if (JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
				return JS_NewString(context, "nil");
			}
			if (JS_IsBool(argv[0])) {
				return JS_NewString(context, "boolean");
			}
			if (JS_IsNumber(argv[0])) {
				return JS_NewString(context, "number");
			}
			if (JS_IsString(argv[0])) {
				return JS_NewString(context, "string");
			}
			if (JS_IsFunction(context, argv[0])) {
				return JS_NewString(context, "function");
			}
			return JS_NewString(context, "table");
		}

		JSValue Warn(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			std::string line;
			for (int index = 0; index < argc; index++) {
				if (index > 0) {
					line += ' ';
				}
				if (const char *text = JS_ToCString(context, argv[index]); text != nullptr) {
					line += text;
					JS_FreeCString(context, text);
				}
			}

			ENGINE_WARN("[script] {}", line);
			return JS_UNDEFINED;
		}

		// --- RunService ------------------------------------------------------

		JSValue IsServer(JSContext *context, JSValueConst, int, JSValueConst *) {
			return JS_NewBool(context, JsOf(context).Role.Server);
		}

		JSValue IsClient(JSContext *context, JSValueConst, int, JSValueConst *) {
			return JS_NewBool(context, JsOf(context).Role.Client);
		}

		JSValue IsStudio(JSContext *context, JSValueConst, int, JSValueConst *) {
			return JS_NewBool(context, JsOf(context).Role.Studio);
		}

		JSValue IsReplica(JSContext *context, JSValueConst, int, JSValueConst *) {
			return JS_NewBool(context, JsOf(context).World->AdoptOnly());
		}
	}

	// --- the codec bridge, in full -------------------------------------------

	namespace {
		bool ArrayToScriptValue(
			JSContext *context, JSValueConst value, ScriptValue &out, uint32_t depth, CodecStatus &why
		) {
			out = ScriptValue{ValueTag::Array};

			JSValue lengthValue = JS_GetPropertyStr(context, value, "length");
			uint32_t length = 0;
			JS_ToUint32(context, &length, lengthValue);
			JS_FreeValue(context, lengthValue);

			out.Items.resize(length);
			for (uint32_t index = 0; index < length; index++) {
				JSValue item = JS_GetPropertyUint32(context, value, index);
				const bool ok = ToScriptValue(context, item, out.Items[index], depth + 1, why);
				JS_FreeValue(context, item);
				if (!ok) {
					return false;
				}
			}
			return true;
		}

		bool ObjectToScriptValue(
			JSContext *context, JSValueConst value, ScriptValue &out, uint32_t depth, CodecStatus &why
		) {
			out = ScriptValue{ValueTag::Map};

			JSPropertyEnum *properties = nullptr;
			uint32_t count = 0;
			if (JS_GetOwnPropertyNames(
					context, &properties, &count, value, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY
				) != 0) {
				why = CodecStatus::Unsupported;
				return false;
			}

			bool ok = true;
			for (uint32_t index = 0; index < count && ok; index++) {
				JSValue key = JS_AtomToString(context, properties[index].atom);
				size_t length = 0;
				const char *text = JS_ToCStringLen(context, &length, key);

				JSValue held = JS_GetProperty(context, value, properties[index].atom);

				ScriptValue child;
				ok = text != nullptr && ToScriptValue(context, held, child, depth + 1, why);
				if (ok) {
					out.Entries.emplace_back(std::string(text, length), std::move(child));
				}

				JS_FreeCString(context, text);
				JS_FreeValue(context, held);
				JS_FreeValue(context, key);
			}

			for (uint32_t index = 0; index < count; index++) {
				JS_FreeAtom(context, properties[index].atom);
			}
			js_free(context, properties);
			return ok;
		}
	}

	bool ToScriptValue(
		JSContext *context, JSValueConst value, ScriptValue &out, uint32_t depth, CodecStatus &why
	) {
		if (depth > CODEC_MAX_DEPTH) {
			// **Depth is how a cycle is caught here**, rather than a visited
			// set as on the Luau side. QuickJS has no cheap identity map for
			// objects reachable from C, and a cyclic object cannot nest past the
			// limit without hitting it first — so the refusal arrives, with a
			// less specific name than `Cyclic`. Stated rather than hidden: the
			// status a script sees for a self-referencing object is `TooDeep`.
			why = CodecStatus::TooDeep;
			return false;
		}

		if (JS_IsNull(value) || JS_IsUndefined(value)) {
			out = ScriptValue{ValueTag::Nil};
			return true;
		}
		if (JS_IsBool(value)) {
			out = ScriptValue{JS_ToBool(context, value) == 1 ? ValueTag::True : ValueTag::False};
			out.Boolean = JS_ToBool(context, value) == 1;
			return true;
		}
		if (JS_IsNumber(value)) {
			out = ScriptValue{ValueTag::Number};
			JS_ToFloat64(context, &out.Number, value);
			return true;
		}
		if (JS_IsString(value)) {
			size_t length = 0;
			const char *text = JS_ToCStringLen(context, &length, value);
			if (text == nullptr) {
				why = CodecStatus::Unsupported;
				return false;
			}

			out = ScriptValue{ValueTag::String};
			out.Text.assign(text, length);
			JS_FreeCString(context, text);
			return true;
		}

		if (const core::Vector3 *vector = AsVector3(context, value); vector != nullptr) {
			out = ScriptValue{ValueTag::Vector3};
			out.Vector = *vector;
			return true;
		}
		if (const core::Color3 *colour = AsColor3(context, value); colour != nullptr) {
			out = ScriptValue{ValueTag::Color3};
			out.Colour = *colour;
			return true;
		}
		if (const core::CFrame *frame = AsCFrame(context, value); frame != nullptr) {
			out = ScriptValue{ValueTag::CFrame};
			out.Frame = *frame;
			return true;
		}

		if (JS_IsArray(value) != 0) {
			return ArrayToScriptValue(context, value, out, depth, why);
		}
		if (JS_IsFunction(context, value)) {
			// A function, an instance, or anything holding a pointer. **An
			// `Entity` is meaningless outside this world**, so a reference must
			// cross as whatever the game uses to name things — rule 3, as a
			// refusal an author can read.
			why = CodecStatus::Unsupported;
			return false;
		}
		if (JS_IsObject(value)) {
			if (JsEntityOf(context, value) != ecs::NULL_ENTITY) {
				why = CodecStatus::Unsupported;
				return false;
			}
			return ObjectToScriptValue(context, value, out, depth, why);
		}

		why = CodecStatus::Unsupported;
		return false;
	}

	JSValue FromScriptValue(JSContext *context, const ScriptValue &value) {
		switch (value.Tag) {
		case ValueTag::Nil:
			return JS_NULL;
		case ValueTag::False:
		case ValueTag::True:
			return JS_NewBool(context, value.Boolean);
		case ValueTag::Number:
			return JS_NewFloat64(context, value.Number);
		case ValueTag::String:
			return JS_NewStringLen(context, value.Text.data(), value.Text.size());
		case ValueTag::Array: {
			JSValue array = JS_NewArray(context);
			for (size_t index = 0; index < value.Items.size(); index++) {
				JS_SetPropertyUint32(
					context, array, static_cast<uint32_t>(index), FromScriptValue(context, value.Items[index])
				);
			}
			return array;
		}
		case ValueTag::Map: {
			JSValue object = JS_NewObject(context);
			for (const auto &entry : value.Entries) {
				JS_SetPropertyStr(
					context, object, entry.first.c_str(), FromScriptValue(context, entry.second)
				);
			}
			return object;
		}
		case ValueTag::Vector3:
			return MakeVector3(context, value.Vector);
		case ValueTag::Color3:
			return MakeColor3(context, value.Colour);
		case ValueTag::CFrame:
			return MakeCFrame(context, value.Frame);
		}
		return JS_NULL;
	}

	// --- the pumps ------------------------------------------------------------

	std::string
	FireJsSignal(JSContext *context, SignalKind kind, Entity subject, int count, JSValueConst *arguments) {
		JsContext &bound = JsOf(context);

		std::string firstError;
		std::vector<ConnectionId> spent;

		bound.Signals.Fire(kind, subject, [&](const Connection &connection) {
			JSValue result =
				JS_Call(context, Held(context, connection.Callback), JS_UNDEFINED, count, arguments);

			// **Every connection runs even when one throws**, and the first
			// error is what the host hears about. A handler that threw once
			// would otherwise silently stop everything registered after it, and
			// the symptom — half a scene animating — points nowhere near the
			// cause.
			if (JS_IsException(result)) {
				const std::string message = ExceptionOf(context, "a connection failed");
				if (firstError.empty()) {
					firstError = message;
				}
			}
			JS_FreeValue(context, result);

			if (connection.Once) {
				spent.push_back(connection.Id);
			}
		});

		// Retired after the fire rather than inside it, so a `Once` handler that
		// connected another one does not have its list compacted under the walk.
		for (const ConnectionId id : spent) {
			CallbackRef released = 0;
			if (bound.Signals.Disconnect(id, released)) {
				Release(context, released);
			}
		}
		return firstError;
	}

	std::string PumpJsChanges(JSContext *context) {
		JsContext &bound = JsOf(context);
		if (bound.Changes.Empty()) {
			return {};
		}

		std::string firstError;
		bound.Changes.Drain([&](Entity instance, Name property) {
			JSValue name = JS_NewString(context, property.Text().data());

			const std::string changed = FireJsSignal(context, SignalKind::Changed, instance, 1, &name);
			if (firstError.empty()) {
				firstError = changed;
			}

			// `GetPropertyChangedSignal` takes no argument and fires only for
			// its own name, which is Roblox's split and the reason it exists —
			// a handler that cares about one property should not be called for
			// every other one and made to filter.
			bound.Signals.Fire(SignalKind::PropertyChanged, instance, [&](const Connection &connection) {
				if (connection.Property != property) {
					return;
				}

				JSValue result =
					JS_Call(context, Held(context, connection.Callback), JS_UNDEFINED, 0, nullptr);
				if (JS_IsException(result)) {
					const std::string message = ExceptionOf(context, "a property listener failed");
					if (firstError.empty()) {
						firstError = message;
					}
				}
				JS_FreeValue(context, result);
			});

			JS_FreeValue(context, name);
		});
		return firstError;
	}

	std::string PumpJsTree(JSContext *context) {
		JsContext &bound = JsOf(context);
		if (!bound.World->TreeObserved()) {
			return {};
		}

		// **Taken, not read.** A handler may reparent something, and a swap
		// leaves the store's list empty before the first one runs — so the move
		// it makes belongs to the next delivery instead of being appended to
		// the list being walked.
		std::vector<ecs::TreeChange> changes;
		bound.World->TakeTreeChanges(changes);
		if (changes.empty()) {
			return {};
		}

		std::string firstError;
		const auto note = [&](std::string message) {
			if (firstError.empty() && !message.empty()) {
				firstError = std::move(message);
			}
		};

		// One argument for three of the four signals, freed once per use rather
		// than once per fire — `MakeJsInstance` mints an object per call, and
		// leaking one per reparent is a leak per reparent.
		const auto fire = [&](SignalKind kind, Entity subject, Entity argument) {
			JSValue value = MakeJsInstance(context, argument);
			note(FireJsSignal(context, kind, subject, 1, &value));
			JS_FreeValue(context, value);
		};

		for (const ecs::TreeChange &change : changes) {
			if (change.From != ecs::NULL_ENTITY) {
				fire(SignalKind::ChildRemoved, change.From, change.Instance);
			}

			if (change.To != ecs::NULL_ENTITY) {
				fire(SignalKind::ChildAdded, change.To, change.Instance);

				// `DescendantAdded` is every ancestor's, not just the new
				// parent's — that is the whole difference between it and
				// `ChildAdded`.
				for (Entity above = change.To; above != ecs::NULL_ENTITY;
					 above = bound.World->ParentOf(above)) {
					fire(SignalKind::DescendantAdded, above, change.Instance);
				}
			}

			// **The instance and everything under it**, because an ancestry
			// change is inherited: moving a model changes the ancestry of every
			// part in it, and a script watching a part has no way to know its
			// model moved otherwise.
			const auto ancestry = [&](Entity subject) {
				JSValue arguments[2] = {
					MakeJsInstance(context, subject),
					MakeJsInstance(context, bound.World->ParentOf(subject)),
				};
				note(FireJsSignal(context, SignalKind::AncestryChanged, subject, 2, arguments));
				JS_FreeValue(context, arguments[0]);
				JS_FreeValue(context, arguments[1]);
			};

			ancestry(change.Instance);
			bound.World->EachDescendant(change.Instance, ancestry);
		}

		return firstError;
	}

	std::string PumpJsGuiEvents(JSContext *context, std::span<const gui::GuiEvent> events) {
		if (events.empty()) {
			return {};
		}

		JsContext &bound = JsOf(context);

		std::string firstError;
		const auto note = [&](std::string message) {
			if (firstError.empty()) {
				firstError = std::move(message);
			}
		};

		for (const gui::GuiEvent &event : events) {
			// The Luau pump's reason, unchanged: a handler earlier in this loop
			// may have destroyed what a later one is about, and a close button
			// is the ordinary case rather than an edge one.
			if (!bound.World->Alive(event.Instance)) {
				continue;
			}

			switch (event.Kind) {
			case gui::EventKind::MouseEnter:
			case gui::EventKind::MouseLeave:
			case gui::EventKind::MouseMoved: {
				const SignalKind kind = event.Kind == gui::EventKind::MouseEnter ? SignalKind::GuiMouseEnter
										: event.Kind == gui::EventKind::MouseLeave
											? SignalKind::GuiMouseLeave
											: SignalKind::GuiMouseMoved;

				JSValue arguments[2] = {
					JS_NewFloat64(context, static_cast<double>(event.Position.X)),
					JS_NewFloat64(context, static_cast<double>(event.Position.Y)),
				};
				note(FireJsSignal(context, kind, event.Instance, 2, arguments));
				JS_FreeValue(context, arguments[0]);
				JS_FreeValue(context, arguments[1]);
				break;
			}

			case gui::EventKind::InputBegan:
				note(FireJsSignal(context, SignalKind::GuiInputBegan, event.Instance, 0, nullptr));
				break;

			case gui::EventKind::InputEnded:
				note(FireJsSignal(context, SignalKind::GuiInputEnded, event.Instance, 0, nullptr));
				break;

			case gui::EventKind::Activated:
				note(FireJsSignal(context, SignalKind::GuiActivated, event.Instance, 0, nullptr));
				break;
			}
		}

		return firstError;
	}

	std::string PumpJsTasks(JSContext *context) {
		JsContext &bound = JsOf(context);
		std::string firstError;

		const auto resume = [&](CallbackRef reference) {
			// A `task.wait` resolves its promise with how long it waited; a
			// `task.defer` or `task.delay` is a plain call.
			const auto waited = bound.WaitTicks.find(reference);
			const bool isWait = waited != bound.WaitTicks.end();

			JSValue argument = JS_UNDEFINED;
			if (isWait) {
				argument = JS_NewFloat64(
					context,
					static_cast<double>(waited->second) * static_cast<double>(bound.World->Time().Delta)
				);
				bound.WaitTicks.erase(waited);
			}

			JSValue result =
				JS_Call(context, Held(context, reference), JS_UNDEFINED, isWait ? 1 : 0, &argument);
			if (JS_IsException(result)) {
				const std::string message = ExceptionOf(context, "a resumed task failed");
				if (firstError.empty()) {
					firstError = message;
				}
			}

			JS_FreeValue(context, result);
			JS_FreeValue(context, argument);
			Release(context, reference);
		};

		// Delayed work first, then deferred — the same order the Luau side
		// uses, so a world scripted in either language sees one sequence.
		bound.Tasks.Advance(bound.World->Time().Tick, resume);
		bound.Tasks.DrainDeferred(resume);
		return firstError;
	}

	JSValue MakeJsSignal(JSContext *context, SignalKind kind, Entity subject, Name property) {
		return Make(context, JsOf(context).SignalClass, SignalPayload{kind, subject, property});
	}

	JSValue MakeJsConnection(JSContext *context, ConnectionId id) {
		return Make(context, JsOf(context).ConnectionClass, id);
	}

	// --- installation ---------------------------------------------------------

	namespace {
		// One class id, its finaliser and its prototype, in one line at the call
		// site. Eleven datatypes would otherwise be eleven copies of six lines.
		template <class T>
		void InstallClass(
			JSContext *context,
			JSClassID &id,
			const char *name,
			const JSCFunctionListEntry *members,
			int count
		) {
			// **Static per instantiation, and that is why `T` is the template
			// parameter rather than a runtime argument.** `JSClassDef` holds a
			// function pointer the runtime keeps, so it has to outlive this
			// call; a local would dangle and a shared one could only hold one
			// finaliser.
			static JSClassDef definition = {name, FreePayload<T>, nullptr, nullptr, nullptr};
			definition.class_name = name;

			JSRuntime *runtime = JS_GetRuntime(context);
			JS_NewClassID(runtime, &id);
			JS_NewClass(runtime, id, &definition);

			JSValue proto = JS_NewObject(context);
			if (members != nullptr && count > 0) {
				JS_SetPropertyFunctionList(context, proto, members, count);
			}
			JS_SetClassProto(context, id, proto);
		}
	}

	void InstallJsInstanceMethods(JSContext *context) {
		JsContext &bound = JsOf(context);
		JSValue global = JS_GetGlobalObject(context);

		JSValue methods = JS_NewObject(context);
		static const JSCFunctionListEntry entries[] = {
			JS_CFUNC_DEF("IsA", 1, InstanceIsA),
			JS_CFUNC_DEF("Destroy", 0, InstanceDestroy),
			JS_CFUNC_DEF("Clone", 0, InstanceClone),
			JS_CFUNC_DEF("GetChildren", 0, InstanceGetChildren),
			JS_CFUNC_DEF("GetDescendants", 0, InstanceGetDescendants),
			JS_CFUNC_DEF("FindFirstChild", 1, InstanceFindFirstChild),
			JS_CFUNC_DEF("FindFirstChildOfClass", 1, InstanceClassLookup<JsLookup::ChildOfClass>),
			JS_CFUNC_DEF("FindFirstChildWhichIsA", 1, InstanceClassLookup<JsLookup::ChildWhichIsA>),
			JS_CFUNC_DEF("FindFirstAncestor", 1, InstanceFindFirstAncestor),
			JS_CFUNC_DEF("FindFirstAncestorOfClass", 1, InstanceClassLookup<JsLookup::AncestorOfClass>),
			JS_CFUNC_DEF("FindFirstAncestorWhichIsA", 1, InstanceClassLookup<JsLookup::AncestorWhichIsA>),
			JS_CFUNC_DEF("GetFullName", 0, InstanceGetFullName),
			JS_CFUNC_DEF("IsDescendantOf", 1, InstanceIsDescendantOf),
			JS_CFUNC_DEF("ClearAllChildren", 0, InstanceClearAllChildren),
			JS_CFUNC_DEF("SetNetworkOwner", 1, InstanceSetNetworkOwner),
			JS_CFUNC_DEF("GetNetworkOwner", 0, InstanceGetNetworkOwner),
			JS_CFUNC_DEF("GetPropertyChangedSignal", 1, InstancePropertyChangedSignal),
			JS_CGETSET_DEF("Changed", InstanceChanged, nullptr),
			JS_CGETSET_DEF("ChildAdded", InstanceTreeSignal<SignalKind::ChildAdded>, nullptr),
			JS_CGETSET_DEF("ChildRemoved", InstanceTreeSignal<SignalKind::ChildRemoved>, nullptr),
			JS_CGETSET_DEF("DescendantAdded", InstanceTreeSignal<SignalKind::DescendantAdded>, nullptr),
			JS_CGETSET_DEF("DescendantRemoving", InstanceTreeSignal<SignalKind::DescendantRemoving>, nullptr),
			JS_CGETSET_DEF("AncestryChanged", InstanceTreeSignal<SignalKind::AncestryChanged>, nullptr),

			// The 2D tree's input. Same template, because a gui signal is a
			// handle onto `SignalTable` exactly as a tree signal is — what
			// differs is only who records it, and that is the pump's business
			// rather than this getter's.
			JS_CGETSET_DEF("Activated", InstanceTreeSignal<SignalKind::GuiActivated>, nullptr),
			JS_CGETSET_DEF("InputBegan", InstanceTreeSignal<SignalKind::GuiInputBegan>, nullptr),
			JS_CGETSET_DEF("InputEnded", InstanceTreeSignal<SignalKind::GuiInputEnded>, nullptr),
			JS_CGETSET_DEF("MouseEnter", InstanceTreeSignal<SignalKind::GuiMouseEnter>, nullptr),
			JS_CGETSET_DEF("MouseLeave", InstanceTreeSignal<SignalKind::GuiMouseLeave>, nullptr),
			JS_CGETSET_DEF("MouseMoved", InstanceTreeSignal<SignalKind::GuiMouseMoved>, nullptr),
		};
		// **`std::size`, not a number somebody has to remember.** This read
		// `10` while the list held sixteen, so the last six — including
		// `IsDescendantOf`, `GetPropertyChangedSignal` and `Changed` — were
		// simply not installed. Nothing warned: a method that is not there
		// is `undefined`, and `undefined` only fails at the call site, in
		// whatever script reaches it first.
		JS_SetPropertyFunctionList(context, methods, entries, static_cast<int>(std::size(entries)));

		// Held in the context so `PrototypeFor` can put every class
		// prototype behind it, and so it is freed with everything else.
		bound.Owned.push_back(JS_DupValue(context, methods));
		JS_SetPropertyStr(context, global, "__instanceMethods", methods);

		JS_FreeValue(context, global);
	}

	void OpenJsSurface(JSContext *context) {
		JsContext &bound = JsOf(context);
		JSValue global = JS_GetGlobalObject(context);

		// The two enums this vocabulary needs. Shared with the Luau surface and
		// with the bindings generator rather than listed again here: process-wide
		// registration takes a second declaration as agreement, which is what
		// kept the duplicate invisible until a third caller needed the same list.
		RegisterDatatypeEnums();

		// --- signals ---
		{
			static const JSCFunctionListEntry members[] = {
				JS_CFUNC_DEF("Connect", 1, SignalConnect),
				JS_CFUNC_DEF("Once", 1, SignalOnce),
				JS_CFUNC_DEF("Equals", 1, SignalEquals),
			};
			InstallClass<SignalPayload>(context, bound.SignalClass, "RBXScriptSignal", members, 3);
		}
		{
			static const JSCFunctionListEntry members[] = {
				JS_CFUNC_DEF("Disconnect", 0, ConnectionDisconnect),
				JS_CGETSET_DEF("Connected", ConnectionConnected, nullptr),
			};
			InstallClass<ConnectionId>(context, bound.ConnectionClass, "RBXScriptConnection", members, 2);
		}

		// --- task ---
		{
			JSValue table = JS_NewObject(context);
			JS_SetPropertyStr(context, table, "wait", JS_NewCFunction(context, TaskWait, "wait", 1));
			JS_SetPropertyStr(context, table, "spawn", JS_NewCFunction(context, TaskSpawn, "spawn", 1));
			JS_SetPropertyStr(context, table, "defer", JS_NewCFunction(context, TaskDefer, "defer", 1));
			JS_SetPropertyStr(context, table, "delay", JS_NewCFunction(context, TaskDelay, "delay", 2));
			JS_SetPropertyStr(context, table, "cancel", JS_NewCFunction(context, TaskCancel, "cancel", 1));
			JS_PreventExtensions(context, table);
			JS_SetPropertyStr(context, global, "task", table);
		}

		// --- the clock ---
		JS_SetPropertyStr(context, global, "time", JS_NewCFunction(context, ClockTime, "time", 0));
		JS_SetPropertyStr(
			context, global, "elapsedTime", JS_NewCFunction(context, ClockTime, "elapsedTime", 0)
		);
		JS_SetPropertyStr(context, global, "tick", JS_NewCFunction(context, ClockTick, "tick", 0));
		{
			JSValue table = JS_NewObject(context);
			JS_SetPropertyStr(context, table, "now", JS_NewCFunction(context, DateTimeNow, "now", 0));
			JS_SetPropertyStr(
				context,
				table,
				"fromSimulated",
				JS_NewCFunction(context, DateTimeFromSimulated, "fromSimulated", 0)
			);
			JS_SetPropertyStr(
				context,
				table,
				"fromUnixTimestamp",
				JS_NewCFunction(context, DateTimeFromUnix, "fromUnixTimestamp", 1)
			);
			JS_SetPropertyStr(context, global, "DateTime", table);
		}

		JS_SetPropertyStr(context, global, "typeOf", JS_NewCFunction(context, TypeOf, "typeOf", 1));
		JS_SetPropertyStr(context, global, "warn", JS_NewCFunction(context, Warn, "warn", 1));

		// --- RunService gains its predicates and a real Heartbeat signal ---
		{
			JSValue service = JS_GetPropertyStr(context, global, "RunService");
			if (JS_IsObject(service)) {
				JS_SetPropertyStr(
					context,
					service,
					"Heartbeat",
					MakeJsSignal(context, SignalKind::Heartbeat, ecs::NULL_ENTITY)
				);
				JS_SetPropertyStr(
					context, service, "IsServer", JS_NewCFunction(context, IsServer, "IsServer", 0)
				);
				JS_SetPropertyStr(
					context, service, "IsClient", JS_NewCFunction(context, IsClient, "IsClient", 0)
				);
				JS_SetPropertyStr(
					context, service, "IsStudio", JS_NewCFunction(context, IsStudio, "IsStudio", 0)
				);
				JS_SetPropertyStr(
					context, service, "IsReplica", JS_NewCFunction(context, IsReplica, "IsReplica", 0)
				);
			}
			JS_FreeValue(context, service);
		}

		InstallJsDatatypes(context, global);
		InstallJsServices(context, global);

		// After `InstallJsInstanceMethods`, which `OpenJsBindings` ran — this
		// adds the component half of the ECS surface to the table it built.
		InstallJsEcs(context, global);

		JS_FreeValue(context, global);
	}
}
