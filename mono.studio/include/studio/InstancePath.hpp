#pragma once

// The one name for an instance that two editors can both read.
//
// **An `EditId` is one log's own name and cannot cross.** Two `CommandLog`s
// issue ids independently and both hand out `1` for their first instance, so
// the sender's id 5 and the receiver's id 5 are two different instances. A
// handle is worse: two stores allocate from zero, so the numbers coincide as a
// matter of course.
//
// What is left is the chain of names from the world's root, which is what a
// person already sees in the explorer and what both editors already agree on.
// `AGENTS.md` rule 4 read strictly: a name crosses a boundary and a number
// does not.
//
// **A list of names, not a joined string.** An instance may be called `a/b`,
// and a separator that can appear inside a name is a separator that eventually
// splits the wrong path.
//
// **Its own header because two things need it and neither is the other.** The
// edit stream names what an edit touched; the lock table names what somebody is
// holding. Putting the type in either would make the other include a stream or
// a lock table to say "an instance".
//
// @tier L12 · client

#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace studio {

	// An instance named the one way two editors can both read.
	//
	// The chain of names from the world's root down, so `{"Workspace", "Model",
	// "Part"}`. Empty names nothing.
	//
	// @since v0.13
	using InstancePath = std::vector<std::string>;

	// The path of an instance.
	//
	// @param store    The world it lives in.
	// @param instance The instance.
	// @return Its path, or empty for a null or dead handle.
	// @since v0.13
	InstancePath PathOf(const engine::ecs::Store &store, engine::ecs::Entity instance);

	// The instance a path names.
	//
	// **The first match wins and duplicates are the caller's problem.** Two
	// siblings may share a name - Roblox allows it and so does this store - so a
	// path is not a key. It is the best identity available without the document
	// format carrying one, and where it is ambiguous the ambiguity was already
	// there in what a person sees.
	//
	// @param store The world to look in.
	// @param path  The path.
	// @return The instance, or `NULL_ENTITY` when nothing there answers to it.
	// @since v0.13
	engine::ecs::Entity ResolvePath(const engine::ecs::Store &store, const InstancePath &path);

	// Whether one path is the other, or an ancestor of it.
	//
	// **The comparison a lock is made of.** A lock over a model has to cover
	// the parts inside it - moving a model moves its children - so "does this
	// edit touch that lock" is not equality, it is whether either path
	// contains the other.
	//
	// @param ancestor The shorter path, or an equal one.
	// @param path     The path to test.
	// @return `true` when `path` is `ancestor` or sits underneath it. An empty
	//         `ancestor` contains nothing, because a lock over nothing must not
	//         read as a lock over everything.
	// @since v0.13
	bool Contains(const InstancePath &ancestor, const InstancePath &path);

	// Whether two paths overlap in either direction.
	//
	// @param left  One path.
	// @param right The other.
	// @return `true` when either contains the other.
	// @since v0.13
	bool Overlaps(const InstancePath &left, const InstancePath &right);

	// A path as something a person can read - `Workspace.Model.Part`.
	//
	// **For a message and never for the wire.** Joining is lossy the moment a
	// name holds the separator, which is exactly why the wire carries the list.
	//
	// @param path The path.
	// @return The text, or `nothing` for an empty path.
	// @since v0.13
	std::string Describe(const InstancePath &path);
}
