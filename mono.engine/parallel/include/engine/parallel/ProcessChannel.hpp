#pragma once

// A channel with one end able to cross a process boundary.
//
// `Channel.hpp` is the queue, and a caller holding one cannot tell what carries
// the bytes. This is the other half of that promise: owning the operating
// system endpoint, handing it to a child, and picking it up again on the far
// side. Separate because it is a different job - a `Channel` is a queue, and an
// endpoint is a resource with an owner and a lifetime - and because most of the
// engine wants the queue and none of the resource.
//
// **Nothing here names a platform either.** An endpoint is an `int64_t` whose
// meaning belongs to the platform layer, the same rule `Process::Identifier`
// follows and for the same reason: `MonoLibrary.cmake` says the build is the
// only place that knows what platform this is.
//
// @tier L2 · shared

#include <engine/parallel/Channel.hpp>

#include <cstdint>
#include <memory>

namespace engine::parallel {

	// One end of a channel before anything has been sent on it.
	//
	// The thing a supervisor hands to `Process::Start` and then forgets. It
	// owns an operating system resource without naming one.
	//
	// Move-only. Two owners would both close it, and the second close would
	// land on whatever number the first one freed.
	//
	// @since v0.2
	class ChannelEnd {
	  public:
		// Creates a handle owning nothing.
		ChannelEnd() = default;

		// Closes the handle if it still owns one.
		~ChannelEnd();

		// An endpoint has one owner.
		ChannelEnd(const ChannelEnd &) = delete;

		// An endpoint has one owner.
		ChannelEnd &operator=(const ChannelEnd &) = delete;

		// Transfers ownership.
		ChannelEnd(ChannelEnd &&other) noexcept;

		// Transfers ownership, closing whatever this held.
		ChannelEnd &operator=(ChannelEnd &&other) noexcept;

		// Whether this handle owns an endpoint.
		//
		// @return `true` when there is something to hand over.
		bool Valid() const {
			return Value >= 0;
		}

		// Closes the endpoint.
		void Close();

		// The underlying handle, for the platform layer only.
		//
		// Public because the spawn code lives in a different translation unit
		// and a `friend` across a platform boundary is worse than a documented
		// accessor. Nothing outside `parallel` has any use for the number.
		//
		// @return The handle, or a negative value when there is none.
		int64_t Raw() const {
			return Value;
		}

		// Gives up ownership without closing.
		//
		// @return The handle, which the caller now owns.
		int64_t Release() {
			const int64_t held = Value;
			Value = -1;
			return held;
		}

		// Takes ownership of a platform handle. For the platform layer only.
		//
		// @param handle The handle to own.
		void Adopt(int64_t handle);

	  private:
		int64_t Value = -1;
	};

	// A channel with one end kept and the other ready to cross a process
	// boundary.
	//
	// @since v0.2
	struct ProcessChannel {
		// This process's end. Null when the pair could not be created.
		std::unique_ptr<Channel> Local;

		// The end to hand to a child, by passing it to `Process::Start`.
		ChannelEnd Remote;

		// Whether the pair was created.
		//
		// @return `true` when both ends exist.
		bool Valid() const {
			return Local != nullptr && Remote.Valid();
		}
	};

	// Creates a pair whose remote end survives being handed to a child process.
	//
	// Same interface, same framing, same refusal-instead-of-blocking as the
	// local one - the caller cannot tell them apart, which is what makes
	// thread-per-world and process-per-world one design rather than two.
	//
	// Costs more than `MakeLocalChannel` even when both ends stay here, because
	// every byte goes through the kernel. Use the local one unless an end is
	// actually leaving.
	//
	// @param settings How to size both directions.
	// @return The pair, or one with `Valid() == false` when the operating
	//         system refused.
	// @since v0.2
	ProcessChannel MakeProcessChannel(const ChannelSettings &settings = {});

	// Adopts the channel this process was started with.
	//
	// A child started by `Process::Start` with an endpoint receives it at a
	// fixed, inherited slot. Fixed rather than negotiated: a child has exactly
	// one channel to its driver, so there is nothing to negotiate, and a
	// constant means the command line does not have to carry a number that
	// only one layer understands.
	//
	// @param settings How to size both directions.
	// @return The channel, or null when this process was not started with one.
	// @since v0.2
	std::unique_ptr<Channel> AdoptInheritedChannel(const ChannelSettings &settings = {});

	// Whether this process was started with an inherited channel.
	//
	// Cheaper than adopting one and throwing it away, and it is what a program
	// deciding whether it is a host rather than a driver asks.
	//
	// @return `true` when `AdoptInheritedChannel` would succeed.
	// @since v0.2
	bool HasInheritedChannel();
}
