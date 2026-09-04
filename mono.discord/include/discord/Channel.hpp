#pragma once

// The byte pipe under the Discord connection, and the two ways to get one.
//
// **A frame in, a frame out, and it never blocks.** Everything above this
// treats a failed write as a dropped statement rather than as a lost message,
// which is what `Activity.hpp` earns by being level-triggered. So there is no
// queue here, no capacity to tune and no thread.
//
// ## Why this is not `engine::parallel::Channel`
//
// That one has the right shape and the wrong job. It builds socket *pairs* for
// process-per-world hosts and hands one end to a child; there is no way to ask
// it to connect to a path that already exists, and adding one would turn a
// module about job workers into a module about IPC endpoints. The hundred lines
// of non-blocking socket setup are written again here rather than shared, and
// that is the cheaper of the two debts.
//
// @tier shared
// @since v0.17
//
// arch-waiver public-header: part of the published surface through
// `discord/Link.hpp`, which includes this header. A `Link` owns its byte
// pipe, so the channel type travels with the connection type.

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace discord {

	// Why a read or a write did not do what was asked.
	//
	// @since v0.17
	enum class ChannelStatus : uint8_t {
		// It worked.
		Ok = 0,

		// Nothing has arrived. Not an error; the ordinary answer when polling.
		Empty = 1,

		// The far end is not draining. The caller drops what it was saying.
		Full = 2,

		// The pipe is gone. Every call after this answers the same.
		Closed = 3,

		// The frame is larger than the protocol allows and was not sent.
		TooLarge = 4,
	};

	// A stable, human-readable name for a status.
	//
	// @param status Which one.
	// @return A view valid for the lifetime of the process.
	// @since v0.17
	const char *Describe(ChannelStatus status);

	// One end of a byte pipe.
	//
	// Bytes, not frames. Framing is `Frame.hpp`'s job, and it is kept apart
	// because a local socket delivers whatever it feels like per read - a whole
	// frame nine times out of ten, and half of one on the tenth.
	//
	// @since v0.17
	class Channel {
	  public:
		virtual ~Channel() = default;

		// Writes the whole span, or none of it, or gives up on the pipe.
		//
		// **A framed stream cannot resynchronise**, so there are only three
		// honest outcomes. Nothing was taken and the caller drops what it was
		// saying: `Full`. Everything was taken: `Ok`. Something was taken and
		// the rest would not go, which leaves half a frame on the wire: the
		// channel closes itself and answers `Closed`, because a reconnect is
		// recoverable and a desynchronised reader is not.
		//
		// @param bytes What to write.
		// @return `Ok`, `Full` or `Closed`.
		virtual ChannelStatus Send(std::span<const std::byte> bytes) = 0;

		// Appends whatever has arrived to `into`.
		//
		// Appends rather than replaces, because the caller is accumulating
		// until a whole frame is there.
		//
		// @param into Where to put it.
		// @return `Ok` when something was read, `Empty` when nothing had
		//         arrived, `Closed` when the far end went away.
		virtual ChannelStatus Receive(std::vector<std::byte> &into) = 0;

		// Whether this end is still usable.
		virtual bool Open() const = 0;

		// Closes it. Safe to call more than once.
		virtual void Close() = 0;
	};

	// Connects to whichever local Discord socket answers first.
	//
	// **A missing socket is `nullptr` and not a fault.** Discord not running is
	// the ordinary state of a headless origin, and every caller here treats it
	// as "try again later" rather than as something to report.
	//
	// @param socketOverride An exact path or pipe name to use instead of
	//        searching. For tests, and for an install nothing here knows about.
	// @return The channel, or `nullptr` when nothing answered.
	// @since v0.17
	std::unique_ptr<Channel> ConnectLocal(std::string_view socketOverride = {});

	// Every place a Discord socket is looked for, in the order tried.
	//
	// Exposed so a suite can assert the search rather than infer it from
	// whether a connection happened, and so the studio can say where it looked
	// when it found nothing.
	//
	// @return The candidate paths, longest-shot last.
	// @since v0.17
	std::vector<std::string> SocketCandidates();

	// A channel with no socket under it, for tests.
	//
	// Two ends, like `engine::parallel::MakeLocalChannel`: what the link writes
	// arrives at the far end, and what the far end writes is what the link
	// reads. `Full` and `Closed` are both reachable on demand, because the
	// failure paths are the ones worth testing and the real socket will not
	// produce them to order.
	//
	// @since v0.17
	class MemoryChannel final : public Channel {
	  public:
		ChannelStatus Send(std::span<const std::byte> bytes) override;
		ChannelStatus Receive(std::vector<std::byte> &into) override;
		bool Open() const override;
		void Close() override;

		// What the link has written, in order.
		std::vector<std::byte> Written;

		// What the link will read next. Drained by `Receive`.
		std::vector<std::byte> Readable;

		// Makes the next `Send` answer `Full` rather than writing.
		bool RefuseWrites = false;

	  private:
		bool Live = true;
	};
}
