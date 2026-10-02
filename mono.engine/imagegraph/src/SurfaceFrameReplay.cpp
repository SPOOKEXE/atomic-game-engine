#include <engine/imagegraph/SurfaceFrameReplay.hpp>

#include <algorithm>
#include <limits>

namespace engine::imagegraph {
	namespace {
		bool Add(uint64_t &bytes, uint64_t amount) {
			if (amount > std::numeric_limits<uint64_t>::max() - bytes) {
				bytes = std::numeric_limits<uint64_t>::max();
				return false;
			}
			bytes += amount;
			return true;
		}
	}
	uint64_t RetainedSurfaceFrameEntryBytes(const SurfaceFrameReplayEntry &entry) {
		uint64_t bytes = sizeof(entry);
		Add(bytes, entry.NodeId.capacity());
		Add(bytes, entry.Input.Pixels.capacity());
		return bytes;
	}
	uint64_t RetainedSurfaceFrameReplayBytes(const SurfaceFrameReplayState &state) {
		uint64_t bytes = state.Entries.capacity() * sizeof(SurfaceFrameReplayEntry);
		for (const auto &entry : state.Entries) {
			Add(bytes, entry.NodeId.capacity());
			Add(bytes, entry.Input.Pixels.capacity());
		}
		return bytes;
	}
	Status ValidateSurfaceFrameReplay(
		const SurfaceFrameReplayState &state, uint64_t maximumBytes, Diagnostic &diagnostic
	) {
		if (state.Entries.size() > Limits::MaximumArrayElements ||
			RetainedSurfaceFrameReplayBytes(state) > maximumBytes) {
			diagnostic = {Status::LimitExceeded, {}, {}, "surface replay owner exceeds byte or entry bounds"};
			return diagnostic.Code;
		}
		for (size_t i = 0; i < state.Entries.size(); i++) {
			const auto &entry = state.Entries[i];
			if (entry.NodeId.empty() || entry.NodeId.size() > Limits::MaximumTextBytes ||
				entry.Frame > Limits::MaximumTick ||
				!ValidSurfaceLayout(entry.Input, Limits::MaximumDimension, Limits::MaximumOutputBytes) ||
				!FiniteSurfaceSamples(entry.Input)) {
				diagnostic = {Status::InvalidValue, entry.NodeId, {}, "surface replay entry is malformed"};
				return diagnostic.Code;
			}
			for (size_t j = 0; j < i; j++)
				if (state.Entries[j].NodeId == entry.NodeId && state.Entries[j].Frame == entry.Frame &&
					state.Entries[j].ProcessorRow == entry.ProcessorRow) {
					diagnostic = {
						Status::DuplicateId, entry.NodeId, {}, "surface replay frame identity is duplicated"
					};
					return diagnostic.Code;
				}
		}
		return Status::Ok;
	}
}
