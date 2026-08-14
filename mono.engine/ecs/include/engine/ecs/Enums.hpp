#pragma once

// The named states this layer talks in.
//
// Gathered rather than scattered because most of them cross a module boundary:
// a snapshot failure is reported to `world`, which reports it to a supervisor,
// which logs it. A `bool` at each of those hops loses the reason, and an `int`
// loses the type. Nothing here is an enum for the sake of having one - every
// value below is returned or accepted by something.
//
// **These names are a format, not a convenience.** Anything serialised or
// bound to script is written by *name*; the underlying numbers are free to move
// and must never be written to a file or a wire. That is the same rule
// `core::Name` carries, for the same reason.
//
// @tier L3 · shared

#include <cstdint>

namespace engine::ecs {

	// What a registered component type physically is.
	//
	// The distinction the storage cares about: a tag occupies no bytes, so its
	// column allocates nothing and its presence *is* its value.
	//
	// @since v0.2
	enum class ComponentKind : uint8_t {
		// Holds bytes. The ordinary case.
		Data,

		// Holds nothing. Matched by a query, never read.
		Tag,
	};

	// Why a snapshot could not be written or read.
	//
	// A `bool` was enough while the only caller was a test. Once a supervisor
	// restarts a world from a snapshot, "it failed" and "it failed because this
	// build does not have that component" are different operational problems
	// and only one of them is worth retrying.
	//
	// @since v0.2
	enum class SnapshotStatus : uint8_t {
		// Written or read in full.
		Ok,

		// The stream does not begin with a snapshot.
		NotASnapshot,

		// A snapshot, but from a format version this build does not read.
		WrongVersion,

		// Names a component this build has not registered. The world would
		// come back narrower than it was written, so it does not come back.
		UnknownComponent,

		// Ran out of bytes part-way. The store is left empty rather than
		// half-restored.
		Truncated,

		// A component in the world has no serialisation, so writing it would
		// produce bytes nothing can read back.
		Unserialisable,
	};

	// What applying a snapshot to a world that is already running should do
	// with entities the snapshot does not mention.
	//
	// The distinction a replica needs. A server sending full authoritative
	// state means "this is everything"; a server sending a region or a delta
	// means "this is what changed". Getting it backwards either resurrects
	// entities the server destroyed or destroys everything outside the part it
	// sent.
	//
	// @since v0.2
	enum class ApplyMode : uint8_t {
		// Entities the snapshot does not mention are left alone.
		//
		// For a partial update - a delta, or the slice of a world one client
		// can perceive.
		Overlay,

		// Entities the snapshot does not mention are destroyed.
		//
		// For full authoritative state: the sender is saying this is the whole
		// world, so anything else the receiver believes is stale.
		Authoritative,
	};

	// Returns a stable, human-readable name for an apply mode.
	//
	// @param mode The mode to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(ApplyMode mode);

	// Returns a stable, human-readable name for a snapshot status.
	//
	// For logs and for the diagnostic surfaces above this layer. Not a format:
	// nothing parses these back.
	//
	// @param status The status to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(SnapshotStatus status);

	// Returns a stable, human-readable name for a component kind.
	//
	// @param kind The kind to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(ComponentKind kind);
}
