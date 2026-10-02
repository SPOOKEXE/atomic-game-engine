#pragma once

#include <engine/assets/ContentPolicy.hpp>
#include <engine/imagegraph/Document.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace engine::imagegraphexport {
	struct GraphImageInput {
		std::string SourceId;
		std::filesystem::path File;
	};
	// Explicit host export. Encoder paths are caller grants, never found through PATH.
	struct GraphExportSettings {
		std::filesystem::path Input;
		std::filesystem::path Output;
		std::string OutputId;
		std::vector<GraphImageInput> ImageInputs;
		std::span<const engine::imagegraph::HostNodeCapture> HostCaptures;
		engine::imagegraph::HostNodeProvider *HostProvider = nullptr;
		engine::assets::ContentPolicy Content =
			engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle);
		engine::imagegraph::TickRange Frames{};
		bool Animation = false;
		double Scale = 1;
		bool LinearScaling = false;
		uint8_t PngSubformat = 2;
		std::optional<size_t> ArrayIndex;
		uint16_t FrameMilliseconds = 33;
		uint16_t DelayNumerator = 0;
		uint16_t DelayDenominator = 0;
		uint32_t Plays = 0;
		uint32_t Quality = 23;
		std::optional<double> EncoderQuality;
		std::optional<double> EncoderFramesPerSecond;
		bool OptimiseGif = false;
		uint32_t GifBatchSize = 0;
		bool NativeGif = false;
		uint8_t NativeGifQuality = 2;
		// Keep the bounded exclusive .graph-export-* frame directory beside the granted output.
		bool RetainTemporaryFrames = false;
		double GifColorMerge = 0.02;
		double MegabitsPerSecond = 2;
		std::filesystem::path ImageEncoder;
		std::filesystem::path VideoEncoder;
		std::chrono::milliseconds EncoderTimeout{30000};
	};

	// Build argv without shell parsing. ExportGraph uses the same plan for its real subprocess.
	bool BuildGraphEncoderArguments(
		const GraphExportSettings &settings,
		const std::filesystem::path &frames,
		const std::filesystem::path &target,
		size_t frameCount,
		std::filesystem::path &executable,
		std::vector<std::string> &arguments,
		std::string &failure
	);

	// Evaluate, encode and atomically publish. Failed frames or encoders keep the prior destination.
	bool ExportGraph(
		const GraphExportSettings &settings,
		std::string &failure,
		std::filesystem::path *retainedTemporaryDirectory = nullptr
	);
	// Resolve one explicitly requested host node and execute its granted capability.
	// Uses the immutable live document and observations. Input is a pathname context, never read.
	bool ExportGraph(
		const GraphExportSettings &settings,
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		const engine::imagegraph::EvaluationRequest &request,
		std::string &failure,
		std::filesystem::path *retainedTemporaryDirectory = nullptr
	);

	bool ExecuteGraphHostNode(
		const GraphExportSettings &settings,
		std::string_view nodeId,
		engine::imagegraph::HostNodeCapture &capture,
		std::string &failure
	);

	// Executes the selected node with resolved live inputs and the explicitly supplied provider.
	bool ExecuteGraphHostNode(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		const engine::imagegraph::EvaluationRequest &request,
		std::string_view nodeId,
		engine::imagegraph::HostNodeCapture &capture,
		std::string &failure
	);

}
