// Owning one end of a channel, without knowing what kind of end it is.
//
// `ChannelEnd` is a handle number, a single owner and a close. None of that
// differs between platforms once `platform::SocketClose` exists, so it lives
// here rather than once per platform — the platform files are left with the
// part that genuinely differs, which is how a connected pair is made and how a
// child finds the end it was given.

#include "platform/Socket.hpp"

#include <engine/parallel/ProcessChannel.hpp>

namespace engine::parallel {

	ChannelEnd::~ChannelEnd() {
		Close();
	}

	ChannelEnd::ChannelEnd(ChannelEnd &&other) noexcept : Value(other.Value) {
		other.Value = -1;
	}

	ChannelEnd &ChannelEnd::operator=(ChannelEnd &&other) noexcept {
		if (this == &other) {
			return *this;
		}
		Close();
		Value = other.Value;
		other.Value = -1;
		return *this;
	}

	void ChannelEnd::Close() {
		if (Value >= 0) {
			platform::SocketClose(Value);
			Value = -1;
		}
	}

	void ChannelEnd::Adopt(int64_t handle) {
		Close();
		Value = handle;
	}
}
