#pragma once

#include <engine/script/Host.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace client::data_capture_driver {
	enum class Action : uint8_t { None, Release, Invalid };
	struct State {
		engine::script::HostCallback Callback;
		std::optional<uint64_t> Ticket;
		bool Cancelling = false;
		const void *Owner = nullptr;
	};

	inline bool Terminal(std::string_view status) {
		return status == "ready" || status == "partial" || status == "cancelled" || status == "error" ||
			   status == "failed" || status == "unsupported" || status == "invalid" ||
			   status == "stale_snapshot";
	}

	inline Action Transition(
		State &state,
		bool paused,
		std::optional<engine::script::HostCallback> callback,
		const engine::script::HostValue *result,
		const void *owner = nullptr
	) {
		if (!paused) {
			if (state.Ticket) state.Cancelling = true;
			return Action::None;
		}
		if (state.Cancelling) {
			if (result != nullptr && result->Tag == engine::script::HostTag::Map) {
				for (const auto &[name, value] : result->Entries)
					if (name == "status" && value.Tag == engine::script::HostTag::String &&
						Terminal(value.Text)) {
						return Action::Release;
					}
			}
			return Action::None;
		}
		if (!callback || !callback->Valid()) {
			if (state.Ticket) {
				state.Cancelling = true;
				return Action::None;
			}
			state = {};
			return Action::None;
		}
		if (!(state.Callback == *callback) || state.Owner != owner) {
			if (state.Ticket) {
				state.Cancelling = true;
				return Action::None;
			}
			const Action action = Action::None;
			state.Callback = *callback;
			state.Owner = owner;
			state.Ticket.reset();
			return action;
		}
		if (result == nullptr) return Action::None;
		auto invalid = [&] {
			if (state.Ticket) {
				state.Cancelling = true;
				return Action::None;
			}
			return Action::Invalid;
		};
		if (result->Tag != engine::script::HostTag::Map) return invalid();
		std::string_view status;
		std::string_view ticket;
		bool sawStatus = false;
		bool sawTicket = false;
		for (const auto &[name, value] : result->Entries) {
			if (name == "status") {
				if (sawStatus || value.Tag != engine::script::HostTag::String) return invalid();
				sawStatus = true;
				status = value.Text;
			} else if (name == "ticket") {
				if (sawTicket || value.Tag != engine::script::HostTag::String) return invalid();
				sawTicket = true;
				ticket = value.Text;
			}
		}
		if (status.empty()) return invalid();
		if (status == "queued") {
			if (state.Ticket) return invalid();
			uint64_t value = 0;
			for (const char character : ticket) {
				if (character < '0' || character > '9' || value > (UINT64_MAX - (character - '0')) / 10)
					return invalid();
				value = value * 10 + static_cast<uint64_t>(character - '0');
			}
			if (ticket.empty() || value == 0) return invalid();
			state.Ticket = value;
			return Action::None;
		}
		if (status == "pending") return state.Ticket ? Action::None : invalid();
		if (Terminal(status) && state.Ticket) {
			return Action::Release;
		}
		return invalid();
	}

	// Drives one bounded renderer cleanup turn. The product supplies bridge calls;
	// tests use the same sequence with a small fake bridge.
	template <class Cancel, class Pump, class Poll, class Release>
	bool AdvanceCancellation(
		State &state, bool &cancelSent, Cancel &&cancel, Pump &&pump, Poll &&poll, Release &&release
	) {
		if (!state.Cancelling || !state.Ticket) return false;
		const uint64_t ticket = *state.Ticket;
		if (!cancelSent) {
			cancel(ticket);
			cancelSent = true;
		}
		pump();
		if (!Terminal(poll(ticket)) || !release(ticket)) return false;
		state.Ticket.reset();
		state.Cancelling = false;
		cancelSent = false;
		return true;
	}
}
