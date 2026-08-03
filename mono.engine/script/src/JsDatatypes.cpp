// The datatype vocabulary and the bus services, in JavaScript.
//
// The twin of `LuauDatatypes.cpp` and the second half of `Services.cpp`, split
// off from `JsSurface.cpp` for the reason that file was split from
// `JsBindings.cpp`: these are value types and a wire, and they are reviewed
// against `core/types/` and `world::Postbox` rather than against the class
// table.
//
// **Every type here is the same C++ value the Luau side wraps.** `Region3` is a
// `core::AABB` and `Ray` a `core::Ray`, because the engine already had both;
// `Random` is a counter over `core::Random`, because that generator is indexed
// rather than streamed. Two bindings over one set of types is the whole reason
// "identical bytes from both VMs" is achievable at all.

#include "JsBindings.hpp"

#include <engine/core/Random.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/core/types/NumberRange.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/core/types/Rect.hpp>
#include <engine/core/types/Sequence.hpp>
#include <engine/core/types/TweenInfo.hpp>
#include <engine/core/types/UDim.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/physics/Query.hpp>
#include <engine/spatial/CollisionGroups.hpp>
#include <engine/world/Postbox.hpp>

#include <string>
#include <vector>

namespace engine::script {

	namespace {
		using core::AABB;
		using core::ColorKeypoint;
		using core::ColorSequence;
		using core::EasingDirection;
		using core::EasingStyle;
		using core::NumberKeypoint;
		using core::NumberRange;
		using core::NumberSequence;
		using core::Ray;
		using core::Rect;
		using core::TweenInfo;
		using core::UDim;
		using core::UDim2;
		using core::Vector2;
		using world::BusKind;
		using world::BusStatus;
		using world::Postbox;
		using world::Ticket;

		double Arg(JSContext *context, int argc, JSValueConst *argv, int index, double fallback = 0.0) {
			if (index >= argc) {
				return fallback;
			}

			double value = fallback;
			if (JS_ToFloat64(context, &value, argv[index]) != 0) {
				return fallback;
			}
			return value;
		}

		float Real(JSContext *context, int argc, JSValueConst *argv, int index, double fallback = 0.0) {
			return static_cast<float>(Arg(context, argc, argv, index, fallback));
		}

		template <class T> JSValue Wrap(JSContext *context, JSClassID id, const T &value) {
			JSValue object = JS_NewObjectClass(context, static_cast<int>(id));
			if (JS_IsException(object)) {
				return object;
			}
			JS_SetOpaque(object, new T(value));
			return object;
		}

		template <class T> T *Unwrap(JSContext *context, JSValueConst value, JSClassID id) {
			auto *held = static_cast<T *>(JS_GetOpaque2(context, value, id));
			if (held == nullptr) {
				JS_FreeValue(context, JS_GetException(context));
			}
			return held;
		}

		template <class T> void Free(JSRuntime *, JSValue value) {
			JSClassID id = 0;
			delete static_cast<T *>(JS_GetAnyOpaque(value, &id));
		}

		// --- Vector2 ---------------------------------------------------------

		JSValue Vector2Get(JSContext *context, JSValueConst self, int magic) {
			const Vector2 *value = Unwrap<Vector2>(context, self, JsOf(context).Vector2Class);
			if (value == nullptr) {
				return JS_ThrowTypeError(context, "not a Vector2");
			}

			switch (magic) {
			case 0:
				return JS_NewFloat64(context, value->X);
			case 1:
				return JS_NewFloat64(context, value->Y);
			default:
				return JS_NewFloat64(context, value->Magnitude());
			}
		}

		JSValue Vector2Unit(JSContext *context, JSValueConst self) {
			const Vector2 *value = Unwrap<Vector2>(context, self, JsOf(context).Vector2Class);
			if (value == nullptr) {
				return JS_ThrowTypeError(context, "not a Vector2");
			}
			return Wrap(context, JsOf(context).Vector2Class, value->Unit());
		}

		// `a.add(b)`, `a.mul(2)` — **methods, because JavaScript has no operator
		// overloading**. Luau writes `a + b` because Luau can, and neither
		// language is made to pretend it is the other.
		JSValue Vector2Add(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			JsContext &bound = JsOf(context);
			const Vector2 *left = Unwrap<Vector2>(context, self, bound.Vector2Class);
			const Vector2 *right = argc > 0 ? Unwrap<Vector2>(context, argv[0], bound.Vector2Class) : nullptr;

			if (left == nullptr || right == nullptr) {
				return JS_ThrowTypeError(context, "add needs a Vector2");
			}
			return Wrap(context, bound.Vector2Class, *left + *right);
		}

		JSValue Vector2Sub(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			JsContext &bound = JsOf(context);
			const Vector2 *left = Unwrap<Vector2>(context, self, bound.Vector2Class);
			const Vector2 *right = argc > 0 ? Unwrap<Vector2>(context, argv[0], bound.Vector2Class) : nullptr;

			if (left == nullptr || right == nullptr) {
				return JS_ThrowTypeError(context, "sub needs a Vector2");
			}
			return Wrap(context, bound.Vector2Class, *left - *right);
		}

		JSValue Vector2Mul(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			JsContext &bound = JsOf(context);
			const Vector2 *left = Unwrap<Vector2>(context, self, bound.Vector2Class);
			if (left == nullptr || argc < 1) {
				return JS_ThrowTypeError(context, "mul needs a number or a Vector2");
			}

			if (const Vector2 *right = Unwrap<Vector2>(context, argv[0], bound.Vector2Class);
				right != nullptr) {
				return Wrap(context, bound.Vector2Class, *left * *right);
			}
			return Wrap(context, bound.Vector2Class, *left * Real(context, argc, argv, 0, 1.0));
		}

		JSValue Vector2Equals(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			JsContext &bound = JsOf(context);
			const Vector2 *left = Unwrap<Vector2>(context, self, bound.Vector2Class);
			const Vector2 *right = argc > 0 ? Unwrap<Vector2>(context, argv[0], bound.Vector2Class) : nullptr;

			return JS_NewBool(context, left != nullptr && right != nullptr && *left == *right);
		}

		JSValue Vector2New(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			return Wrap(
				context,
				JsOf(context).Vector2Class,
				Vector2{Real(context, argc, argv, 0), Real(context, argc, argv, 1)}
			);
		}

		// --- UDim and UDim2 --------------------------------------------------

		JSValue UDimGet(JSContext *context, JSValueConst self, int magic) {
			const UDim *value = Unwrap<UDim>(context, self, JsOf(context).UDimClass);
			if (value == nullptr) {
				return JS_ThrowTypeError(context, "not a UDim");
			}
			return JS_NewFloat64(context, magic == 0 ? value->Scale : value->Offset);
		}

		JSValue UDimNew(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			return Wrap(
				context,
				JsOf(context).UDimClass,
				UDim{Real(context, argc, argv, 0), Real(context, argc, argv, 1)}
			);
		}

		JSValue UDim2Get(JSContext *context, JSValueConst self, int magic) {
			JsContext &bound = JsOf(context);
			const UDim2 *value = Unwrap<UDim2>(context, self, bound.UDim2Class);
			if (value == nullptr) {
				return JS_ThrowTypeError(context, "not a UDim2");
			}
			return Wrap(context, bound.UDimClass, magic == 0 ? value->X : value->Y);
		}

		JSValue UDim2New(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			return Wrap(
				context,
				JsOf(context).UDim2Class,
				UDim2{
					Real(context, argc, argv, 0),
					Real(context, argc, argv, 1),
					Real(context, argc, argv, 2),
					Real(context, argc, argv, 3)
				}
			);
		}

		JSValue UDim2FromScale(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			return Wrap(
				context,
				JsOf(context).UDim2Class,
				UDim2{Real(context, argc, argv, 0), 0.0f, Real(context, argc, argv, 1), 0.0f}
			);
		}

		JSValue UDim2FromOffset(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			return Wrap(
				context,
				JsOf(context).UDim2Class,
				UDim2{0.0f, Real(context, argc, argv, 0), 0.0f, Real(context, argc, argv, 1)}
			);
		}

		// --- Rect ------------------------------------------------------------

		JSValue RectGet(JSContext *context, JSValueConst self, int magic) {
			JsContext &bound = JsOf(context);
			const Rect *value = Unwrap<Rect>(context, self, bound.RectClass);
			if (value == nullptr) {
				return JS_ThrowTypeError(context, "not a Rect");
			}

			switch (magic) {
			case 0:
				return Wrap(context, bound.Vector2Class, value->Min);
			case 1:
				return Wrap(context, bound.Vector2Class, value->Max);
			case 2:
				return JS_NewFloat64(context, value->Width());
			default:
				return JS_NewFloat64(context, value->Height());
			}
		}

		JSValue RectNew(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			JsContext &bound = JsOf(context);

			// Two corners or four numbers, both of which Roblox accepts.
			if (argc > 0) {
				if (const Vector2 *min = Unwrap<Vector2>(context, argv[0], bound.Vector2Class);
					min != nullptr && argc > 1) {
					if (const Vector2 *max = Unwrap<Vector2>(context, argv[1], bound.Vector2Class);
						max != nullptr) {
						return Wrap(context, bound.RectClass, Rect{*min, *max});
					}
				}
			}

			return Wrap(
				context,
				bound.RectClass,
				Rect{
					Real(context, argc, argv, 0),
					Real(context, argc, argv, 1),
					Real(context, argc, argv, 2),
					Real(context, argc, argv, 3)
				}
			);
		}

		// --- Region3 ---------------------------------------------------------

		JSValue Region3Get(JSContext *context, JSValueConst self, int magic) {
			const AABB *value = Unwrap<AABB>(context, self, JsOf(context).Region3Class);
			if (value == nullptr) {
				return JS_ThrowTypeError(context, "not a Region3");
			}

			if (magic == 0) {
				// The centre with no rotation. An axis-aligned box has none, and
				// Roblox's `Region3.CFrame` is the same identity-rotation frame.
				return MakeCFrame(context, core::CFrame{value->Centre()});
			}
			return MakeVector3(context, value->Size());
		}

		JSValue Region3New(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			if (argc < 2) {
				return JS_ThrowTypeError(context, "Region3.new needs two corners");
			}

			const core::Vector3 *min = AsVector3(context, argv[0]);
			const core::Vector3 *max = AsVector3(context, argv[1]);
			if (min == nullptr || max == nullptr) {
				return JS_ThrowTypeError(context, "Region3.new needs two Vector3s");
			}
			return Wrap(context, JsOf(context).Region3Class, AABB{*min, *max});
		}

		// --- NumberRange -----------------------------------------------------

		JSValue NumberRangeGet(JSContext *context, JSValueConst self, int magic) {
			const NumberRange *value = Unwrap<NumberRange>(context, self, JsOf(context).NumberRangeClass);
			if (value == nullptr) {
				return JS_ThrowTypeError(context, "not a NumberRange");
			}
			return JS_NewFloat64(context, magic == 0 ? value->Minimum : value->Maximum);
		}

		JSValue NumberRangeNew(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			const float minimum = Real(context, argc, argv, 0);
			const float maximum = argc > 1 ? Real(context, argc, argv, 1) : minimum;

			return Wrap(context, JsOf(context).NumberRangeClass, NumberRange{minimum, maximum});
		}

		// --- the sequences ---------------------------------------------------

		JSValue NumberSequenceEvaluate(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			const NumberSequence *value =
				Unwrap<NumberSequence>(context, self, JsOf(context).NumberSequenceClass);
			if (value == nullptr) {
				return JS_ThrowTypeError(context, "not a NumberSequence");
			}
			return JS_NewFloat64(context, value->Evaluate(Real(context, argc, argv, 0)));
		}

		JSValue NumberSequenceNew(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			NumberSequence built;

			if (argc > 0 && JS_IsArray(argv[0])) {
				JSValue lengthValue = JS_GetPropertyStr(context, argv[0], "length");
				uint32_t length = 0;
				JS_ToUint32(context, &length, lengthValue);
				JS_FreeValue(context, lengthValue);

				for (uint32_t index = 0; index < length; index++) {
					JSValue entry = JS_GetPropertyUint32(context, argv[0], index);
					JSValue time = JS_GetPropertyUint32(context, entry, 0);
					JSValue value = JS_GetPropertyUint32(context, entry, 1);

					double at = 0.0;
					double amount = 0.0;
					JS_ToFloat64(context, &at, time);
					JS_ToFloat64(context, &amount, value);

					JS_FreeValue(context, time);
					JS_FreeValue(context, value);
					JS_FreeValue(context, entry);

					// Refused rather than dropped, for the reason the Luau side
					// gives: a sequence silently missing its last stop is a
					// gradient subtly wrong everywhere and obviously wrong
					// nowhere.
					if (!built.Add(NumberKeypoint{static_cast<float>(at), static_cast<float>(amount)})) {
						return JS_ThrowTypeError(
							context, "NumberSequence: more than %u keypoints", core::SEQUENCE_CAPACITY
						);
					}
				}
			} else {
				const float from = Real(context, argc, argv, 0);
				built = argc > 1 ? NumberSequence{from, Real(context, argc, argv, 1)} : NumberSequence{from};
			}

			return Wrap(context, JsOf(context).NumberSequenceClass, built);
		}

		JSValue ColorSequenceEvaluate(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			const ColorSequence *value =
				Unwrap<ColorSequence>(context, self, JsOf(context).ColorSequenceClass);
			if (value == nullptr) {
				return JS_ThrowTypeError(context, "not a ColorSequence");
			}
			return MakeColor3(context, value->Evaluate(Real(context, argc, argv, 0)));
		}

		JSValue ColorSequenceNew(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			if (argc < 1) {
				return JS_ThrowTypeError(context, "ColorSequence.new needs a colour");
			}

			const core::Color3 *from = AsColor3(context, argv[0]);
			if (from == nullptr) {
				return JS_ThrowTypeError(context, "ColorSequence.new needs a Color3");
			}

			ColorSequence built{*from};
			if (argc > 1) {
				if (const core::Color3 *to = AsColor3(context, argv[1]); to != nullptr) {
					built = ColorSequence{*from, *to};
				}
			}
			return Wrap(context, JsOf(context).ColorSequenceClass, built);
		}

		// --- TweenInfo -------------------------------------------------------

		JSValue TweenInfoGet(JSContext *context, JSValueConst self, int magic) {
			const TweenInfo *value = Unwrap<TweenInfo>(context, self, JsOf(context).TweenInfoClass);
			if (value == nullptr) {
				return JS_ThrowTypeError(context, "not a TweenInfo");
			}

			switch (magic) {
			case 0:
				return JS_NewFloat64(context, value->Time);
			case 1:
				return JS_NewFloat64(context, value->DelayTime);
			case 2:
				return JS_NewInt32(context, value->RepeatCount);
			default:
				return JS_NewBool(context, value->Reverses);
			}
		}

		JSValue TweenInfoEvaluate(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			const TweenInfo *value = Unwrap<TweenInfo>(context, self, JsOf(context).TweenInfoClass);
			if (value == nullptr) {
				return JS_ThrowTypeError(context, "not a TweenInfo");
			}
			return JS_NewFloat64(context, value->Evaluate(Real(context, argc, argv, 0)));
		}

		JSValue TweenInfoNew(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			TweenInfo built;
			built.Time = static_cast<float>(Arg(context, argc, argv, 0, 1.0));

			core::Name style;
			if (argc > 1 && ReadJsEnumValue(context, argv[1], core::Name("EasingStyle"), style)) {
				built.Style = EasingStyleOf(style);
			}

			core::Name direction;
			if (argc > 2 && ReadJsEnumValue(context, argv[2], core::Name("EasingDirection"), direction)) {
				built.Direction = EasingDirectionOf(direction);
			}

			built.RepeatCount = static_cast<int32_t>(Arg(context, argc, argv, 3, 0.0));
			built.Reverses = argc > 4 && JS_ToBool(context, argv[4]) == 1;
			built.DelayTime = static_cast<float>(Arg(context, argc, argv, 5, 0.0));

			return Wrap(context, JsOf(context).TweenInfoClass, built);
		}

		// --- Ray -------------------------------------------------------------

		JSValue RayGet(JSContext *context, JSValueConst self, int magic) {
			const Ray *value = Unwrap<Ray>(context, self, JsOf(context).RayClass);
			if (value == nullptr) {
				return JS_ThrowTypeError(context, "not a Ray");
			}
			return MakeVector3(context, magic == 0 ? value->Origin : value->Direction);
		}

		JSValue RayPointAt(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			const Ray *value = Unwrap<Ray>(context, self, JsOf(context).RayClass);
			if (value == nullptr) {
				return JS_ThrowTypeError(context, "not a Ray");
			}
			return MakeVector3(context, value->PointAt(Real(context, argc, argv, 0)));
		}

		JSValue RayNew(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			if (argc < 2) {
				return JS_ThrowTypeError(context, "Ray.new needs an origin and a direction");
			}

			const core::Vector3 *origin = AsVector3(context, argv[0]);
			const core::Vector3 *direction = AsVector3(context, argv[1]);
			if (origin == nullptr || direction == nullptr) {
				return JS_ThrowTypeError(context, "Ray.new needs two Vector3s");
			}

			// **Normalised on the way in.** `core::Ray::Direction` must be unit
			// length and Roblox's need not be, so the conversion happens here
			// rather than leaving every query to report distances at the wrong
			// scale.
			return Wrap(context, JsOf(context).RayClass, Ray{*origin, direction->Unit()});
		}

		// --- Random ----------------------------------------------------------
		//
		// The counter over `core::Random` the Luau side uses, for the same
		// reason: that generator is indexed rather than streamed, so the seed is
		// the salt and the draw number is the index. A script's sequence is a
		// pure function of its seed and how many values it has taken.

		struct RandomStream {
			uint32_t Seed = 0;
			uint32_t Drawn = 0;
		};

		JSValue RandomNext(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			RandomStream *stream = Unwrap<RandomStream>(context, self, JsOf(context).RandomClass);
			if (stream == nullptr) {
				return JS_ThrowTypeError(context, "not a Random");
			}

			const float value = core::Random::Float(stream->Drawn++, stream->Seed);
			if (argc < 2) {
				return JS_NewFloat64(context, value);
			}

			const float minimum = Real(context, argc, argv, 0);
			const float maximum = Real(context, argc, argv, 1);
			return JS_NewFloat64(context, minimum + value * (maximum - minimum));
		}

		JSValue RandomNextInteger(JSContext *context, JSValueConst self, int argc, JSValueConst *argv) {
			RandomStream *stream = Unwrap<RandomStream>(context, self, JsOf(context).RandomClass);
			if (stream == nullptr || argc < 2) {
				return JS_ThrowTypeError(context, "NextInteger needs a minimum and a maximum");
			}

			const auto minimum = static_cast<int64_t>(Arg(context, argc, argv, 0));
			const auto maximum = static_cast<int64_t>(Arg(context, argc, argv, 1));
			if (maximum < minimum) {
				return JS_ThrowTypeError(context, "NextInteger: the maximum is below the minimum");
			}

			// Inclusive of both ends, which is Roblox's contract. `Float` is
			// half-open, so the span is `max - min + 1`.
			const uint64_t span = static_cast<uint64_t>(maximum - minimum) + 1;
			const float value = core::Random::Float(stream->Drawn++, stream->Seed);
			const auto offset = static_cast<uint64_t>(static_cast<double>(value) * static_cast<double>(span));

			return JS_NewInt64(context, minimum + static_cast<int64_t>(offset < span ? offset : span - 1));
		}

		JSValue RandomNew(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			RandomStream stream;
			stream.Seed = static_cast<uint32_t>(Arg(context, argc, argv, 0, 0.0));
			return Wrap(context, JsOf(context).RandomClass, stream);
		}

		// --- the store services ----------------------------------------------
		//
		// **The calls a script genuinely suspends on.** A `Get` returns a
		// `Ticket`; the reply lands in the inbox at a later tick, applied sorted
		// at the barrier — which is §1's *first* legal resume source. The
		// suspension is a promise here rather than a coroutine, and the host
		// resolves it in `PumpJsDeliveries`.

		JSValue AwaitTicket(JSContext *context, Ticket ticket, const char *what) {
			if (!ticket.Expected()) {
				return JS_ThrowTypeError(context, "%s: over this world's budget", what);
			}

			JsContext &bound = JsOf(context);

			JSValue settle[2];
			JSValue promise = JS_NewPromiseCapability(context, settle);
			if (JS_IsException(promise)) {
				return promise;
			}

			// `insert_or_assign` for the reason the Luau side gives: a chained
			// call reuses the ticket slot and the stale resolver would leak.
			bound.AwaitedTickets.insert_or_assign(ticket.Value, Retain(context, settle[0]));
			JS_FreeValue(context, settle[0]);
			JS_FreeValue(context, settle[1]);
			return promise;
		}

		// Encodes an argument, throwing a named error on refusal.
		bool EncodeArgument(JSContext *context, JSValueConst value, std::vector<std::byte> &out) {
			ScriptValue tree;
			CodecStatus why = CodecStatus::Ok;

			if (!ToScriptValue(context, value, tree, 0, why)) {
				JS_ThrowTypeError(context, "the value cannot cross a world boundary: %s", Describe(why));
				return false;
			}

			if (const CodecStatus status = Encode(tree, out); status != CodecStatus::Ok) {
				JS_ThrowTypeError(context, "the value cannot cross a world boundary: %s", Describe(status));
				return false;
			}
			return true;
		}

		JSValue StoreGet(JSContext *context, JSValueConst, int argc, JSValueConst *argv, int magic) {
			if (argc < 1) {
				return JS_ThrowTypeError(context, "GetAsync needs a key");
			}

			const char *key = JS_ToCString(context, argv[0]);
			if (key == nullptr) {
				return JS_EXCEPTION;
			}

			Postbox box(*JsOf(context).World);
			JSValue promise = AwaitTicket(
				context, box.Get(magic == 0 ? BusKind::MemoryStore : BusKind::DataStore, key), "GetAsync"
			);
			JS_FreeCString(context, key);
			return promise;
		}

		JSValue StoreSet(JSContext *context, JSValueConst, int argc, JSValueConst *argv, int magic) {
			if (argc < 2) {
				return JS_ThrowTypeError(context, "SetAsync needs a key and a value");
			}

			const char *key = JS_ToCString(context, argv[0]);
			if (key == nullptr) {
				return JS_EXCEPTION;
			}

			std::vector<std::byte> payload;
			if (!EncodeArgument(context, argv[1], payload)) {
				JS_FreeCString(context, key);
				return JS_EXCEPTION;
			}

			Postbox box(*JsOf(context).World);

			// Both flags, for the reason `Services.cpp` gives: the bus's own
			// `Replica` resource and the store's adopt-only flag are the same
			// fact in two places, and checking one is checking half.
			if (box.IsReplica() || JsOf(context).World->AdoptOnly()) {
				JS_FreeCString(context, key);
				return JS_ThrowTypeError(
					context,
					"SetAsync: this world is a replica and its store writes are refused. Test "
					"RunService.IsReplica() first"
				);
			}

			JSValue promise = AwaitTicket(
				context,
				box.Set(magic == 0 ? BusKind::MemoryStore : BusKind::DataStore, key, payload),
				"SetAsync"
			);
			JS_FreeCString(context, key);
			return promise;
		}

		JSValue StoreRemove(JSContext *context, JSValueConst, int argc, JSValueConst *argv, int magic) {
			if (argc < 1) {
				return JS_ThrowTypeError(context, "RemoveAsync needs a key");
			}

			const char *key = JS_ToCString(context, argv[0]);
			if (key == nullptr) {
				return JS_EXCEPTION;
			}

			Postbox box(*JsOf(context).World);
			JSValue promise = AwaitTicket(
				context,
				box.Remove(magic == 0 ? BusKind::MemoryStore : BusKind::DataStore, key),
				"RemoveAsync"
			);
			JS_FreeCString(context, key);
			return promise;
		}

		// The compare-and-swap, which is the cross-world lock.
		//
		// §4: a lock in the shape an author expects cannot exist here, because
		// rule 3 leaves no shared memory to guard. What they actually want is
		// this — the version the caller read goes in, and `Conflict` comes back
		// when it has moved on.
		JSValue MemoryStoreUpdate(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			if (argc < 3) {
				return JS_ThrowTypeError(context, "UpdateAsync needs a key, a version and a value");
			}

			const char *key = JS_ToCString(context, argv[0]);
			if (key == nullptr) {
				return JS_EXCEPTION;
			}

			double version = 0.0;
			JS_ToFloat64(context, &version, argv[1]);

			std::vector<std::byte> payload;
			if (!EncodeArgument(context, argv[2], payload)) {
				JS_FreeCString(context, key);
				return JS_EXCEPTION;
			}

			Postbox box(*JsOf(context).World);
			JSValue promise =
				AwaitTicket(context, box.Update(key, static_cast<uint64_t>(version), payload), "UpdateAsync");
			JS_FreeCString(context, key);
			return promise;
		}

		// --- raycasting ------------------------------------------------------
		//
		// The JavaScript twin of `LuauQuery.cpp`, against the same
		// `physics::Raycast` and the same exact shapes. `RaycastParams` filters
		// on a **collision group** rather than on a list of instances, for the
		// reason that file gives: the engine has a bit test where Roblox's
		// `FilterDescendantsInstances` would be a per-ray walk of a subtree.

		struct RaycastFilter {
			spatial::LayerMask Mask = spatial::LayerMask::All();
		};

		JSValue RaycastParamsNew(JSContext *context, JSValueConst, int, JSValueConst *) {
			return Wrap(context, JsOf(context).RaycastParamsClass, RaycastFilter{});
		}

		JSValue RaycastParamsGet(JSContext *context, JSValueConst self) {
			const RaycastFilter *filter =
				Unwrap<RaycastFilter>(context, self, JsOf(context).RaycastParamsClass);
			if (filter == nullptr) {
				return JS_ThrowTypeError(context, "not a RaycastParams");
			}

			for (uint32_t index = 0; index < spatial::LayerMask::LAYER_COUNT; index++) {
				if ((filter->Mask.Bits & (1u << index)) != 0) {
					return JS_NewString(context, spatial::CollisionGroups::NameOf(index).Text().data());
				}
			}
			return JS_NewString(context, "");
		}

		JSValue RaycastParamsSet(JSContext *context, JSValueConst self, JSValueConst value) {
			RaycastFilter *filter = Unwrap<RaycastFilter>(context, self, JsOf(context).RaycastParamsClass);
			if (filter == nullptr) {
				return JS_ThrowTypeError(context, "not a RaycastParams");
			}

			const char *group = JS_ToCString(context, value);
			if (group == nullptr) {
				return JS_EXCEPTION;
			}

			const uint32_t index = spatial::CollisionGroups::IndexOf(core::Name(group));
			if (index == spatial::NO_GROUP) {
				// Refused rather than defaulted to everything: a typo that
				// quietly widened a filter is a ray that hits what it was told
				// to ignore.
				JSValue error = JS_ThrowTypeError(context, "'%s' is not a registered collision group", group);
				JS_FreeCString(context, group);
				return error;
			}

			JS_FreeCString(context, group);
			filter->Mask = spatial::CollisionGroups::MaskFor(index);
			return JS_UNDEFINED;
		}

		// `workspace.Raycast(origin, direction, params)`
		//
		// The **direction carries the distance**, as Roblox's does:
		// `Raycast(origin, direction.mul(500))` is what an author writes, and
		// `core::Ray` needs a unit direction — so the split happens here rather
		// than being lost.
		JSValue WorkspaceRaycast(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			if (argc < 2) {
				return JS_ThrowTypeError(context, "Raycast needs an origin and a direction");
			}

			const core::Vector3 *origin = AsVector3(context, argv[0]);
			const core::Vector3 *travel = AsVector3(context, argv[1]);
			if (origin == nullptr || travel == nullptr) {
				return JS_ThrowTypeError(context, "Raycast needs two Vector3s");
			}

			const float distance = travel->Magnitude();
			if (distance <= 0.0f) {
				return JS_NULL;
			}

			spatial::LayerMask mask = spatial::LayerMask::All();
			if (argc > 2 && !JS_IsNull(argv[2]) && !JS_IsUndefined(argv[2])) {
				if (const RaycastFilter *filter =
						Unwrap<RaycastFilter>(context, argv[2], JsOf(context).RaycastParamsClass);
					filter != nullptr) {
					mask = filter->Mask;
				}
			}

			JsContext &bound = JsOf(context);
			const auto hit =
				physics::Raycast(*bound.World, core::Ray{*origin, *travel / distance}, distance, mask);

			// **Null, not a result carrying a flag.** A flag makes reading the
			// position out of a miss compile and produce a plausible number.
			if (!hit.has_value()) {
				return JS_NULL;
			}

			JSValue result = JS_NewObject(context);
			JS_SetPropertyStr(context, result, "Instance", MakeJsInstance(context, hit->Owner));
			JS_SetPropertyStr(context, result, "Position", MakeVector3(context, hit->Position));
			JS_SetPropertyStr(context, result, "Normal", MakeVector3(context, hit->Normal));
			JS_SetPropertyStr(context, result, "Distance", JS_NewFloat64(context, hit->Distance));
			JS_PreventExtensions(context, result);
			return result;
		}

		template <class T>
		void Install(
			JSContext *context,
			JSValueConst global,
			JSClassID &id,
			const char *name,
			const JSCFunctionListEntry *members,
			int count,
			const JSCFunctionListEntry *constructors,
			int constructorCount
		) {
			static JSClassDef definition = {name, Free<T>, nullptr, nullptr, nullptr};
			definition.class_name = name;

			JSRuntime *runtime = JS_GetRuntime(context);
			JS_NewClassID(runtime, &id);
			JS_NewClass(runtime, id, &definition);

			JSValue proto = JS_NewObject(context);
			if (count > 0) {
				JS_SetPropertyFunctionList(context, proto, members, count);
			}
			JS_SetClassProto(context, id, proto);

			JSValue table = JS_NewObject(context);
			JS_SetPropertyFunctionList(context, table, constructors, constructorCount);
			JS_PreventExtensions(context, table);
			JS_SetPropertyStr(context, global, name, table);
		}
	}

	void InstallJsDatatypes(JSContext *context, JSValueConst global) {
		JsContext &bound = JsOf(context);

		{
			static const JSCFunctionListEntry members[] = {
				JS_CGETSET_MAGIC_DEF("X", Vector2Get, nullptr, 0),
				JS_CGETSET_MAGIC_DEF("Y", Vector2Get, nullptr, 1),
				JS_CGETSET_MAGIC_DEF("Magnitude", Vector2Get, nullptr, 2),
				JS_CGETSET_DEF("Unit", Vector2Unit, nullptr),
				JS_CFUNC_DEF("add", 1, Vector2Add),
				JS_CFUNC_DEF("sub", 1, Vector2Sub),
				JS_CFUNC_DEF("mul", 1, Vector2Mul),
				JS_CFUNC_DEF("Equals", 1, Vector2Equals),
			};
			static const JSCFunctionListEntry constructors[] = {JS_CFUNC_DEF("new", 2, Vector2New)};
			Install<Vector2>(context, global, bound.Vector2Class, "Vector2", members, 8, constructors, 1);
		}
		{
			static const JSCFunctionListEntry members[] = {
				JS_CGETSET_MAGIC_DEF("Scale", UDimGet, nullptr, 0),
				JS_CGETSET_MAGIC_DEF("Offset", UDimGet, nullptr, 1),
			};
			static const JSCFunctionListEntry constructors[] = {JS_CFUNC_DEF("new", 2, UDimNew)};
			Install<UDim>(context, global, bound.UDimClass, "UDim", members, 2, constructors, 1);
		}
		{
			static const JSCFunctionListEntry members[] = {
				JS_CGETSET_MAGIC_DEF("X", UDim2Get, nullptr, 0),
				JS_CGETSET_MAGIC_DEF("Y", UDim2Get, nullptr, 1),
			};
			static const JSCFunctionListEntry constructors[] = {
				JS_CFUNC_DEF("new", 4, UDim2New),
				JS_CFUNC_DEF("fromScale", 2, UDim2FromScale),
				JS_CFUNC_DEF("fromOffset", 2, UDim2FromOffset),
			};
			Install<UDim2>(context, global, bound.UDim2Class, "UDim2", members, 2, constructors, 3);
		}
		{
			static const JSCFunctionListEntry members[] = {
				JS_CGETSET_MAGIC_DEF("Min", RectGet, nullptr, 0),
				JS_CGETSET_MAGIC_DEF("Max", RectGet, nullptr, 1),
				JS_CGETSET_MAGIC_DEF("Width", RectGet, nullptr, 2),
				JS_CGETSET_MAGIC_DEF("Height", RectGet, nullptr, 3),
			};
			static const JSCFunctionListEntry constructors[] = {JS_CFUNC_DEF("new", 4, RectNew)};
			Install<Rect>(context, global, bound.RectClass, "Rect", members, 4, constructors, 1);
		}
		{
			static const JSCFunctionListEntry members[] = {
				JS_CGETSET_MAGIC_DEF("CFrame", Region3Get, nullptr, 0),
				JS_CGETSET_MAGIC_DEF("Size", Region3Get, nullptr, 1),
			};
			static const JSCFunctionListEntry constructors[] = {JS_CFUNC_DEF("new", 2, Region3New)};
			Install<AABB>(context, global, bound.Region3Class, "Region3", members, 2, constructors, 1);
		}
		{
			static const JSCFunctionListEntry members[] = {
				JS_CGETSET_MAGIC_DEF("Min", NumberRangeGet, nullptr, 0),
				JS_CGETSET_MAGIC_DEF("Max", NumberRangeGet, nullptr, 1),
			};
			static const JSCFunctionListEntry constructors[] = {JS_CFUNC_DEF("new", 2, NumberRangeNew)};
			Install<NumberRange>(
				context, global, bound.NumberRangeClass, "NumberRange", members, 2, constructors, 1
			);
		}
		{
			static const JSCFunctionListEntry members[] = {
				JS_CFUNC_DEF("Evaluate", 1, NumberSequenceEvaluate)
			};
			static const JSCFunctionListEntry constructors[] = {JS_CFUNC_DEF("new", 2, NumberSequenceNew)};
			Install<NumberSequence>(
				context, global, bound.NumberSequenceClass, "NumberSequence", members, 1, constructors, 1
			);
		}
		{
			static const JSCFunctionListEntry members[] = {
				JS_CFUNC_DEF("Evaluate", 1, ColorSequenceEvaluate)
			};
			static const JSCFunctionListEntry constructors[] = {JS_CFUNC_DEF("new", 2, ColorSequenceNew)};
			Install<ColorSequence>(
				context, global, bound.ColorSequenceClass, "ColorSequence", members, 1, constructors, 1
			);
		}
		{
			static const JSCFunctionListEntry members[] = {
				JS_CGETSET_MAGIC_DEF("Time", TweenInfoGet, nullptr, 0),
				JS_CGETSET_MAGIC_DEF("DelayTime", TweenInfoGet, nullptr, 1),
				JS_CGETSET_MAGIC_DEF("RepeatCount", TweenInfoGet, nullptr, 2),
				JS_CGETSET_MAGIC_DEF("Reverses", TweenInfoGet, nullptr, 3),
				JS_CFUNC_DEF("Evaluate", 1, TweenInfoEvaluate),
			};
			static const JSCFunctionListEntry constructors[] = {JS_CFUNC_DEF("new", 6, TweenInfoNew)};
			Install<TweenInfo>(
				context, global, bound.TweenInfoClass, "TweenInfo", members, 5, constructors, 1
			);
		}
		{
			static const JSCFunctionListEntry members[] = {
				JS_CGETSET_MAGIC_DEF("Origin", RayGet, nullptr, 0),
				JS_CGETSET_MAGIC_DEF("Direction", RayGet, nullptr, 1),
				JS_CFUNC_DEF("PointAt", 1, RayPointAt),
			};
			static const JSCFunctionListEntry constructors[] = {JS_CFUNC_DEF("new", 2, RayNew)};
			Install<Ray>(context, global, bound.RayClass, "Ray", members, 3, constructors, 1);
		}
		{
			static const JSCFunctionListEntry members[] = {
				JS_CFUNC_DEF("NextNumber", 2, RandomNext),
				JS_CFUNC_DEF("NextInteger", 2, RandomNextInteger),
			};
			static const JSCFunctionListEntry constructors[] = {JS_CFUNC_DEF("new", 1, RandomNew)};
			Install<RandomStream>(context, global, bound.RandomClass, "Random", members, 2, constructors, 1);
		}
	}

	void InstallJsQueries(JSContext *context, JSValueConst global, JSValueConst workspace) {
		JsContext &bound = JsOf(context);

		{
			static const JSCFunctionListEntry members[] = {
				JS_CGETSET_DEF("CollisionGroup", RaycastParamsGet, RaycastParamsSet),
			};
			static const JSCFunctionListEntry constructors[] = {JS_CFUNC_DEF("new", 0, RaycastParamsNew)};
			Install<RaycastFilter>(
				context, global, bound.RaycastParamsClass, "RaycastParams", members, 1, constructors, 1
			);
		}

		// On the world, because `workspace.Raycast` is where Roblox puts it — a
		// query is against a world and not against a part.
		JS_SetPropertyStr(
			context, workspace, "Raycast", JS_NewCFunction(context, WorkspaceRaycast, "Raycast", 3)
		);
	}

	void InstallJsServices(JSContext *context, JSValueConst global) {
		{
			static const JSCFunctionListEntry members[] = {
				JS_CFUNC_MAGIC_DEF("GetAsync", 1, StoreGet, 0),
				JS_CFUNC_MAGIC_DEF("SetAsync", 2, StoreSet, 0),
				JS_CFUNC_MAGIC_DEF("RemoveAsync", 1, StoreRemove, 0),
				JS_CFUNC_DEF("UpdateAsync", 3, MemoryStoreUpdate),
			};
			JSValue service = JS_NewObject(context);
			JS_SetPropertyFunctionList(context, service, members, 4);
			JS_PreventExtensions(context, service);
			JS_SetPropertyStr(context, global, "MemoryStoreService", service);
		}
		{
			static const JSCFunctionListEntry members[] = {
				JS_CFUNC_MAGIC_DEF("GetAsync", 1, StoreGet, 1),
				JS_CFUNC_MAGIC_DEF("SetAsync", 2, StoreSet, 1),
				JS_CFUNC_MAGIC_DEF("RemoveAsync", 1, StoreRemove, 1),
			};
			JSValue service = JS_NewObject(context);
			JS_SetPropertyFunctionList(context, service, members, 3);
			JS_PreventExtensions(context, service);
			JS_SetPropertyStr(context, global, "DataStoreService", service);
		}
	}
}
