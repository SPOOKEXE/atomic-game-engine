#pragma once
#include <engine/imagegraph/Document.hpp>
namespace engine::imagegraph {
	struct SliceStackFace {
		std::array<Vector3, 3> Points{};
		Vector3 Center{};
		double MaximumX = 0;
		Colour Color{255, 255, 255, 255};
		bool operator==(const SliceStackFace &) const = default;
	};
	struct SliceStackReplayEntry {
		std::string NodeId;
		FrameTime ActionTime{};
		bool Active = false;
		Vector3 Minimum{}, Maximum{}, Padding{};
		uint32_t Width = 0, Height = 0, Slices = 0, Slice = 0, Pixel = 0;
		std::vector<SliceStackFace> Faces;
		std::vector<Image> Images;
		bool operator==(const SliceStackReplayEntry &) const = default;
	};
	struct SliceStackReplayState {
		std::vector<SliceStackReplayEntry> Entries;
		bool operator==(const SliceStackReplayState &) const = default;
	};
	uint64_t RetainedSliceStackReplayBytes(const SliceStackReplayState &state);
	Status ValidateSliceStackReplay(
		const SliceStackReplayState &state, uint64_t maximumBytes, Diagnostic &diagnostic
	);
	// Starts authored Splice actions and advances at most WorkPixels. Failure leaves the owner unchanged.
	// Source ray tests and color selection are preserved; pixel scheduling replaces its wall-time partition.
	Status AdvanceSliceStack(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		const EvaluationRequest &request,
		uint32_t workPixels,
		SliceStackReplayState &state,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
	std::string WriteSliceStackReplay(const SliceStackReplayState &state);
	Status ReadSliceStackReplay(
		std::string_view text,
		SliceStackReplayState &state,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
}
