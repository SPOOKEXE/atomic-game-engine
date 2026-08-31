#pragma once

// The Roblox-shaped instance boundary shared by every script VM.
//
// A script instance is still exactly one ECS entity. This shim owns no rows,
// caches or mirrored property values: it only applies the scripting policy
// before forwarding an operation to the world's `ecs::Store`. Keeping that
// policy here means Luau and JavaScript cannot disagree about which properties
// exist, how creation fails, or whether a failed parent leaves an unreachable
// orphan behind.
//
// @tier L9 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

namespace engine::script {

	// Why an `Instance.new` operation did not produce an instance.
	//
	// The VM adapter turns this neutral result into its language's error shape.
	// No exception or VM value crosses this boundary.
	enum class InstanceCreateFailure {
		None,
		UnknownClass,
		StoreRefused,
		ParentRefused,
	};

	// The result of creating and optionally parenting one script instance.
	struct InstanceCreateResult {
		ecs::Entity Instance = ecs::NULL_ENTITY;
		InstanceCreateFailure Failure = InstanceCreateFailure::None;

		explicit operator bool() const {
			return Instance != ecs::NULL_ENTITY && Failure == InstanceCreateFailure::None;
		}
	};

	// Every property of an instance that a script may discover.
	//
	// This allocates once while JavaScript builds a class prototype. The hot
	// read and write paths use `ScriptableProperty` below and allocate nothing.
	std::vector<const ecs::PropertyDescriptor *>
	ScriptableProperties(const ecs::Store &store, ecs::Entity instance);

	// One instance's property of a name, or null when a script may not see it.
	//
	// A non-scriptable property is not found. Otherwise its name alone would
	// disclose private authoring state such as a script's source path.
	const ecs::PropertyDescriptor *
	ScriptableProperty(const ecs::Store &store, ecs::Entity instance, std::string_view name);

	// Reads a descriptor obtained from this shim.
	bool ReadInstanceProperty(
		const ecs::Store &store,
		ecs::Entity instance,
		const ecs::PropertyDescriptor &property,
		void *value,
		size_t bytes
	);

	// Writes a writable descriptor obtained from this shim.
	bool WriteInstanceProperty(
		ecs::Store &store,
		ecs::Entity instance,
		const ecs::PropertyDescriptor &property,
		const void *value,
		size_t bytes
	);

	// Finds a direct child through the shared instance tree.
	ecs::Entity FindInstanceChild(const ecs::Store &store, ecs::Entity instance, std::string_view name);

	// Reports whether the handle still names a row in this world.
	bool InstanceAlive(const ecs::Store &store, ecs::Entity instance);

	// The class the instance projects, or an invalid id for a classless ECS row.
	ecs::ClassId InstanceClassOf(const ecs::Store &store, ecs::Entity instance);

	// Reports whether the instance projects a class or one derived from it.
	bool InstanceIsA(const ecs::Store &store, ecs::Entity instance, ecs::ClassId wanted);

	// The instance's authored name.
	core::Name InstanceNameOf(const ecs::Store &store, ecs::Entity instance);

	// The direct parent, or the null handle for a root.
	ecs::Entity InstanceParentOf(const ecs::Store &store, ecs::Entity instance);

	// Creates one instance and optionally parents it as one operation.
	//
	// A refused parent destroys the newly created orphan before returning. VM
	// adapters cannot retain its handle after raising an error, so leaving it in
	// the world would be invisible state.
	InstanceCreateResult CreateScriptInstance(
		ecs::Store &store, std::string_view className, ecs::Entity parent = ecs::NULL_ENTITY
	);
}
