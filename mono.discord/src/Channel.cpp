#include <discord/Channel.hpp>

namespace discord {

	const char *Describe(ChannelStatus status) {
		switch (status) {
		case ChannelStatus::Ok:
			return "ok";
		case ChannelStatus::Empty:
			return "empty";
		case ChannelStatus::Full:
			return "full";
		case ChannelStatus::Closed:
			return "closed";
		case ChannelStatus::TooLarge:
			return "frame too large";
		}
		// No default label, so adding a status is a warning here.
		return "?";
	}

	ChannelStatus MemoryChannel::Send(std::span<const std::byte> bytes) {
		if (!Live) {
			return ChannelStatus::Closed;
		}
		if (RefuseWrites) {
			return ChannelStatus::Full;
		}
		Written.insert(Written.end(), bytes.begin(), bytes.end());
		return ChannelStatus::Ok;
	}

	ChannelStatus MemoryChannel::Receive(std::vector<std::byte> &into) {
		if (Readable.empty()) {
			// Closed *and* drained, in that order, which is
			// `engine::parallel::LocalChannel`'s rule: a peer that hung up
			// should not strip this end of what it already said.
			return Live ? ChannelStatus::Empty : ChannelStatus::Closed;
		}
		into.insert(into.end(), Readable.begin(), Readable.end());
		Readable.clear();
		return ChannelStatus::Ok;
	}

	bool MemoryChannel::Open() const {
		return Live;
	}

	void MemoryChannel::Close() {
		Live = false;
	}
}
