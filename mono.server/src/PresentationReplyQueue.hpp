#pragma once

#include <engine/world/PresentationStream.hpp>

#include <algorithm>
#include <deque>
#include <utility>

namespace server {
	struct PresentationReplyDrain {
		size_t Queued = 0;
		size_t TransferEyes = 0;
		size_t Invalid = 0;
		size_t RoutineDeferred = 0;
		bool Full = false;
	};

	// One reliable user lane has a 64 KiB default tick allowance. Keep routine
	// work to one window so a transfer eye can join before a long FIFO develops.
	inline constexpr uint64_t ROUTINE_PRESENTATION_WINDOW_BYTES = 64u * 1024u;

	inline size_t RetirePresentationReplies(
		std::deque<engine::world::PresentationMessage> &pending,
		const engine::world::PresentationDirectory &directory
	) {
		return std::erase_if(pending, [&](const auto &message) {
			return std::find(directory.Endpoints.begin(), directory.Endpoints.end(), message.To) ==
				   directory.Endpoints.end();
		});
	}

	inline size_t RetireClosedPresentationReplies(
		std::deque<engine::world::PresentationMessage> &pending,
		const engine::world::PresentationStream &stream
	) {
		if (stream.Open()) return 0;
		const auto count = pending.size();
		pending.clear();
		return count;
	}

	// Select endpoint heads so an urgent transfer eye cannot pass an earlier
	// message from the same endpoint. Routine traffic remains pending after one
	// reliable user-lane window.
	inline PresentationReplyDrain DrainPresentationReplies(
		std::deque<engine::world::PresentationMessage> &pending, engine::world::PresentationStream &stream
	) {
		using namespace engine::world;
		PresentationReplyDrain result;
		while (!pending.empty()) {
			auto selected = pending.end();
			for (auto candidate = pending.begin(); candidate != pending.end(); ++candidate) {
				const bool endpointHead =
					std::none_of(pending.begin(), candidate, [&](const PresentationMessage &earlier) {
						return earlier.To == candidate->To;
					});
				if (!endpointHead) continue;
				if (candidate->Priority == PresentationPriority::TransferEye) {
					selected = candidate;
					break;
				}
				if (selected == pending.end()) selected = candidate;
			}
			if (selected == pending.end()) break;
			const bool transferEye = selected->Priority == PresentationPriority::TransferEye;
			if (!transferEye && stream.Outgoing().Bytes >= ROUTINE_PRESENTATION_WINDOW_BYTES) {
				++result.RoutineDeferred;
				break;
			}
			PresentationStreamFrame frame;
			frame.Message = std::move(*selected);
			const auto status = stream.Queue(frame);
			if (status == PresentationStatus::Full) {
				*selected = std::move(frame.Message);
				result.Full = true;
				break;
			}
			pending.erase(selected);
			if (status == PresentationStatus::Ok) {
				++result.Queued;
				if (transferEye) ++result.TransferEyes;
			} else {
				++result.Invalid;
			}
		}
		return result;
	}
}
