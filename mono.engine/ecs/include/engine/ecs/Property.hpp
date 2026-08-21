#pragma once

// Declaring a property, as opposed to reading one.
//
// `Classes.hpp` describes what a property *is* and is what a consumer includes.
// This header holds the one piece that cannot live there: the template that
// generates a plain field's conversion, which calls `Store` and therefore needs
// `Store.hpp` - a header that sits above `Classes.hpp` and does not include it.
//
// The split is the whole reason this file exists, so it is worth stating rather
// than leaving to be inferred: **include this to declare, include `Classes.hpp`
// to read.** A module that only reads properties - the bindings, the manifest
// generator, an editor - does not pull `Store.hpp` in behind them.
//
// @tier L3 · shared

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace engine::ecs {

	template <auto Member> void Classes::Property(ClassId owner, std::string_view name) {
		using Component = typename MemberOf<decltype(Member)>::Class;
		using Value = typename MemberOf<decltype(Member)>::Type;

		PropertyDescriptor descriptor;
		descriptor.Name = core::Name(name);
		descriptor.Type = TypeOf<Value>();
		descriptor.Size = static_cast<uint32_t>(sizeof(Value));
		descriptor.Kind = PropertyKind::Field;
		descriptor.Reads = &ComponentSet::Intern({Components::Of<Component>()});
		descriptor.Writes = descriptor.Reads;

		descriptor.Get = [](const Store &store, Entity instance, void *out) -> bool {
			const Component *component = store.Get<Component>(instance);
			if (component == nullptr) {
				return false;
			}
			*static_cast<Value *>(out) = component->*Member;
			return true;
		};

		// `GetMutable` rather than a raw pointer, and that is load-bearing:
		// handing out a mutable pointer is already counted as a write, so the
		// column is marked changed and `replication` sees a script's edit. A
		// setter that reached the bytes another way would be a silent desync.
		descriptor.Set = [](Store &store, Entity instance, const void *value) -> bool {
			Component *component = store.GetMutable<Component>(instance);
			if (component == nullptr) {
				return false;
			}
			component->*Member = *static_cast<const Value *>(value);
			return true;
		};

		Declare(owner, descriptor);
	}

	template <auto Member, Bound Low, Bound High>
	void Classes::ClampedProperty(ClassId owner, std::string_view name) {
		using Component = typename MemberOf<decltype(Member)>::Class;
		using Value = typename MemberOf<decltype(Member)>::Type;

		// **`Bound` carries a float, so the member has to be one.** This replaces
		// the older check that the bounds were spelled in the member's own type,
		// and it buys the same thing: a clamp against a range that is not the one
		// written at the call is the failure worth refusing, and a member of some
		// other type reaching this would get one through a conversion nobody
		// wrote. A clamped integer property would need its own bound type, and
		// there has never been one.
		static_assert(
			std::is_same_v<Value, float>,
			"ClampedProperty's bounds are floats - a member of another type needs a bound type of "
			"its own rather than a conversion nobody wrote"
		);
		static_assert(
			Low.Value() <= High.Value(), "the bounds are the wrong way round, so every write would be refused"
		);

		PropertyDescriptor descriptor;
		descriptor.Name = core::Name(name);
		descriptor.Type = TypeOf<Value>();
		descriptor.Size = static_cast<uint32_t>(sizeof(Value));
		descriptor.Kind = PropertyKind::Field;
		descriptor.Reads = &ComponentSet::Intern({Components::Of<Component>()});
		descriptor.Writes = descriptor.Reads;

		descriptor.Get = [](const Store &store, Entity instance, void *out) -> bool {
			const Component *component = store.Get<Component>(instance);
			if (component == nullptr) {
				return false;
			}
			*static_cast<Value *>(out) = component->*Member;
			return true;
		};

		// Captureless, like `Property`'s, so it converts to the descriptor's
		// function pointer. `GetMutable` rather than a raw pointer for that
		// function's reason: handing out a mutable pointer is already counted as
		// a write, so a script's edit reaches replication.
		descriptor.Set = [](Store &store, Entity instance, const void *value) -> bool {
			Component *component = store.GetMutable<Component>(instance);
			if (component == nullptr) {
				return false;
			}
			component->*Member = std::clamp(*static_cast<const Value *>(value), Low.Value(), High.Value());
			return true;
		};

		Declare(owner, descriptor);
	}
}
