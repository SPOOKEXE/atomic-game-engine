#pragma once

// The fetching half: submit a request, pump, take a response.
//
// **Submit / pump / take / cancel, because that is already the shape of every
// asynchronous thing in this engine.** `cdn::Origin` answers those four calls
// and v0.2's buses answer them for messages. A fetch that returned a future
// would be a fifth way to wait for something, and the one property that makes
// this usable from a world is the one a future does not have: **the completion
// becomes visible when the caller pumps, and at no other moment.**
//
// That is the boundary an origin lives on. The origin may finish a transfer
// whenever it
// likes; a world applies the result at the barrier, because a chunk that becomes
// visible to a system mid-tick is a desync - two machines whose networks
// happened to differ would simulate different things. `AGENTS.md` rule 5 with no
// exception, and this class is what makes obeying it the easy path.
//
// **One connection per outstanding fetch, and that is deliberate.** The point
// is N groups streaming concurrently so a slow group does not hold up the
// others and a dropped connection re-fetches one group rather than restarting
// the run. Multiplexing them onto one socket would reintroduce exactly the
// head-of-line blocking that arrangement exists to avoid - HTTP/2 or /3 stays
// open as a future answer to the same question.
//
// **Every byte that arrives is hostile.** An origin is something anyone can
// run, so a response is bounded before it is buffered, and
// what it decompresses to is bounded by the *signed manifest* rather than by
// anything the origin said. That second check is the delivery client's;
// this one bounds the transfer.
//
// @tier L11 · shared

#include <engine/net/Endpoint.hpp>
#include <engine/net/http/Message.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace engine::net::http {

	// How a client is sized and bounded.
	//
	// @since v0.9
	struct ClientSettings {
		// The bounds every response parse is held to.
		MessageLimits Limits;

		// How many fetches may be in flight.
		//
		// A submit past this is refused with an invalid handle rather than
		// queued: a queue here would hide the fact that the caller is asking
		// for more than it configured for, and the symptom would be latency
		// nobody can attribute.
		size_t MaximumOutstanding = 16;

		// How many polls a fetch may go without progress before it fails.
		//
		// Counted in polls rather than wall time - `net/AGENTS.md`'s rule that
		// time is passed in rather than read, which is what lets the suite
		// state a timeout instead of sleeping for one. Zero disables it.
		uint32_t IdlePolls = 0;
	};

	// A fetch in flight.
	//
	// A number rather than a pointer, for `AGENTS.md` rule 3's reason: this is
	// exactly the sort of handle that ends up crossing a boundary.
	//
	// @since v0.9
	struct FetchId {
		// The value meaning "no fetch". Zero is never issued.
		static constexpr uint64_t NONE = 0;

		// The client's monotonic counter.
		uint64_t Value = NONE;

		// Whether this names a fetch at all.
		constexpr bool IsValid() const {
			return Value != NONE;
		}

		// Whether two handles name the same fetch.
		constexpr bool operator==(const FetchId &other) const = default;
	};

	// Where a fetch has got to.
	//
	// @since v0.9
	enum class FetchState : uint8_t {
		// Not a fetch this client issued, or one already taken.
		Unknown,

		// Connecting, sending or reading.
		Pending,

		// A whole response arrived and is waiting to be taken.
		Ready,

		// Abandoned by the caller.
		Cancelled,

		// The connection failed, the peer answered something that is not a
		// message, or the idle bound was reached.
		//
		// **One state rather than several.** A caller retries or falls through
		// to the next source either way, and a taxonomy of network failures at
		// this layer is a taxonomy every caller then has to switch on.
		Failed,
	};

	// Returns a stable, human-readable name for a fetch state.
	//
	// @param state The state to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(FetchState state);

	// Issues requests and collects responses, without blocking.
	//
	// **One owner, one thread.**
	//
	// @since v0.9
	class Client {
	  public:
		virtual ~Client() = default;

		// Starts a fetch.
		//
		// @param to Where to send it.
		// @param request What to ask for. `Host` is written from `host`.
		// @param host The `Host` header's value - the name the origin is known
		//        by, which is not always the address it is reached at.
		// @return A handle, or an invalid one when `MaximumOutstanding` is
		//         reached or the endpoint cannot be addressed.
		virtual FetchId Submit(const Endpoint &to, const Request &request, std::string_view host) = 0;

		// Where a fetch has got to.
		//
		// @param id The fetch.
		// @return Its state, or `Unknown` for a handle this client did not
		//         issue or has already handed the result of.
		virtual FetchState StateOf(FetchId id) const = 0;

		// Drives every outstanding fetch, without blocking.
		//
		// @return How many fetches reached `Ready` or `Failed`.
		virtual size_t Pump() = 0;

		// Takes a ready fetch's response.
		//
		// The fetch is finished by this call, so a second take answers nothing.
		//
		// @param id The fetch.
		// @return The response, or nothing when the fetch is not `Ready`.
		virtual std::optional<Response> Take(FetchId id) = 0;

		// Abandons a fetch and closes its connection.
		//
		// **Cancellation is load-bearing, not a convenience** - the absence of
		// it is what produces a game that hitches every time a player turns
		// around, which is the sentence `cdn::Origin::Cancel` already carries.
		//
		// @param id The fetch to abandon.
		// @return False for an unknown fetch or one already finished.
		virtual bool Cancel(FetchId id) = 0;

		// How many fetches have been submitted and not yet taken.
		virtual size_t Outstanding() const = 0;

		// How many body bytes have arrived over this client's life.
		//
		// The measurement that answers "did this actually travel compressed" -
		// which is a question about the wire, so it is counted at the wire.
		virtual uint64_t ReceivedBytes() const = 0;
	};

	// Builds a client.
	//
	// @param settings How to size and bound it.
	// @return The client. Never null: nothing is bound or connected until a
	//         fetch is submitted, so there is no failure to report yet.
	// @since v0.9
	std::unique_ptr<Client> MakeClient(const ClientSettings &settings = {});
}
