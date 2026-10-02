#pragma once

#include <engine/imagegraph/DataReplay.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraph/RandomReplay.hpp>
#include <engine/imagegraph/SimulationReplay.hpp>
#include <engine/imagegraph/SliceStackReplay.hpp>
#include <engine/imagegraph/SurfaceFrameReplay.hpp>

namespace engine::imagegraph {
	struct PixelBuilderGroupState {
		GroupReplayState Replay;
		PixelBuilderGroupState() = default;
		PixelBuilderGroupState(const PixelBuilderGroupState &other);
		PixelBuilderGroupState &operator=(const PixelBuilderGroupState &other);
		PixelBuilderGroupState(PixelBuilderGroupState &&) noexcept = default;
		PixelBuilderGroupState &operator=(PixelBuilderGroupState &&) noexcept = default;
		bool operator==(const PixelBuilderGroupState &other) const;
	};
	// A recipe owns source authoring and recordings. It never retains a host provider or request spans.
	struct PixelBuilderData {
		Document Authored;
		std::string OwnerNodeId;
		Vector2 BaseDimension;
		uint64_t Tick = 0, Seed = 0;
		double Subframe = 0;
		bool NegativeFrame = false, ResetSurfaceReplay = false;
		bool RequireSourceGpuRasterCoverage = false;
		uint32_t MaximumImageDimension = Limits::MaximumDimension;
		uint32_t CirclePrecision = 24;
		std::vector<AudioCaptureFrame> AudioFrames;
		std::vector<AudioClipSource> AudioClips;
		std::vector<RequestImageSource> ImageSources;
		std::vector<HostNodeCapture> HostCaptures;
		std::optional<SimulationReplayState> Simulation;
		std::optional<SurfaceFrameReplayState> Surfaces;
		std::optional<RandomReplayState> Random;
		std::optional<DataReplayState> DataHistory;
		std::optional<SliceStackReplayState> SliceStack;
		uint64_t SimulationAuthoringRevision = 0;
		std::optional<PixelBuilderGroupState> Groups;
		uint64_t GroupAuthoringRevision = 0;
		std::vector<RandomEntropyCapture> Entropy;
		std::vector<AuthoredValue> PcxObservations;
		std::string ProjectName;
		std::vector<std::string> SimulationCacheCaptures;
		bool operator==(const PixelBuilderData &other) const;
	};
}
namespace engine::imagegraph::detail {
	// Borrowed only while one evaluator retains the child image producers.
	struct PixelBuilderLayer {
		std::string_view NodeId;
		const Image *Surface = nullptr;
		double Layer = 1;
		int64_t BlendMode = 0;
		bool Active = true;
	};
	uint64_t PixelBuilderStorageBytes(const DynamicSurfaceValue &value, bool retained);
	bool ValidPixelBuilderPayload(const DynamicSurfaceValue &value);
	bool ValidPixelBuilderRecording(const HostNodeCapture &capture);
	bool PixelBuilderRecordingsEqual(const HostNodeCapture &left, const HostNodeCapture &right);
	bool ValidPixelBuilderRecordingValue(const Value &value);
	uint64_t PixelBuilderGroupCloneBytes(const GroupReplayState &source);
	Status OverridePixelBuilderGroups(
		const GroupReplayState &source,
		std::string_view nodeId,
		PixelBuilderGroupState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	);
	Status FreezePixelBuilderGroups(
		const GroupReplayState &source,
		PixelBuilderGroupState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	);
	Status RasterizePixelBuilder(
		const DynamicSurfaceValue &value,
		Vector2 dimension,
		Image &image,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
	);
}
