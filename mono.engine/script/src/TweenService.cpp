// `TweenService`, in neither language.
//
// **The service is neutral and the handle it answers with is not.**
// `TweenTable` holds what a tween is and decides every order a recording depends
// on; `GetValue` and `Create` are `ScriptMethod`s, so both VMs install the same
// two rows; and the `Tween` object those rows hand back is a tagged userdata in
// `LuauTween.cpp` and a registered class in `JsTween.cpp`. That last split is
// deliberate rather than unfinished - `Tweens.hpp` and `ScriptCall::ReturnTween`
// carry the argument, and the short version is that `Play` is a name the neutral
// instance table would take from every part in the world.
//
// **`GetValue` is the piece worth having first**, and it is why the easing maths
// live in `core::TweenInfo` rather than here: a curve inside a service is
// checked by watching something move, and a pure function of an alpha is checked
// by asserting that `Quad`/`Out` at a half is three quarters.
//
// **The goal map is what needed a record reader.** A goal's value is a `UDim2`,
// a `Rect` or a `ColorSequence`, and `ScriptValue` has no tag for any of them
// and must not gain one - so `ScriptCall::ReadFieldNames` and
// `ReadFieldProperty` read the record by *name and declared type* instead, and
// the policy below - what may be tweened and what the refusal says - is written
// once for both languages rather than twice with two spellings.
//
// @tier L9 · shared

#include <engine/ecs/Classes.hpp>
#include <engine/script/ScriptCall.hpp>
#include <engine/script/ServiceSurface.hpp>
#include <engine/script/Tweens.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::script {

	namespace {
		using ecs::Entity;
		using ecs::PropertyDescriptor;

		// Where the goal record sits in `Create`'s argument list.
		//
		// Named because `ReadGoals` reads the same argument twice - once for the
		// names and once per value - and two literal `2`s three lines apart is
		// exactly the pair that gets edited singly.
		constexpr size_t GOALS = 2;

		// The property a script named, or null when the instance has no such
		// scriptable property.
		//
		// The same rule `LuauInstances.cpp` states for the read path: a
		// non-scriptable property is *not found* rather than found and refused,
		// so the error message does not tell a program what is there to reach
		// for.
		const PropertyDescriptor *FindGoal(const ecs::Store &store, Entity instance, std::string_view name) {
			for (const PropertyDescriptor &property : store.PropertiesOf(instance)) {
				if (property.Spelling == name) {
					return property.Scriptable ? &property : nullptr;
				}
			}
			return nullptr;
		}

		// Reads the goal record, refusing anything a tween cannot drive **by
		// name**.
		//
		// **Every refusal names the property**, because the alternative is a
		// tween that runs for its whole duration and moves nothing - which reads
		// as a broken engine rather than as a scene asking for something that
		// does not mean anything. A `Bool` has no midpoint, `Anchored` is a
		// `Bool`, and saying so is the difference between a minute and an
		// afternoon.
		//
		// @param call   The call, whose argument two is the record.
		// @param store  The world.
		// @param target What the tween drives.
		// @return The goals, sorted by property name.
		std::vector<TweenGoal> ReadGoals(ScriptCall &call, ecs::Store &store, Entity target) {
			std::vector<std::string> names;
			call.ReadFieldNames(GOALS, names);

			std::vector<TweenGoal> goals;
			goals.reserve(names.size());

			for (const std::string &name : names) {
				const PropertyDescriptor *property = FindGoal(store, target, name);
				if (property == nullptr) {
					call.Raise(
						("TweenService:Create: '" + name + "' is not a property of this instance").c_str()
					);
				}
				if (!property->Writable) {
					call.Raise(("TweenService:Create: '" + name + "' cannot be assigned").c_str());
				}
				if (!Interpolable(property->Type)) {
					call.Raise(("TweenService:Create: '" + name + "' is a " + ecs::Describe(property->Type) +
								", which has no midpoint to interpolate through")
								   .c_str());
				}

				TweenGoal goal;
				goal.Property = property->Name;
				goal.Type = property->Type;
				goal.Size = property->Size;

				if (!call.ReadFieldProperty(GOALS, name, property->Type, property->EnumName, goal.Goal)) {
					call.Raise(("TweenService:Create: could not read the goal for '" + name + "'").c_str());
				}

				goals.push_back(goal);
			}

			// **Sorted by spelling, which is what makes two goals a stated
			// order.** A Luau table is walked in hash order and a JavaScript
			// object in insertion order, and two properties of one instance may
			// project onto one component - `Position` and `CFrame` both write
			// `Transform` - so which of them lands last is observable and must
			// not depend on which language asked.
			std::sort(goals.begin(), goals.end(), [](const TweenGoal &left, const TweenGoal &right) {
				return left.Property.Text() < right.Property.Text();
			});
			return goals;
		}

		// `TweenService:GetValue(alpha, easingStyle, easingDirection)`
		//
		// **Pure, and the only part of this service a scene can use without
		// building anything.** A layout that wants a curve without a target - an
		// emitter's rate, a camera's own easing, a value written into an
		// attribute - reaches for this rather than making a tween to read.
		void GetValue(ScriptCall &call) {
			const double alpha = call.AsNumber(0);

			core::Name style;
			if (!call.ReadEnum(1, core::Name("EasingStyle"), style)) {
				call.Raise("TweenService:GetValue: expected an Enum.EasingStyle");
			}

			core::Name direction;
			if (!call.ReadEnum(2, core::Name("EasingDirection"), direction)) {
				call.Raise("TweenService:GetValue: expected an Enum.EasingDirection");
			}

			// Clamped by `Ease` itself rather than here - past the end a tween is
			// finished, and an elastic curve extrapolated past one grows without
			// bound.
			call.ReturnNumber(
				core::TweenInfo::Ease(
					static_cast<float>(alpha), EasingStyleOf(style), EasingDirectionOf(direction)
				)
			);
		}

		// `TweenService:Create(instance, tweenInfo, goals)` -> Tween
		void Create(ScriptCall &call) {
			ecs::Store &store = call.World();

			const Entity target = call.AsInstance(0);
			const core::TweenInfo info = call.AsTweenInfo(1);
			std::vector<TweenGoal> goals = ReadGoals(call, store, target);

			std::vector<Entity> dropped;
			const Entity tween = call.Tweens().Create(store, target, info, std::move(goals), dropped);

			// **The connections go before the row does.** Only a VM knows what a
			// `CallbackRef` means, which is why the release is a request on the
			// interface - see `ScriptCall::ForgetSubject`.
			for (const Entity stale : dropped) {
				call.ForgetSubject(stale);
				store.Destroy(stale);
			}

			// **The cap is named in the refusal**, because the alternative is a
			// tween that was never made answering `Play` with silence. See
			// `TweenTable::MAXIMUM` for why a table of live tweens refuses where
			// `Debris` evicts.
			if (tween == ecs::NULL_ENTITY) {
				call.Raise(("TweenService:Create: this world already holds " +
							std::to_string(TweenTable::MAXIMUM) + " running tweens")
							   .c_str());
			}

			call.ReturnTween(tween);
		}

		constexpr std::array<ServiceMethod, 2> TWEEN_METHODS{{
			{"GetValue", GetValue},
			{"Create", Create},
		}};
	}

	const ServiceSurface &TweenServiceSurface() {
		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "TweenService";
			surface.Methods = TWEEN_METHODS;
			return surface;
		}();
		return SURFACE;
	}
}
