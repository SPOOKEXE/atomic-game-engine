#include "../PixelBuilderPayload.hpp"
#include "../SourceSafeDraw.hpp"
#include "Processor.hpp"

#include <algorithm>
#include <numeric>

namespace engine::imagegraph::detail {
	bool SurfaceSize(NodeContext &context, double x, double y, uint32_t &width, uint32_t &height);
	namespace {
		bool AddRecipeBytes(uint64_t &bytes, uint64_t extra) {
			if (extra > UINT64_MAX - bytes) return false;
			bytes += extra;
			return bytes <= Limits::MaximumEvaluationBytes;
		}
		bool FreezeBuilder(NodeContext &context, Vector2 dimension) {
			if (!context.EvaluationDocument)
				return context.Fail(
					Status::InvalidValue,
					"Dynamic Builder requires an authored evaluation document",
					"dynamic_builder"
				);
			const auto documentBytes = DocumentRetainedPayloadBytes(*context.EvaluationDocument);
			if (!documentBytes)
				return context.Fail(
					Status::LimitExceeded,
					"Builder authoring snapshot exceeds its byte bound",
					"dynamic_builder"
				);
			uint64_t bytes = sizeof(PixelBuilderData) + *documentBytes + context.Authored.Id.capacity();
			uint64_t builtinBytes = 0;
			Diagnostic builtinDiagnostic;
			if (ValidateBuiltinRandomCaptures(
					context.Request.BuiltinRandomCaptures,
					Limits::MaximumEvaluationBytes,
					builtinBytes,
					builtinDiagnostic
				) != Status::Ok)
				return context.Fail(builtinDiagnostic.Code, builtinDiagnostic.Message);
			if (!AddRecipeBytes(bytes, builtinBytes))
				return context.Fail(
					Status::LimitExceeded, "Builder builtin RNG snapshot exceeds its byte bound"
				);
			if (context.Request.GroupReplay &&
				!AddRecipeBytes(bytes, PixelBuilderGroupCloneBytes(*context.Request.GroupReplay)))
				return context.Fail(
					Status::LimitExceeded, "Builder group snapshot exceeds its byte bound", "dynamic_builder"
				);
			for (const auto &frame : context.Request.AudioFrames) {
				if (!AddRecipeBytes(
						bytes,
						sizeof(frame) + frame.SourceId.capacity() +
							frame.Samples.capacity() * sizeof(double) +
							frame.Channels.capacity() * sizeof(std::vector<double>)
					))
					return context.Fail(
						Status::LimitExceeded,
						"Builder audio snapshot exceeds its byte bound",
						"dynamic_builder"
					);
				for (const auto &channel : frame.Channels)
					if (!AddRecipeBytes(bytes, channel.capacity() * sizeof(double)))
						return context.Fail(
							Status::LimitExceeded,
							"Builder audio snapshot exceeds its byte bound",
							"dynamic_builder"
						);
			}
			for (const auto &clip : context.Request.AudioClips)
				if (!AddRecipeBytes(
						bytes, sizeof(clip) + clip.SourceId.capacity() + RetainedPayloadBytes(clip.Data)
					))
					return context.Fail(
						Status::LimitExceeded,
						"Builder clip snapshot exceeds its byte bound",
						"dynamic_builder"
					);
			for (const auto &image : context.Request.ImageSources)
				if (!AddRecipeBytes(
						bytes, sizeof(image) + image.SourceId.capacity() + image.Data.Pixels.capacity()
					))
					return context.Fail(
						Status::LimitExceeded,
						"Builder image snapshot exceeds its byte bound",
						"dynamic_builder"
					);
			const size_t receiptCount =
				context.Request.HostCaptures.size() + context.ObservedHostCaptures.size();
			if (receiptCount > Limits::MaximumNodes * 2)
				return context.Fail(
					Status::LimitExceeded, "Builder host receipt count exceeds bounded nodes"
				);
			auto receiptsCharge =
				context.ReserveWorkspace(receiptCount * sizeof(const HostNodeCapture *), "dynamic_builder");
			if (!receiptsCharge) return false;
			std::vector<const HostNodeCapture *> receipts;
			receipts.reserve(receiptCount);
			for (const auto source : {context.Request.HostCaptures, context.ObservedHostCaptures})
				for (const auto &capture : source)
					if (std::none_of(receipts.begin(), receipts.end(), [&](const auto *other) {
							return PixelBuilderRecordingsEqual(capture, *other);
						}))
						receipts.push_back(&capture);
			if (receipts.size() > Limits::MaximumNodes)
				return context.Fail(
					Status::LimitExceeded, "Builder unique host receipts exceed bounded nodes"
				);
			for (const auto *record : receipts) {
				const auto &capture = *record;
				if (!ValidPixelBuilderRecording(capture))
					return context.Fail(
						Status::UnsupportedExecution,
						"Builder host recording requires a bounded frozen closure",
						"dynamic_builder"
					);
				const auto nodeBytes = NodeClonePayloadBytes(capture.Authored);
				if (!nodeBytes || !AddRecipeBytes(
									  bytes,
									  sizeof(capture) + *nodeBytes + capture.Failure.capacity() +
										  capture.Inputs.capacity() * sizeof(AuthoredValue) +
										  capture.Outputs.capacity() * sizeof(AuthoredValue) +
										  capture.InputImages.capacity() * sizeof(HostImageBinding) +
										  capture.Images.capacity() * sizeof(HostCapturedImage) +
										  capture.ImageArrays.capacity() * sizeof(HostCapturedImageArray)
								  ))
					return context.Fail(
						Status::LimitExceeded,
						"Builder host snapshot exceeds its byte bound",
						"dynamic_builder"
					);
				for (const auto *values : {&capture.Inputs, &capture.Outputs})
					for (const auto &value : *values)
						if (!AddRecipeBytes(bytes, value.Port.capacity() + RetainedPayloadBytes(value.Data)))
							return context.Fail(
								Status::LimitExceeded,
								"Builder host value snapshot exceeds its byte bound",
								"dynamic_builder"
							);
				for (const auto &binding : capture.InputImages)
					if (!AddRecipeBytes(bytes, binding.Port.capacity()))
						return context.Fail(
							Status::LimitExceeded,
							"Builder host binding snapshot exceeds its byte bound",
							"dynamic_builder"
						);
				for (const auto &image : capture.Images)
					if (!AddRecipeBytes(bytes, image.Port.capacity() + image.Data.Pixels.capacity()))
						return context.Fail(
							Status::LimitExceeded,
							"Builder host image snapshot exceeds its byte bound",
							"dynamic_builder"
						);
				for (const auto &images : capture.ImageArrays) {
					if (!AddRecipeBytes(
							bytes, images.Port.capacity() + images.Frames.capacity() * sizeof(Image)
						))
						return context.Fail(
							Status::LimitExceeded,
							"Builder host array snapshot exceeds its byte bound",
							"dynamic_builder"
						);
					for (const auto &image : images.Frames)
						if (!AddRecipeBytes(bytes, image.Pixels.capacity()))
							return context.Fail(
								Status::LimitExceeded,
								"Builder host array snapshot exceeds its byte bound",
								"dynamic_builder"
							);
				}
			}
			if (context.Request.SimulationReplay &&
				!AddRecipeBytes(bytes, RetainedSimulationReplayBytes(*context.Request.SimulationReplay)))
				return context.Fail(
					Status::LimitExceeded,
					"Builder simulation snapshot exceeds its byte bound",
					"dynamic_builder"
				);
			if (context.Request.SurfaceReplay &&
				!AddRecipeBytes(bytes, RetainedSurfaceFrameReplayBytes(*context.Request.SurfaceReplay)))
				return context.Fail(
					Status::LimitExceeded,
					"Builder surface snapshot exceeds its byte bound",
					"dynamic_builder"
				);
			if (context.Request.RigidReplay &&
				!AddRecipeBytes(bytes, RetainedRigidReplayBytes(*context.Request.RigidReplay)))
				return context.Fail(Status::LimitExceeded, "Builder rigid history exceeds its byte bound");
			if (context.Request.DataReplay &&
				!AddRecipeBytes(bytes, RetainedDataReplayBytes(*context.Request.DataReplay)))
				return context.Fail(Status::LimitExceeded, "Builder data history exceeds its byte bound");
			if (context.Request.SliceStackReplay &&
				!AddRecipeBytes(bytes, RetainedSliceStackReplayBytes(*context.Request.SliceStackReplay)))
				return context.Fail(Status::LimitExceeded, "Builder slice stack exceeds its byte bound");
			if (context.Request.RandomReplay &&
				!AddRecipeBytes(bytes, RetainedRandomReplayBytes(*context.Request.RandomReplay)))
				return context.Fail(
					Status::LimitExceeded, "Builder random snapshot exceeds its byte bound", "dynamic_builder"
				);
			for (const auto &entropy : context.Request.RandomEntropy)
				if (!AddRecipeBytes(bytes, sizeof(entropy) + entropy.NodeId.capacity()))
					return context.Fail(
						Status::LimitExceeded,
						"Builder entropy snapshot exceeds its byte bound",
						"dynamic_builder"
					);
			if (!AddRecipeBytes(
					bytes,
					context.Request.ProjectName.size() +
						context.Request.PcxObservations.size() * sizeof(AuthoredValue)
				))
				return context.Fail(
					Status::LimitExceeded, "Builder PCX snapshot exceeds its byte bound", "dynamic_builder"
				);
			for (const auto &observation : context.Request.PcxObservations) {
				if (!ValidPixelBuilderRecordingValue(observation.Data))
					return context.Fail(
						Status::UnsupportedExecution,
						"Builder PCX observation requires a bounded source closure",
						"dynamic_builder"
					);
				if (!AddRecipeBytes(
						bytes, observation.Port.capacity() + RetainedPayloadBytes(observation.Data)
					))
					return context.Fail(
						Status::LimitExceeded,
						"Builder PCX snapshot exceeds its byte bound",
						"dynamic_builder"
					);
			}
			if (!AddRecipeBytes(bytes, context.Request.SimulationCacheCaptures.size() * sizeof(std::string)))
				return context.Fail(
					Status::LimitExceeded,
					"Builder cache action snapshot exceeds its byte bound",
					"dynamic_builder"
				);
			for (const auto node : context.Request.SimulationCacheCaptures)
				if (!AddRecipeBytes(bytes, std::max(node.size(), std::string{}.capacity())))
					return context.Fail(
						Status::LimitExceeded,
						"Builder cache action snapshot exceeds its byte bound",
						"dynamic_builder"
					);
			if (!context.ReserveOutput(bytes, "dynamic_builder")) return false;
			DynamicSurfaceValue recipe;
			auto &data = recipe.Data.emplace();
			data.Authored = *context.EvaluationDocument;
			data.OwnerNodeId = context.Authored.Id;
			data.GroupAuthoringRevision = context.Request.GroupAuthoringRevision;
			if (context.Request.GroupReplay) {
				Diagnostic diagnostic;
				const auto code = FreezePixelBuilderGroups(
					*context.Request.GroupReplay,
					data.Groups.emplace(),
					diagnostic,
					Limits::MaximumEvaluationBytes
				);
				if (code != Status::Ok) return context.Fail(code, diagnostic.Message, "dynamic_builder");
			}
			data.BaseDimension = dimension;
			data.Tick = context.Request.Tick;
			data.Seed = context.Request.Seed;
			data.Subframe = context.Request.Subframe;
			data.NegativeFrame = context.Request.NegativeFrame;
			data.ResetSurfaceReplay = context.Request.ResetSurfaceReplay;
			data.MaximumImageDimension = context.Request.MaximumImageDimension;
			data.CirclePrecision = context.Request.PixelBuilderCirclePrecision;
			data.RequireSourceGpuRasterCoverage = context.Request.RequireSourceGpuRasterCoverage;
			data.AudioFrames.assign(context.Request.AudioFrames.begin(), context.Request.AudioFrames.end());
			data.AudioClips.assign(context.Request.AudioClips.begin(), context.Request.AudioClips.end());
			data.ImageSources.assign(
				context.Request.ImageSources.begin(), context.Request.ImageSources.end()
			);
			data.HostCaptures.reserve(receipts.size());
			for (const auto *receipt : receipts)
				data.HostCaptures.push_back(*receipt);
			if (context.Request.DataReplay) data.DataHistory = *context.Request.DataReplay;
			// Retained Draw regenerates this frame from the immutable prior, never the current actor prefix.
			if (context.Request.RigidReplay) data.RigidHistory = *context.Request.RigidReplay;
			data.RigidAuthoringRevision = context.Request.RigidAuthoringRevision;
			data.RigidPlaying = context.Request.RigidPlaying;
			data.RigidFrameProgress = context.Request.RigidFrameProgress;
			if (context.Request.SliceStackReplay) data.SliceStack = *context.Request.SliceStackReplay;
			data.SimulationAuthoringRevision = context.Request.SimulationAuthoringRevision;
			if (context.Request.SimulationReplay) data.Simulation = *context.Request.SimulationReplay;
			if (context.Request.SurfaceReplay) data.Surfaces = *context.Request.SurfaceReplay;
			if (context.Request.RandomReplay) data.Random = *context.Request.RandomReplay;
			data.SimulationCacheCaptures.reserve(context.Request.SimulationCacheCaptures.size());
			for (const auto node : context.Request.SimulationCacheCaptures)
				data.SimulationCacheCaptures.emplace_back(node);
			data.ProjectName = context.Request.ProjectName;
			data.PcxObservations.assign(
				context.Request.PcxObservations.begin(), context.Request.PcxObservations.end()
			);
			data.BuiltinRandomCaptures.assign(
				context.Request.BuiltinRandomCaptures.begin(), context.Request.BuiltinRandomCaptures.end()
			);
			data.Entropy.assign(context.Request.RandomEntropy.begin(), context.Request.RandomEntropy.end());
			if (!ValidPixelBuilderPayload(recipe))
				return context.Fail(
					Status::UnsupportedExecution,
					"Builder recordings require a bounded frozen source closure",
					"dynamic_builder"
				);
			context.SetValue("dynamic_builder", std::move(recipe));
			return context.FailureCode == Status::Ok;
		}
	}
	bool PixelBuilder(NodeContext &context) {
		Vector2 dimension;
		if (context.PixelBuilderCanvas)
			dimension = *context.PixelBuilderCanvas;
		else {
			dimension = context.Vec2("dimension", {1, 1});
			if (!context.IsLinked("dimension") && context.Integer("dimension_unit", 1) == 1) {
				dimension.X *= context.Project.SurfaceWidth;
				dimension.Y *= context.Project.SurfaceHeight;
			}
		}
		uint32_t width = 0, height = 0;
		if (!SurfaceSize(context, dimension.X, dimension.Y, width, height))
			return context.Fail(
				Status::InvalidValue, "Builder dimensions must be positive and bounded", "dimension"
			);
		if (context.PixelBuilderLayers.size() > Limits::MaximumNodes)
			return context.Fail(Status::LimitExceeded, "Builder layer count exceeds its node bound");
		auto workspace = context.ReserveWorkspace(
			uint64_t(width) * height * 4 + context.PixelBuilderLayers.size() * sizeof(size_t)
		);
		if (!workspace) return false;
		Image layers{width, height, std::vector<uint8_t>(uint64_t(width) * height * 4), 0};
		for (const auto &layer : context.PixelBuilderLayers)
			if (!std::isfinite(layer.Layer) || layer.BlendMode < 0 || layer.BlendMode > 1)
				return context.Fail(Status::InvalidValue, "Builder layer controls are invalid");
		std::vector<size_t> order(context.PixelBuilderLayers.size());
		std::iota(order.begin(), order.end(), 0);
		// GameMaker permits any order for equal priorities. Preserve the evaluator's authored order.
		std::sort(order.begin(), order.end(), [&](size_t left, size_t right) {
			const double a = context.PixelBuilderLayers[left].Layer,
						 b = context.PixelBuilderLayers[right].Layer;
			return a < b || (a == b && left < right);
		});
		for (size_t index : order) {
			const auto &layer = context.PixelBuilderLayers[index];
			if (!layer.Active || !layer.Surface) continue;
			if (!std::isfinite(layer.Layer) || layer.BlendMode < 0 || layer.BlendMode > 1)
				return context.Fail(Status::InvalidValue, "Builder layer controls are invalid");
			for (uint32_t y = 0; y < std::min(height, layer.Surface->Height); ++y)
				for (uint32_t x = 0; x < std::min(width, layer.Surface->Width); ++x) {
					const Rgba source = SourceSafeDrawPixel(*layer.Surface, x, y);
					Rgba colour = ReadPixel(layers, x, y);
					for (size_t channel = 0; channel < 4; ++channel)
						colour[channel] = layer.BlendMode == 0 ? source[channel] * source[3] +
																	 colour[channel] * (1 - source[3])
															   : colour[channel] * (1 - source[channel]);
					if (!WritePixel(layers, x, y, colour))
						return context.Fail(Status::InvalidValue, "Builder layer sample is nonfinite");
				}
		}
		Image *output = context.NewImage("surface_out", width, height, SurfaceFormat::RGBA8Unorm);
		if (!output) return false;
		const bool outline = context.Boolean("outline");
		const double thickness = context.Integer("thickness");
		const Colour authored = context.Get<Colour>("color", Colour{255, 255, 255, 255});
		const Rgba outlineColour{
			authored.Red / 255.0, authored.Green / 255.0, authored.Blue / 255.0, authored.Alpha / 255.0
		};
		if (outline && uint64_t(width) * height > 64000000 / 1089)
			return context.Fail(
				Status::LimitExceeded, "Builder outline exceeds the sampling budget", "thickness"
			);
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				Rgba colour = ReadPixel(layers, x, y);
				if (outline && colour[3] != 1) {
					double borderDistance = 99999;
					for (int i = -16; i <= 16; ++i)
						for (int j = -16; j <= 16; ++j) {
							if (std::abs(i) > thickness + 1 || std::abs(j) > thickness + 1) continue;
							const Rgba sample =
								SampleNearest(layers, (x + 0.5 + i) / width, (y + 0.5 + j) / height);
							if (sample[3] < 1) continue;
							double distance = std::hypot(i, j);
							if ((i == 0 && std::abs(j) == 1) || (std::abs(i) == 1 && j == 0))
								distance = 0;
							else if (std::abs(i) == 1 && std::abs(j) == 1)
								distance = 1;
							borderDistance = std::min(borderDistance, distance);
						}
					if (borderDistance <= thickness) colour = outlineColour;
				}
				if (!WritePixel(*output, x, y, colour))
					return context.Fail(
						Status::InvalidValue, "Builder final sample is nonfinite", "surface_out"
					);
			}
		return FreezeBuilder(context, dimension);
	}
}
