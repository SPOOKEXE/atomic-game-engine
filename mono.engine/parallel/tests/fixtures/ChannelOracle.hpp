#pragma once

// Owned byte patterns and independently checked FIFO/capacity transitions.
// A separate pair keeps this preflight out of the evolving measured queues.
#include <engine/parallel/Channel.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace channel_fixture {
	inline void Require(bool condition) {
		if (!condition) throw std::runtime_error("local channel ownership or frame oracle failed");
	}
	inline std::byte Byte(uint32_t sequence, size_t index) {
		return static_cast<std::byte>((sequence * 37u + index * 13u + (index >> 8u)) & 255u);
	}
	inline std::vector<std::byte> Pattern(uint32_t sequence, size_t bytes) {
		std::vector<std::byte> result(bytes);
		for (size_t index = 0; index < bytes; ++index)
			result[index] = Byte(sequence, index);
		return result;
	}
	inline void Check(const std::vector<std::byte> &frame, uint32_t sequence, size_t bytes) {
		Require(frame.size() == bytes);
		for (size_t index = 0; index < bytes; ++index)
			Require(frame[index] == Byte(sequence, index));
	}
	inline void Verify() {
		using namespace engine::parallel;
		auto [left, right] = MakeLocalChannel();
		constexpr std::array<size_t, 7> sizes{64, 262144, 0, 4096, 64, 262144, 4096};
		size_t queued = 0;
		for (size_t sequence = 0; sequence < sizes.size(); ++sequence) {
			auto source = Pattern(static_cast<uint32_t>(sequence), sizes[sequence]);
			Require(left->Send(source) == ChannelStatus::Ok);
			// Send must own a copy, including after its source storage is released.
			std::fill(source.begin(), source.end(), std::byte{0});
			queued += sizes[sequence];
			Require(right->Pending() == sequence + 1 && right->PendingBytes() == queued);
		}
		std::vector<std::byte> frame;
		frame.reserve(524288);
		const size_t capacity = frame.capacity();
		for (size_t sequence = 0; sequence < sizes.size(); ++sequence) {
			Require(right->Receive(frame) == ChannelStatus::Ok);
			Check(frame, static_cast<uint32_t>(sequence), sizes[sequence]);
			Require(frame.capacity() == capacity);
			queued -= sizes[sequence];
			Require(right->Pending() == sizes.size() - sequence - 1 && right->PendingBytes() == queued);
		}
		const auto untouched = frame;
		Require(right->Receive(frame) == ChannelStatus::Empty);
		Require(frame == untouched && frame.capacity() == capacity);

		ChannelSettings settings;
		settings.MaximumFrame = 64;
		settings.Capacity = 128;
		auto [sender, receiver] = MakeLocalChannel(settings);
		Require(sender->Send(Pattern(11, 64)) == ChannelStatus::Ok);
		Require(sender->Send(Pattern(12, 64)) == ChannelStatus::Ok);
		Require(sender->Send(Pattern(13, 64)) == ChannelStatus::Full);
		Require(sender->Send(Pattern(14, 65)) == ChannelStatus::TooLarge);
		Require(receiver->Pending() == 2 && receiver->PendingBytes() == 128);
		// A full direction must not consume or refuse the opposite direction.
		Require(receiver->Send(Pattern(15, 32)) == ChannelStatus::Ok);
		Require(sender->Receive(frame) == ChannelStatus::Ok);
		Check(frame, 15, 32);
		Require(receiver->Pending() == 2 && receiver->PendingBytes() == 128);
		sender->Close();
		Require(receiver->Send(Pattern(16, 32)) == ChannelStatus::Closed);
		Require(sender->Send(Pattern(17, 32)) == ChannelStatus::Closed);
		Require(receiver->Receive(frame) == ChannelStatus::Ok);
		Check(frame, 11, 64);
		Require(receiver->Receive(frame) == ChannelStatus::Ok);
		Check(frame, 12, 64);
		const auto last = frame;
		Require(receiver->Receive(frame) == ChannelStatus::Closed);
		Require(frame == last && frame.capacity() == capacity);
		Require(receiver->Pending() == 0 && receiver->PendingBytes() == 0);
	}
} // namespace channel_fixture
