#include "PixelBuilderPayload.hpp"

#include "ValuePayload.hpp"

#include <algorithm>
#include <limits>
#include <tuple>

namespace engine::imagegraph {
	DynamicSurfaceValue::DynamicSurfaceValue() = default;
	DynamicSurfaceValue::~DynamicSurfaceValue() = default;
	DynamicSurfaceValue::DynamicSurfaceValue(const DynamicSurfaceValue &) = default;
	DynamicSurfaceValue &DynamicSurfaceValue::operator=(const DynamicSurfaceValue &) = default;
	DynamicSurfaceValue::DynamicSurfaceValue(DynamicSurfaceValue &&) noexcept = default;
	DynamicSurfaceValue &DynamicSurfaceValue::operator=(DynamicSurfaceValue &&) noexcept = default;
	bool DynamicSurfaceValue::operator==(const DynamicSurfaceValue &other) const {
		return Data == other.Data;
	}
	namespace {
		template <class T, class Equal>
		bool SameSequence(const std::vector<T> &left, const std::vector<T> &right, Equal equal) {
			return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), equal);
		}
		bool SameCapture(const HostNodeCapture &left, const HostNodeCapture &right) {
			if (std::tie(
					left.Authored,
					left.Tick,
					left.Subframe,
					left.NegativeFrame,
					left.Inputs,
					left.Outputs,
					left.State,
					left.Failure
				) !=
				std::tie(
					right.Authored,
					right.Tick,
					right.Subframe,
					right.NegativeFrame,
					right.Inputs,
					right.Outputs,
					right.State,
					right.Failure
				))
				return false;
			return SameSequence(
					   left.InputImages,
					   right.InputImages,
					   [](const auto &a, const auto &b) { return a.Port == b.Port && a.Hash == b.Hash; }
				   ) &&
				   SameSequence(
					   left.Images,
					   right.Images,
					   [](const auto &a, const auto &b) { return a.Port == b.Port && a.Data == b.Data; }
				   ) &&
				   SameSequence(left.ImageArrays, right.ImageArrays, [](const auto &a, const auto &b) {
					   return a.Port == b.Port && a.Frames == b.Frames;
				   });
		}
	}
	bool PixelBuilderData::operator==(const PixelBuilderData &other) const {
		return Authored == other.Authored && OwnerNodeId == other.OwnerNodeId &&
			   BaseDimension == other.BaseDimension && Tick == other.Tick && Seed == other.Seed &&
			   Subframe == other.Subframe && NegativeFrame == other.NegativeFrame &&
			   ResetSurfaceReplay == other.ResetSurfaceReplay &&
			   BuiltinRandomCaptures == other.BuiltinRandomCaptures &&
			   RequireSourceGpuRasterCoverage == other.RequireSourceGpuRasterCoverage &&
			   MaximumImageDimension == other.MaximumImageDimension &&
			   CirclePrecision == other.CirclePrecision && AudioFrames == other.AudioFrames &&
			   Simulation == other.Simulation && Surfaces == other.Surfaces && Random == other.Random &&
			   Groups == other.Groups && GroupAuthoringRevision == other.GroupAuthoringRevision &&
			   DataHistory == other.DataHistory && SliceStack == other.SliceStack &&
			   SimulationAuthoringRevision == other.SimulationAuthoringRevision && Entropy == other.Entropy &&
			   PcxObservations == other.PcxObservations && ProjectName == other.ProjectName &&
			   SimulationCacheCaptures == other.SimulationCacheCaptures &&
			   SameSequence(
				   AudioClips,
				   other.AudioClips,
				   [](const auto &a, const auto &b) { return a.SourceId == b.SourceId && a.Data == b.Data; }
			   ) &&
			   SameSequence(
				   ImageSources,
				   other.ImageSources,
				   [](const auto &a, const auto &b) { return a.SourceId == b.SourceId && a.Data == b.Data; }
			   ) &&
			   SameSequence(HostCaptures, other.HostCaptures, SameCapture);
	}
}
namespace engine::imagegraph::detail {
	namespace {
		uint64_t Add(uint64_t left, uint64_t right) {
			return right > UINT64_MAX - left ? UINT64_MAX : left + right;
		}
		template <class T> uint64_t Container(const std::vector<T> &items, bool retained) {
			return uint64_t(retained ? items.capacity() : items.size()) * sizeof(T);
		}
		uint64_t Text(const std::string &text, bool retained) {
			return retained ? text.capacity() : text.size();
		}
		uint64_t ImageBytes(const Image &image, bool retained) {
			return retained ? image.Pixels.capacity() : image.Pixels.size();
		}
		// One traversal bounds owned aggregate/AST closures before recursive byte accounting.
		template <class T> bool FrozenLeaf(const T &item, size_t depth, size_t &count) {
			if (depth > Limits::MaximumArrayDepth || ++count > Limits::MaximumArrayElements) return false;
			if constexpr (std::is_same_v<T, DynamicSurfaceValue>)
				return false;
			else if constexpr (std::is_same_v<T, StructValue>) {
				if (item.Data)
					for (const auto &[key, value] : item.Data->Fields)
						if (key.size() > Limits::MaximumTextBytes ||
							!std::visit(
								[&](const auto &child) { return FrozenLeaf(child, depth + 1, count); }, value
							))
							return false;
			} else if constexpr (std::is_same_v<T, PcxExpressionValue>) {
				if (item.Data) {
					for (const auto &instruction : item.Data->Instructions)
						if (!std::visit(
								[&](const auto &child) { return FrozenLeaf(child, depth + 1, count); },
								instruction.Literal
							))
							return false;
					for (const auto &[key, value] : item.Data->Bindings)
						if (key.size() > Limits::MaximumTextBytes ||
							!std::visit(
								[&](const auto &child) { return FrozenLeaf(child, depth + 1, count); }, value
							))
							return false;
				}
			} else if constexpr (std::is_same_v<T, ArraySelectorValue>) {
				if (item.Data && !FrozenLeaf(item.Data->Values, depth + 1, count)) return false;
			} else if constexpr (std::is_same_v<T, ArrayValue>) {
				if (item.Elements.size() > Limits::MaximumArrayElements ||
					item.Nested.size() > Limits::MaximumArrayElements ||
					item.Items.size() > Limits::MaximumArrayElements)
					return false;
				for (const auto &leaf : item.Elements)
					if (!std::visit(
							[&](const auto &child) { return FrozenLeaf(child, depth + 1, count); }, leaf
						))
						return false;
				for (const auto &row : item.Nested)
					for (const auto &leaf : row)
						if (!std::visit(
								[&](const auto &child) { return FrozenLeaf(child, depth + 2, count); }, leaf
							))
							return false;
				const auto visitItem =
					[&](const auto &self, const SourceArrayItem &entry, size_t level) -> bool {
					if (level > Limits::MaximumArrayDepth || ++count > Limits::MaximumArrayElements)
						return false;
					if (const auto *leaf = std::get_if<ElementValue>(&entry.Data))
						return std::visit(
							[&](const auto &child) { return FrozenLeaf(child, level + 1, count); }, *leaf
						);
					for (const auto &child : std::get<std::vector<SourceArrayItem>>(entry.Data))
						if (!self(self, child, level + 1)) return false;
					return true;
				};
				for (const auto &entry : item.Items)
					if (!visitItem(visitItem, entry, depth + 1)) return false;
			}
			return ValidPayload(item, true);
		}
		bool FrozenValue(const Value &value) {
			size_t count = 0;
			return std::visit([&](const auto &leaf) { return FrozenLeaf(leaf, 0, count); }, value);
		}
		bool AuthoredSnapshot(const Document &document) {
			if (document.Nodes.size() > Limits::MaximumNodes ||
				document.Links.size() > Limits::MaximumLinks ||
				document.Groups.size() > Limits::MaximumGroups ||
				document.Junctions.size() > Limits::MaximumJunctions ||
				document.Keyframes.size() > Limits::MaximumKeyframes ||
				document.Outputs.size() > Limits::MaximumOutputs)
				return false;
			for (const auto &node : document.Nodes) {
				if (node.Values.size() > Limits::MaximumArrayElements) return false;
				for (const auto &value : node.Values)
					if (!ValidValuePayload(value.Data, false)) return false;
				if (node.SourceProperties.size() > Limits::MaximumPropertiesPerNode) return false;
				for (const auto &value : node.SourceProperties)
					if (!ValidValuePayload(value.Data, false)) return false;
			}
			for (const auto &key : document.Keyframes)
				if (!ValidValuePayload(key.Data, false)) return false;
			for (const auto &junction : document.Junctions)
				if (junction.Default && !ValidValuePayload(*junction.Default, false)) return false;
			return true;
		}
		bool FrozenCapture(const HostNodeCapture &capture) {
			if (!std::isfinite(capture.Subframe) || capture.Subframe < 0 || capture.Subframe >= 1 ||
				uint8_t(capture.State) > uint8_t(HostCaptureState::Failed) ||
				capture.Inputs.size() > Limits::MaximumArrayElements ||
				capture.Outputs.size() > Limits::MaximumArrayElements ||
				capture.Images.size() > Limits::MaximumArrayElements ||
				capture.ImageArrays.size() > Limits::MaximumArrayElements ||
				capture.InputImages.size() > Limits::MaximumArrayElements)
				return false;
			for (const auto &value : capture.Authored.Values)
				if (!ValidValuePayload(value.Data, false)) return false;
			if (capture.Authored.SourceProperties.size() > Limits::MaximumPropertiesPerNode) return false;
			for (const auto &value : capture.Authored.SourceProperties)
				if (!ValidValuePayload(value.Data, false)) return false;
			for (const auto &value : capture.Inputs)
				if (!FrozenValue(value.Data)) return false;
			for (const auto &value : capture.Outputs)
				if (!FrozenValue(value.Data)) return false;
			for (const auto &image : capture.Images)
				if (!ValidSurfaceLayout(image.Data, Limits::MaximumDimension, Limits::MaximumArrayBytes) ||
					!FiniteSurfaceSamples(image.Data))
					return false;
			for (const auto &images : capture.ImageArrays) {
				if (images.Frames.size() > Limits::MaximumArrayElements) return false;
				for (const auto &image : images.Frames)
					if (!ValidSurfaceLayout(image, Limits::MaximumDimension, Limits::MaximumArrayBytes) ||
						!FiniteSurfaceSamples(image))
						return false;
			}
			return true;
		}
	}
	bool ValidPixelBuilderRecordingValue(const Value &value) {
		return FrozenValue(value);
	}
	bool PixelBuilderRecordingsEqual(const HostNodeCapture &left, const HostNodeCapture &right) {
		return SameCapture(left, right);
	}
	bool ValidPixelBuilderRecording(const HostNodeCapture &capture) {
		return FrozenCapture(capture);
	}
	uint64_t PixelBuilderStorageBytes(const DynamicSurfaceValue &value, bool retained) {
		if (!value.Data) return 0;
		const auto &data = *value.Data;
		if (!AuthoredSnapshot(data.Authored)) return UINT64_MAX;
		for (const auto &capture : data.HostCaptures)
			if (!FrozenCapture(capture)) return UINT64_MAX;
		uint64_t builtinBytes = 0;
		Diagnostic builtinDiagnostic;
		if (ValidateBuiltinRandomCaptures(
				data.BuiltinRandomCaptures, Limits::MaximumEvaluationBytes, builtinBytes, builtinDiagnostic
			) != Status::Ok)
			return UINT64_MAX;
		const auto documentBytes = DocumentRetainedPayloadBytes(data.Authored);
		if (!documentBytes) return UINT64_MAX;
		uint64_t bytes = Add(Add(sizeof(PixelBuilderData), *documentBytes), builtinBytes);
		if (retained) {
			bytes =
				Add(bytes,
					(data.BuiltinRandomCaptures.capacity() - data.BuiltinRandomCaptures.size()) *
						sizeof(SourceBuiltinRandomCapture));
			for (const auto &capture : data.BuiltinRandomCaptures) {
				bytes =
					Add(bytes,
						(capture.Draws.capacity() - capture.Draws.size()) * sizeof(SourceBuiltinRandomDraw));
				bytes =
					Add(bytes, (capture.Inputs.capacity() - capture.Inputs.size()) * sizeof(AuthoredValue));
				for (const auto &input : capture.Inputs)
					bytes = Add(bytes, input.Port.capacity() + RetainedPayloadBytes(input.Data));
			}
		}
		bytes = Add(bytes, Text(data.OwnerNodeId, retained));
		if (data.Groups) bytes = Add(bytes, data.Groups->Replay.RetainedBytes());
		if (data.DataHistory) bytes = Add(bytes, RetainedDataReplayBytes(*data.DataHistory));
		if (data.SliceStack) bytes = Add(bytes, RetainedSliceStackReplayBytes(*data.SliceStack));
		bytes = Add(bytes, Container(data.AudioFrames, retained));
		for (const auto &frame : data.AudioFrames) {
			bytes = Add(bytes, Text(frame.SourceId, retained));
			bytes = Add(bytes, Container(frame.Samples, retained));
			bytes = Add(bytes, Container(frame.Channels, retained));
			for (const auto &channel : frame.Channels)
				bytes = Add(bytes, Container(channel, retained));
		}
		bytes = Add(bytes, Container(data.AudioClips, retained));
		for (const auto &clip : data.AudioClips) {
			bytes = Add(bytes, Text(clip.SourceId, retained));
			bytes = Add(bytes, retained ? RetainedPayloadBytes(clip.Data) : PayloadOwnedBytes(clip.Data));
		}
		bytes = Add(bytes, Container(data.ImageSources, retained));
		for (const auto &image : data.ImageSources) {
			bytes = Add(bytes, Text(image.SourceId, retained));
			bytes = Add(bytes, ImageBytes(image.Data, retained));
		}
		bytes = Add(bytes, Container(data.HostCaptures, retained));
		for (const auto &capture : data.HostCaptures) {
			const auto node = NodeClonePayloadBytes(capture.Authored);
			if (!node) return UINT64_MAX;
			bytes = Add(bytes, *node);
			bytes = Add(bytes, Text(capture.Failure, retained));
			for (const auto *values : {&capture.Inputs, &capture.Outputs}) {
				bytes = Add(bytes, Container(*values, retained));
				for (const auto &input : *values) {
					bytes = Add(bytes, Text(input.Port, retained));
					bytes =
						Add(bytes,
							retained ? RetainedPayloadBytes(input.Data) : ValuePayloadBytes(input.Data));
				}
			}
			bytes = Add(bytes, Container(capture.InputImages, retained));
			for (const auto &image : capture.InputImages)
				bytes = Add(bytes, Text(image.Port, retained));
			bytes = Add(bytes, Container(capture.Images, retained));
			for (const auto &image : capture.Images) {
				bytes = Add(bytes, Text(image.Port, retained));
				bytes = Add(bytes, ImageBytes(image.Data, retained));
			}
			bytes = Add(bytes, Container(capture.ImageArrays, retained));
			for (const auto &images : capture.ImageArrays) {
				bytes = Add(bytes, Text(images.Port, retained));
				bytes = Add(bytes, Container(images.Frames, retained));
				for (const auto &image : images.Frames)
					bytes = Add(bytes, ImageBytes(image, retained));
			}
		}
		if (data.Simulation) bytes = Add(bytes, RetainedSimulationReplayBytes(*data.Simulation));
		if (data.Surfaces) bytes = Add(bytes, RetainedSurfaceFrameReplayBytes(*data.Surfaces));
		if (data.Random) bytes = Add(bytes, RetainedRandomReplayBytes(*data.Random));
		bytes = Add(bytes, Container(data.Entropy, retained));
		for (const auto &entry : data.Entropy)
			bytes = Add(bytes, Text(entry.NodeId, retained));
		bytes = Add(bytes, Container(data.SimulationCacheCaptures, retained));
		for (const auto &node : data.SimulationCacheCaptures)
			bytes = Add(bytes, Text(node, retained));
		bytes = Add(bytes, Text(data.ProjectName, retained));
		bytes = Add(bytes, Container(data.PcxObservations, retained));
		for (const auto &observation : data.PcxObservations) {
			if (!FrozenValue(observation.Data)) return UINT64_MAX;
			bytes = Add(bytes, Text(observation.Port, retained));
			bytes =
				Add(bytes,
					retained ? RetainedPayloadBytes(observation.Data) : ValuePayloadBytes(observation.Data));
		}
		return bytes;
	}
	bool ValidPixelBuilderPayload(const DynamicSurfaceValue &value) {
		if (!value.Data) return true;
		const auto &data = *value.Data;
		uint64_t builtinBytes = 0;
		Diagnostic builtinDiagnostic;
		if (ValidateBuiltinRandomCaptures(
				data.BuiltinRandomCaptures, Limits::MaximumEvaluationBytes, builtinBytes, builtinDiagnostic
			) != Status::Ok)
			return false;
		if (data.OwnerNodeId.empty() || data.OwnerNodeId.size() > Limits::MaximumTextBytes ||
			!std::isfinite(data.BaseDimension.X) || !std::isfinite(data.BaseDimension.Y) ||
			std::abs(data.BaseDimension.X) > Limits::MaximumDimension ||
			std::abs(data.BaseDimension.Y) > Limits::MaximumDimension || !std::isfinite(data.Subframe) ||
			data.Subframe < 0 || data.Subframe >= 1 || data.MaximumImageDimension == 0 ||
			data.MaximumImageDimension > Limits::MaximumDimension || data.CirclePrecision < 4 ||
			data.CirclePrecision > 64 || data.CirclePrecision % 4 != 0 ||
			data.AudioFrames.size() > Limits::MaximumAudioCaptureFrames ||
			data.AudioClips.size() > Limits::MaximumArrayElements ||
			data.ImageSources.size() > Limits::MaximumArrayElements ||
			data.HostCaptures.size() > Limits::MaximumNodes ||
			data.Entropy.size() > Limits::MaximumArrayElements)
			return false;
		const auto owner =
			std::find_if(data.Authored.Nodes.begin(), data.Authored.Nodes.end(), [&](const Node &node) {
				return node.Id == data.OwnerNodeId && node.Type == "pc.pixel_builder";
			});
		if (owner == data.Authored.Nodes.end()) return false;
		for (const auto &frame : data.AudioFrames) {
			if (frame.SourceId.size() > Limits::MaximumTextBytes ||
				frame.Samples.size() > Limits::MaximumAudioCaptureSamples || frame.Channels.size() > 255 ||
				!std::isfinite(frame.SampleRate) || frame.SampleRate < 0)
				return false;
			for (double sample : frame.Samples)
				if (!std::isfinite(sample)) return false;
			for (const auto &channel : frame.Channels) {
				if (channel.size() > Limits::MaximumAudioCaptureSamples) return false;
				for (double sample : channel)
					if (!std::isfinite(sample)) return false;
			}
		}
		for (const auto &clip : data.AudioClips)
			if (!ValidPayload(clip.Data, true)) return false;
		for (const auto &source : data.ImageSources)
			if (source.SourceId.size() > Limits::MaximumTextBytes ||
				!ValidSurfaceLayout(source.Data, Limits::MaximumDimension, Limits::MaximumArrayBytes) ||
				!FiniteSurfaceSamples(source.Data))
				return false;
		if (data.SimulationCacheCaptures.size() > Limits::MaximumNodes) return false;
		for (const auto &node : data.SimulationCacheCaptures)
			if (node.empty() || node.size() > Limits::MaximumTextBytes) return false;
		if (data.ProjectName.size() > Limits::MaximumTextBytes ||
			data.PcxObservations.size() > Limits::MaximumArrayElements)
			return false;
		for (const auto &entry : data.Entropy)
			if (entry.NodeId.size() > Limits::MaximumTextBytes || !std::isfinite(entry.Subframe) ||
				entry.Subframe < 0 || entry.Subframe >= 1)
				return false;
		Diagnostic diagnostic;
		if (data.DataHistory &&
			ValidateDataReplay(*data.DataHistory, Limits::MaximumEvaluationBytes, diagnostic) != Status::Ok)
			return false;
		if (data.SliceStack &&
			ValidateSliceStackReplay(*data.SliceStack, Limits::MaximumEvaluationBytes, diagnostic) !=
				Status::Ok)
			return false;
		if (data.Simulation &&
			ValidateSimulationReplay(*data.Simulation, Limits::MaximumEvaluationBytes, diagnostic) !=
				Status::Ok)
			return false;
		if (data.Surfaces &&
			ValidateSurfaceFrameReplay(*data.Surfaces, Limits::MaximumEvaluationBytes, diagnostic) !=
				Status::Ok)
			return false;
		if (data.Random &&
			ValidateRandomReplay(*data.Random, Limits::MaximumEvaluationBytes, diagnostic) != Status::Ok)
			return false;
		return PixelBuilderStorageBytes(value, true) <= Limits::MaximumEvaluationBytes;
	}
}
