// Everything a JavaScript author reaches for once the property surface exists.
//
// **The second consumer for every binding, which is the point of having two
// VMs.** `JsBindings.cpp` is the property surface - `Instance.new`, accessors,
// `Enum`, `workspace` - and this is signals, the instance methods, `task`, the
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
// resolved by the host at a barrier is exactly the same contract -
// `docs/retired/SCRIPT_CONCURRENCY.md` §1's "a script may only resume from something
// the barrier delivers in a deterministic order". `JS_ExecutePendingJob` is what
// makes that true rather than aspirational: the host drives the microtask
// queue, so a reaction runs at a point the engine picked. A JS engine without
// that API would have had to be rejected on rule 5 alone.

#include "JsBindings.hpp"

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
#include <engine/scene/Characters.hpp>
#include <engine/script/Datatypes.hpp>
#include <engine/script/Subtree.hpp>
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
		// Three fields and no list - the connections live in `SignalTable`, so
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

			if (kind == SignalKind::PlayerAdded) {
				bound.World->ObserveTree();
				return;
			}

			const bool removing = kind == SignalKind::DescendantRemoving ||
								  kind == SignalKind::PlayerRemoving || kind == SignalKind::CharacterRemoving;
			if (!removing || bound.RemovingHooked) {
				return;
			}
			bound.RemovingHooked = true;

			ecs::Store *world = bound.World;
			bound.World->OnDescendantRemoving([context, world](Entity ancestor, Entity leaving) {
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

				// The Luau side's reason, unchanged: a character dies by being
				// destroyed, so the barrier would hand a handler a model it
				// cannot read a property off. `PlayerLosingCharacter` is what
				// makes this fire once rather than once per ancestor.
				if (const Entity losing = PlayerLosingCharacter(*world, ancestor, leaving);
					losing != ecs::NULL_ENTITY) {
					const std::string lost =
						FireJsSignal(context, SignalKind::CharacterRemoving, losing, 1, &subject);
					if (!lost.empty()) {
						ENGINE_WARN("[script] a CharacterRemoving handler failed: {}", lost);
					}
				}

				// The Luau side's reason, unchanged: `PlayerRemoving` rides
				// this hook because a game saving somebody's progress on the
				// way out needs the player still there to read.
				if (IsPlayerOfService(*world, ancestor, leaving)) {
					const std::string left =
						FireJsSignal(context, SignalKind::PlayerRemoving, ancestor, 1, &subject);
					if (!left.empty()) {
						ENGINE_WARN("[script] a PlayerRemoving handler failed: {}", left);
					}
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

		// --- the instance signals, and the methods that are no longer here ------
		//
		// **Twenty `JSCFunction`s stood here until v0.18 and every one of them had
		// a Luau twin.** They are `ScriptMethods.cpp`'s and `GuiMethods.cpp`'s
		// rows now, installed through `InstallJsNeutralMethods`, so what is left
		// in this file is the half a language genuinely decides: the signal
		// getters below, the codec bridge and the datatype vocabulary.
		//
		// A getter still checks its own receiver where a method no longer does -
		// `NeutralJsMethod` does that once for every row, and there is no
		// equivalent trampoline for a `JS_CGETSET_DEF`.

		Entity SelfEntity(JSContext *context, JSValueConst self) {
			return JsEntityOf(context, self);
		}

		// `instance.Changed` - a getter, because it takes no arguments and
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
		// machinery a connection uses, so `TaskQueue` - which knows nothing about
		// JavaScript - can name it.
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

		JSValue TaskWait(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			JsContext &bound = JsOf(context);
			const uint64_t ticks = TicksFor(*bound.World, Number(context, argc, argv, 0, 0.0));

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
			const uint64_t ticks = TicksFor(*bound.World, Number(context, argc, argv, 0, 0.0));

			const CallbackRef reference = Retain(context, argv[1]);
			bound.Tasks.Delay(reference, bound.World->Time().Tick + ticks);
			return JS_NewInt32(context, reference);
		}

		// `task.cancel(handle)` - the integer `task.delay` returned.
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

		// `typeOf(value)` - **not `typeof`**, which is a JavaScript keyword and
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
			// limit without hitting it first - so the refusal arrives, with a
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
			// cross as whatever the game uses to name things - rule 3, as a
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

	std::string FireJsSignal(
		JSContext *context, SignalKind kind, Entity subject, int count, JSValueConst *arguments, Name property
	) {
		JsContext &bound = JsOf(context);

		std::string firstError;
		std::vector<ConnectionId> spent;

		bound.Signals.Fire(kind, subject, [&](const Connection &connection) {
			// An invalid filter fires everything, which is what a kind with one
			// meaning wants; a valid one is a channel or a property name.
			if (property.IsValid() && connection.Property != property) {
				return;
			}

			JSValue result =
				JS_Call(context, Held(context, connection.Callback), JS_UNDEFINED, count, arguments);

			// **Every connection runs even when one throws**, and the first
			// error is what the host hears about. A handler that threw once
			// would otherwise silently stop everything registered after it, and
			// the symptom - half a scene animating - points nowhere near the
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
			// its own name, which is Roblox's split and the reason it exists -
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
		// leaves the store's list empty before the first one runs - so the move
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
		// than once per fire - `MakeJsInstance` mints an object per call, and
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

				// The Luau pump's reason, unchanged: a player joining *is* a
				// reparent, so this is that arrival filtered rather than a
				// second recording of the same fact.
				if (IsPlayerOfService(*bound.World, change.To, change.Instance)) {
					fire(SignalKind::PlayerAdded, change.To, change.Instance);
				}

				// `DescendantAdded` is every ancestor's, not just the new
				// parent's - that is the whole difference between it and
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

	std::string PumpJsChildWaiters(JSContext *context) {
		JsContext &bound = JsOf(context);
		if (bound.Waiters.Empty()) {
			return {};
		}

		std::vector<ChildWaiters::Resumption> ready;
		bound.Waiters.Advance(*bound.World, bound.World->Time().Tick, ready);

		std::string firstError;
		for (const ChildWaiters::Resumption &resumption : ready) {
			const auto waiting = bound.AwaitedChildren.find(resumption.Waiter);
			if (waiting == bound.AwaitedChildren.end()) {
				continue;
			}

			const CallbackRef resolver = waiting->second;
			bound.AwaitedChildren.erase(waiting);

			// **`null` and not `undefined` for a wait that ran out**, which is
			// what every other lookup on this surface answers with - see
			// `ReturnInstance`. A promise resolving to `undefined` would read as
			// one nobody gave a value to.
			JSValue child =
				resumption.Child == ecs::NULL_ENTITY ? JS_NULL : MakeJsInstance(context, resumption.Child);

			// **Taken off the context whether or not it is reported**, which is
			// what `ExceptionOf` does: a pending exception left there would
			// surface in whatever this VM did next, a long way from the resume
			// that threw.
			JSValue result = JS_Call(context, Held(context, resolver), JS_UNDEFINED, 1, &child);
			if (JS_IsException(result)) {
				const std::string message = ExceptionOf(context, "a resumed WaitForChild failed");
				if (firstError.empty()) {
					firstError = message;
				}
			}

			JS_FreeValue(context, result);
			JS_FreeValue(context, child);
			Release(context, resolver);
		}

		return firstError;
	}

	std::string PumpJsCharacters(JSContext *context) {
		JsContext &bound = JsOf(context);

		std::vector<scene::CharacterChange> changes;
		scene::TakeCharacterChanges(*bound.World, changes);
		if (changes.empty()) {
			return {};
		}

		std::string firstError;
		for (const scene::CharacterChange &change : changes) {
			// **A model that has gone is not reported here**, which is the Luau
			// pump's rule and the reason the two halves are disjoint: a
			// destroyed body already fired `CharacterRemoving` synchronously
			// with the instance still readable.
			if (!bound.World->Alive(change.Character)) {
				continue;
			}

			// Freed per fire, for `PumpJsTree`'s reason: `MakeJsInstance` mints
			// an object per call, and leaking one per respawn is a leak per
			// respawn.
			JSValue value = MakeJsInstance(context, change.Character);
			const std::string failed = FireJsSignal(
				context,
				change.Added ? SignalKind::CharacterAdded : SignalKind::CharacterRemoving,
				change.Player,
				1,
				&value
			);
			JS_FreeValue(context, value);

			if (firstError.empty() && !failed.empty()) {
				firstError = failed;
			}
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

			case gui::EventKind::Focused:
				note(FireJsSignal(context, SignalKind::GuiFocused, event.Instance, 0, nullptr));
				break;

			case gui::EventKind::DragBegan:
			case gui::EventKind::DragContinue:
			case gui::EventKind::DragEnded: {
				const SignalKind kind = event.Kind == gui::EventKind::DragBegan ? SignalKind::GuiDragBegan
										: event.Kind == gui::EventKind::DragContinue
											? SignalKind::GuiDragContinue
											: SignalKind::GuiDragEnded;
				JSValue arguments[4]{
					JS_NewFloat64(context, event.Position.X),
					JS_NewFloat64(context, event.Position.Y),
					JS_NewFloat64(context, event.Local.X),
					JS_NewFloat64(context, event.Local.Y),
				};
				note(FireJsSignal(context, kind, event.Instance, 4, arguments));
				break;
			}

			case gui::EventKind::FocusReleased: {
				// The Luau pump's `enterPressed`, off the event for the reason
				// `SignalKind::GuiFocusLost` gives.
				JSValue entered = JS_NewBool(context, event.Entered ? 1 : 0);
				note(FireJsSignal(context, SignalKind::GuiFocusLost, event.Instance, 1, &entered));
				JS_FreeValue(context, entered);
				break;
			}
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

		// Delayed work first, then deferred - the same order the Luau side
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
			JS_CGETSET_DEF("Changed", InstanceChanged, nullptr),
			JS_CGETSET_DEF("ChildAdded", InstanceTreeSignal<SignalKind::ChildAdded>, nullptr),
			JS_CGETSET_DEF("ChildRemoved", InstanceTreeSignal<SignalKind::ChildRemoved>, nullptr),
			JS_CGETSET_DEF("DescendantAdded", InstanceTreeSignal<SignalKind::DescendantAdded>, nullptr),
			JS_CGETSET_DEF("DescendantRemoving", InstanceTreeSignal<SignalKind::DescendantRemoving>, nullptr),
			JS_CGETSET_DEF("AncestryChanged", InstanceTreeSignal<SignalKind::AncestryChanged>, nullptr),
			JS_CGETSET_DEF("PlayerAdded", InstanceTreeSignal<SignalKind::PlayerAdded>, nullptr),
			JS_CGETSET_DEF("PlayerRemoving", InstanceTreeSignal<SignalKind::PlayerRemoving>, nullptr),
			JS_CGETSET_DEF("CharacterAdded", InstanceTreeSignal<SignalKind::CharacterAdded>, nullptr),
			JS_CGETSET_DEF("CharacterRemoving", InstanceTreeSignal<SignalKind::CharacterRemoving>, nullptr),

			// The 2D tree's input. Same template, because a gui signal is a
			// handle onto `SignalTable` exactly as a tree signal is - what
			// differs is only who records it, and that is the pump's business
			// rather than this getter's.
			JS_CGETSET_DEF("Activated", InstanceTreeSignal<SignalKind::GuiActivated>, nullptr),

			// **Roblox's second name for the same event, and one kind under
			// both.** `MouseButton1Click` is what a `GuiButton` carries there
			// and what most scripts connect to; this router produces exactly one
			// primary button, so the two questions have one answer. A second
			// `SignalKind` would be a second list for one event and whichever
			// name the pump did not know would never fire. `LuauInstances.cpp` says
			// the same from the Luau side, including why `InputChanged` is not
			// here.
			JS_CGETSET_DEF("MouseButton1Click", InstanceTreeSignal<SignalKind::GuiActivated>, nullptr),

			JS_CGETSET_DEF("InputBegan", InstanceTreeSignal<SignalKind::GuiInputBegan>, nullptr),
			JS_CGETSET_DEF("InputEnded", InstanceTreeSignal<SignalKind::GuiInputEnded>, nullptr),
			JS_CGETSET_DEF("MouseEnter", InstanceTreeSignal<SignalKind::GuiMouseEnter>, nullptr),
			JS_CGETSET_DEF("MouseLeave", InstanceTreeSignal<SignalKind::GuiMouseLeave>, nullptr),
			JS_CGETSET_DEF("MouseMoved", InstanceTreeSignal<SignalKind::GuiMouseMoved>, nullptr),

			// A `UIDragDetector`'s three. `LuauInstances.cpp` says why they sit
			// on every instance rather than on one class.
			JS_CGETSET_DEF("DragStart", InstanceTreeSignal<SignalKind::GuiDragBegan>, nullptr),
			JS_CGETSET_DEF("DragContinue", InstanceTreeSignal<SignalKind::GuiDragContinue>, nullptr),
			JS_CGETSET_DEF("DragEnd", InstanceTreeSignal<SignalKind::GuiDragEnded>, nullptr),

			// A `TextBox`'s pair. On every instance and inert anywhere else, for
			// the reason the six above are - `LuauInstances.cpp` says the same
			// from the other VM.
			JS_CGETSET_DEF("Focused", InstanceTreeSignal<SignalKind::GuiFocused>, nullptr),
			JS_CGETSET_DEF("FocusLost", InstanceTreeSignal<SignalKind::GuiFocusLost>, nullptr),
		};
		// **`std::size`, not a number somebody has to remember.** This read
		// `10` while the list held sixteen, so the last six - including
		// `IsDescendantOf`, `GetPropertyChangedSignal` and `Changed` - were
		// simply not installed. Nothing warned: a method that is not there
		// is `undefined`, and `undefined` only fails at the call site, in
		// whatever script reaches it first.
		//
		// The list is signals only now, and the scar is worth keeping because the
		// list can grow again: a getter added below without this being a `size_t`
		// of the array would fail exactly the same way.
		JS_SetPropertyFunctionList(context, methods, entries, static_cast<int>(std::size(entries)));

		// **Every method, written once for both languages.** There is no
		// hand-written JavaScript instance method left - the twenty that stood
		// beside this list until v0.18 are rows in `ScriptMethods.cpp` and
		// `GuiMethods.cpp` and land here through one trampoline. A row added there
		// is reachable from both VMs in the same commit, which is what the drift
		// above cost when it was two lists. See `ScriptCall.hpp`.
		InstallJsNeutralMethods(context, methods);

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

		InstallJsDatatypes(context, global);

		// **Before the services, because one of them produces it.** A bound
		// action's handler is handed an `InputObject`, and a class registered
		// after the service that hands one over would be a class id of zero at
		// the first press - the same ordering `LuauRuntime` keeps for the Luau
		// metatable, and `UserInputService`'s three input signals hand one over too.
		InstallJsInputObject(context);

		// **Beside it and for its reason**: `TweenService.Create` hands back a
		// `Tween`, and a class registered after the service that answers with one
		// would be a class id of zero at the first call.
		OpenJsTweenHandle(context);

		// **The services, from the catalogue.** `ServiceCatalogue.hpp` carries
		// the argument: which services exist is one fact and it was two lists,
		// so Luau bound nine and this language bound five with nothing in the
		// build to say so.
		//
		// **Here rather than in `OpenJsBindings`, because this is the later of
		// the two halves and every class a service hands back is registered by
		// now.** No service is installed anywhere else: since v0.16 every one of
		// them is a `ServiceSurface` the catalogue builds, and this walk is the
		// only place this language builds one.
		InstallJsServices(context, global, ServiceAvailability::Always, JsOf(context).Access);
		InstallJsServices(context, global, ServiceAvailability::Studio, JsOf(context).Access);

		// After `InstallJsInstanceMethods`, which `OpenJsBindings` ran - this
		// adds the component half of the ECS surface to the table it built.
		InstallJsEcs(context, global);

		JS_FreeValue(context, global);
	}
}
