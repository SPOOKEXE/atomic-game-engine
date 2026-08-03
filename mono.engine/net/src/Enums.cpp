#include <engine/net/Enums.hpp>

namespace engine::net {

	const char *Describe(ConnectionState state) {
		switch (state) {
		case ConnectionState::Connecting:
			return "connecting";
		case ConnectionState::Connected:
			return "connected";
		case ConnectionState::Disconnecting:
			return "disconnecting";
		case ConnectionState::Disconnected:
			return "disconnected";
		}
		// No default label, so adding a state is a compiler warning here rather
		// than a log line reading "?" that nobody traces back.
		return "?";
	}

	const char *Describe(DisconnectReason reason) {
		switch (reason) {
		case DisconnectReason::None:
			return "none";
		case DisconnectReason::Requested:
			return "requested";
		case DisconnectReason::TimedOut:
			return "timed out";
		case DisconnectReason::HandshakeFailed:
			return "handshake failed";
		case DisconnectReason::ProtocolError:
			return "protocol error";
		case DisconnectReason::BudgetExceeded:
			return "budget exceeded";
		case DisconnectReason::Shutdown:
			return "shutdown";
		}
		return "?";
	}

	const char *Describe(ChannelKind kind) {
		switch (kind) {
		case ChannelKind::Unreliable:
			return "unreliable";
		case ChannelKind::Reliable:
			return "reliable";
		case ChannelKind::Handshake:
			return "handshake";
		}
		return "?";
	}
}
