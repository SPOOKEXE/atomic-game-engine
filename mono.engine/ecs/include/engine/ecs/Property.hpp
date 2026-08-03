#pragma once

// Declaring a property, as opposed to reading one.
//
// `Classes.hpp` describes what a property *is* and is what a consumer includes.
// This header holds the one piece that cannot live there: the template that
// generates a plain field's conversion, which calls `Store` and therefore needs
// `Store.hpp` — a header that sits above `Classes.hpp` and does not include it.
//
// The split is the whole reason this file exists, so it is worth stating rather
// than leaving to be inferred: **include this to declare, include `Classes.hpp`
// to read.** A module that only reads properties — the bindings, the manifest
// generator, an editor — does not pull `Store.hpp` in behind them.
//
// @tier L3 · shared

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>

#include <cstdint>
#include <string_view>

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
}
