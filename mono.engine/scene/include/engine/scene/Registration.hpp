#pragma once

// Where this module's types get their names.
//
// Called once, during single-threaded startup, before any world is built or any
// snapshot is read. Two separate reasons, and both of them bite:
//
// - **Registration order decides iteration order.** Component ids are a dense
//   counter, an archetype is a sorted list of them, and archetypes are iterated
//   in id order - so two runs that register in different orders visit rows in
//   different orders and a floating-point sum over those rows diverges.
//   `ecs::Components::Seal` is what pins it, and everything has to be
//   registered before it closes.
// - **A name crosses and an id does not.** These types are registered under
//   explicit names rather than the compiler's spelling of them, because a
//   recording written by one build is read by another and
//   `engine::scene::Transform` as GCC happens to spell it is not a promise
//   anybody made. `ecs::TypeDescriptor` says the same thing from the other end.
//
// Both entry points are idempotent, so a program that registers, tears a
// universe down and builds another does not accumulate anything.
//
// @tier L7 · shared

namespace engine::scene {

	// Registers this module's components and resources under explicit names.
	//
	// Every type in `Components.hpp`, plus the `SurfaceTable` and
	// `ActiveCamera` resources - a resource is keyed by a component id too, so
	// `Store::SetResource` on an unregistered type would try to mint one, and
	// after `Seal` that aborts.
	//
	// Anything holding a `core::Name` is registered with an explicit writer and
	// reader that write the name as **text**. The raw object representation
	// would write the name's process-local id, which restores in another
	// process as whatever string happened to take that number.
	void RegisterSceneComponents();

	// Registers the `Instance`/`PVInstance`/`BasePart`/`Part` class tree.
	//
	// Calls `RegisterSceneComponents` first, because a class is a set of
	// component ids and cannot be declared before they exist. A caller that
	// only wants the components may call that one alone; a caller that wants
	// classes cannot get the order wrong.
	void RegisterSceneClasses();
}
