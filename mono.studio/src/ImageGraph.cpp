#include "ImageGraphCapturedKeyEdit.hpp"
#include "ImageGraphChoices.hpp"
#include "ImageGraphInputs.hpp"
#include "ImageGraphPorts.hpp"
#include "ImageGraphPreview.hpp"
#include "ImageGraphPreviewResult.hpp"

#include <engine/imagegraph/AudioCapture.hpp>
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/SourceKeyframeTransition.hpp>
#include <engine/imagegraph/SourceTimeline.hpp>
#include <engine/imagegraph/SourceTrackTransition.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/imagegraph/WavClip.hpp>
#include <engine/imagegraphio/PxcxAppend.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <new>
#include <nodegraph/Registry.hpp>
#include <stdexcept>
#include <string_view>
#include <studio/ImageGraph.hpp>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace studio {
	namespace {
		using engine::imagegraph::AuthoredValue;
		using engine::imagegraph::Document;
		using engine::imagegraph::Node;

		constexpr std::string_view IMAGE_NODE_TYPES[] = {
			"image.solid",
			"image.gradient",
			"image.noise_simplex",
			"image.tile",
			"image.blend",
			"image.height_blend",
			"image.passthrough",
			"image.flip",
			"image.invert",
			"image.alpha_cutoff",
			"image.offset",
			"image.threshold",
			"image.posterize",
			"image.transform_3d",
			"image.audio_recording",
			"image.audio_volume",
			"value.array",
			"value.array_get"
		};
		constexpr std::string_view IMAGE_PORT_TYPE = "imagegraph.image";

		bool ValidFramesPerSecond(double framesPerSecond) {
			if (!std::isfinite(framesPerSecond) || framesPerSecond <= 0.0) return false;
			const double frameDuration = 1.0 / framesPerSecond;
			return std::isfinite(frameDuration) && frameDuration > 0.0;
		}

		std::string CanvasType(engine::imagegraph::ValueType type) {
			if (type == engine::imagegraph::ValueType::Image) return std::string(IMAGE_PORT_TYPE);
			return "imagegraph." + std::string(engine::imagegraph::ValueTypeName(type));
		}

		std::optional<engine::imagegraph::ValueType> ValueTypeFromCanvas(std::string_view type) {
			if (type == IMAGE_PORT_TYPE) return engine::imagegraph::ValueType::Image;
			constexpr std::string_view PREFIX = "imagegraph.";
			if (!type.starts_with(PREFIX)) return std::nullopt;
			return engine::imagegraph::ParseValueTypeName(type.substr(PREFIX.size()));
		}

		nodegraph::Colour PortTint(engine::imagegraph::ValueType type) {
			using engine::imagegraph::ValueType;
			switch (type) {
			case ValueType::Boolean:
				return nodegraph::Colour::Hex(0xD997FF);
			case ValueType::Integer:
				return nodegraph::Colour::Hex(0xF2C14E);
			case ValueType::Scalar:
				return nodegraph::Colour::Hex(0xFF9F43);
			case ValueType::Text:
				return nodegraph::Colour::Hex(0x7ED6DF);
			case ValueType::Colour:
				return nodegraph::Colour::Hex(0xE06C9F);
			case ValueType::Vector2:
				return nodegraph::Colour::Hex(0x4CA6FF);
			case ValueType::Image:
				return nodegraph::Colour::Hex(0x73D673);
			case ValueType::Array:
				return nodegraph::Colour::Hex(0x54B0B0);
			case ValueType::Gradient:
				return nodegraph::Colour::Hex(0x9B59B6);
			case ValueType::Area:
				return nodegraph::Colour::Hex(0x8E6BBE);
			case ValueType::Curve:
				return nodegraph::Colour::Hex(0x16A085);
			case ValueType::Vector4:
				return nodegraph::Colour::Hex(0x3498DB);
			case ValueType::Path2D:
				return nodegraph::Colour::Hex(0xE67E22);
			case ValueType::Vector3:
				return nodegraph::Colour::Hex(0x4C78A8);
			case ValueType::Quaternion:
				return nodegraph::Colour::Hex(0xF58518);
			case ValueType::Enum:
				return nodegraph::Colour::Hex(0xB279A2);
			case ValueType::Mesh:
				return nodegraph::Colour::Hex(0x79706E);
			case ValueType::AudioBit:
				return nodegraph::Colour::Hex(0xE4D96F);
			default:
				// Runtime-only simulation, 3D and structure sockets share one neutral tint.
				return nodegraph::Colour::Hex(0xA0A0A0);
			}
		}

		std::string NodeTitle(std::string_view type) {
			if (type == "image.solid") return "Solid";
			if (type == "image.gradient") return "Gradient";
			if (type == "image.noise_simplex") return "Simplex Noise";
			if (type == "image.tile") return "Tile";
			if (type == "image.height_blend") return "Height Blend";
			if (type == "image.passthrough") return "Passthrough";
			if (type == "image.flip") return "Flip";
			if (type == "image.invert") return "Invert";
			if (type == "image.alpha_cutoff") return "Alpha Cutoff";
			if (type == "image.blend") return "Blend";
			if (type == "image.offset") return "Offset";
			if (type == "image.threshold") return "Threshold";
			if (type == "image.posterize") return "Posterize";
			if (type == "image.transform_3d") return "Transform Image 3D";
			if (type == "image.audio_recording") return "Recorded Audio";
			if (type == "image.audio_volume") return "Audio Volume";
			if (type == "value.array") return "Array";
			if (type == "value.array_get") return "Array Get";
			const size_t dot = type.find_last_of('.');
			return std::string(dot == std::string_view::npos ? type : type.substr(dot + 1));
		}

		std::string_view NativeImageNodeCategory(std::string_view type) {
			if (type.starts_with("value.")) return "Values";
			if (type.starts_with("image.audio_")) return "Audio";
			if (type == "image.transform_3d") return "3D";
			if (type == "image.solid" || type == "image.gradient" || type == "image.noise_simplex" ||
				type == "image.tile")
				return "Generate";
			if (type == "image.blend" || type == "image.height_blend") return "Composite";
			if (type == "image.flip" || type == "image.invert" || type == "image.alpha_cutoff" ||
				type == "image.offset" || type == "image.threshold" || type == "image.posterize")
				return "Filter";
			if (type == "image.passthrough") return "Utility";
			return "Image";
		}

		void RegisterDataType(engine::imagegraph::ValueType type, const char *label) {
			nodegraph::DataType dataType;
			dataType.Id = CanvasType(type);
			dataType.Label = label;
			dataType.Tint = PortTint(type);
			dataType.Description = std::string("Image graph ") + label + " value";
			nodegraph::DataTypes::Register(dataType);
		}

		void AddUniqueId(
			std::unordered_set<std::string> &issued,
			uint64_t &serial,
			std::string_view prefix,
			std::string &result
		) {
			for (;;) {
				result = std::string(prefix) + std::to_string(serial++);
				if (issued.insert(result).second) return;
			}
		}

		std::vector<AuthoredValue> StarterValues(const Document &document, std::string_view type) {
			const engine::imagegraph::NodeSchema *schema = engine::imagegraph::FindSchema(type);
			if (schema == nullptr) return {};

			std::vector<AuthoredValue> values;
			values.reserve(schema->Properties.size());
			for (const engine::imagegraph::PropertySchema &property : schema->Properties) {
				if (auto value = ImageGraphPropertyDefault(document, type, property.Id)) {
					values.push_back({std::string(property.Id), std::move(*value)});
				}
			}
			return values;
		}

		std::string EnsureDocumentId(ImageGraphCanvasIds &ids, nodegraph::NodeId canvasId) {
			if (const auto found = ids.ToDocument.find(canvasId); found != ids.ToDocument.end()) {
				return found->second;
			}
			std::string documentId;
			AddUniqueId(ids.IssuedNodeIds, ids.NextNodeId, "node-", documentId);
			ids.ToCanvas.emplace(documentId, canvasId);
			ids.ToDocument.emplace(canvasId, documentId);
			return documentId;
		}

		std::string EnsureGroupDocumentId(ImageGraphCanvasIds &ids, nodegraph::GroupId canvasId) {
			if (const auto found = ids.GroupsToDocument.find(canvasId); found != ids.GroupsToDocument.end()) {
				return found->second;
			}
			std::string documentId;
			AddUniqueId(ids.IssuedGroupIds, ids.NextGroupId, "group-", documentId);
			ids.GroupsToCanvas.emplace(documentId, canvasId);
			ids.GroupsToDocument.emplace(canvasId, documentId);
			return documentId;
		}

		bool IsSamePosition(const engine::imagegraph::Vector2 &authored, const nodegraph::Node &canvas) {
			return static_cast<float>(authored.X) == canvas.X && static_cast<float>(authored.Y) == canvas.Y;
		}

		bool HasLink(const nodegraph::Graph &graph, const nodegraph::Link &candidate) {
			return std::find_if(graph.Links().begin(), graph.Links().end(), [&](const nodegraph::Link &link) {
					   return link.From == candidate.From && link.FromPort == candidate.FromPort &&
							  link.To == candidate.To && link.ToPort == candidate.ToPort;
				   }) != graph.Links().end();
		}

		bool HasAuthoredLink(
			const std::vector<engine::imagegraph::Link> &links, const engine::imagegraph::Link &candidate
		) {
			return std::find(links.begin(), links.end(), candidate) != links.end();
		}

		std::optional<engine::imagegraph::ValueType> TypeOf(const engine::imagegraph::Value &value) {
			using engine::imagegraph::ValueType;
			if (std::holds_alternative<bool>(value)) return ValueType::Boolean;
			if (std::holds_alternative<int64_t>(value)) return ValueType::Integer;
			if (std::holds_alternative<double>(value)) return ValueType::Scalar;
			if (std::holds_alternative<std::string>(value)) return ValueType::Text;
			if (std::holds_alternative<engine::imagegraph::Colour>(value)) return ValueType::Colour;
			if (std::holds_alternative<engine::imagegraph::Vector2>(value)) return ValueType::Vector2;
			if (std::holds_alternative<engine::imagegraph::ArrayValue>(value)) return ValueType::Array;
			if (std::holds_alternative<engine::imagegraph::Gradient>(value)) return ValueType::Gradient;
			if (std::holds_alternative<engine::imagegraph::Area>(value)) return ValueType::Area;
			if (std::holds_alternative<engine::imagegraph::Curve>(value)) return ValueType::Curve;
			if (std::holds_alternative<engine::imagegraph::Vector4>(value)) return ValueType::Vector4;
			if (std::holds_alternative<engine::imagegraph::Path2D>(value)) return ValueType::Path2D;
			if (std::holds_alternative<engine::imagegraph::Vector3>(value)) return ValueType::Vector3;
			if (std::holds_alternative<engine::imagegraph::Quaternion>(value)) return ValueType::Quaternion;
			if (std::holds_alternative<engine::imagegraph::EnumValue>(value)) return ValueType::Enum;
			return std::nullopt;
		}

		void PromoteFormatVersion(Document &document) {
			using engine::imagegraph::ValueType;
			uint32_t required = 1;
			for (const Node &node : document.Nodes) {
				if (!node.DynamicInputs.empty()) required = std::max(required, 2u);
				if (!node.DynamicOutputs.empty() || !node.SourceProperties.empty())
					required = std::max(required, 9u);
				for (const AuthoredValue &value : node.Values) {
					if (const auto type = TypeOf(value.Data); type && *type >= ValueType::Gradient)
						required = std::max(required, *type >= ValueType::Vector3 ? 6u : 3u);
				}
				for (const engine::imagegraph::DynamicInput &input : node.DynamicInputs) {
					if (!input.SourceInputId.empty()) required = std::max(required, 9u);
					if (input.Type >= ValueType::Gradient ||
						(input.Default && TypeOf(*input.Default) >= ValueType::Gradient))
						required = std::max(required, input.Type >= ValueType::Vector3 ? 6u : 3u);
				}
			}
			for (const engine::imagegraph::Group &group : document.Groups) {
				if (!group.ParentId.empty() || !group.Ports.empty()) required = std::max(required, 2u);
			}
			if (!document.Junctions.empty()) required = std::max(required, 2u);
			for (const engine::imagegraph::Junction &junction : document.Junctions) {
				if (junction.Type >= ValueType::Gradient ||
					(junction.Default && TypeOf(*junction.Default) >= ValueType::Gradient))
					required = std::max(required, junction.Type >= ValueType::Vector3 ? 6u : 3u);
			}
			for (const engine::imagegraph::Keyframe &keyframe : document.Keyframes) {
				if (const auto type = TypeOf(keyframe.Data); type && *type >= ValueType::Gradient)
					required = std::max(required, *type >= ValueType::Vector3 ? 6u : 3u);
				if (keyframe.Ease || keyframe.Interpolation == "source") required = std::max(required, 4u);
				if (keyframe.SineDriver) required = std::max(required, 6u);
				if (keyframe.SourceDriver) required = std::max(required, 8u);
				if (keyframe.Subframe != 0 || keyframe.NegativeFrame ||
					keyframe.Kind == engine::imagegraph::KeyframeKind::Adder || !keyframe.SourceKeyId.empty())
					required = std::max(required, 9u);
			}
			if (!document.Tracks.empty()) required = std::max(required, 4u);
			for (const auto &track : document.Tracks)
				if (track.QuaternionMode) required = std::max(required, 8u);
			if (document.Timeline) required = std::max(required, 5u);
			if (document.Timeline && document.Timeline->SourceBounds) required = 9;
			if (document.Project) {
				required = std::max(required, 7u);
				const auto &project = *document.Project;
				if (project.PreviewGrid != engine::imagegraph::PreviewGridSettings{} ||
					!project.PreviewRulers.empty() || project.ShowPreviewRulers)
					required = std::max(required, 9u);
			}
			document.FormatVersion = std::max(document.FormatVersion, required);
		}

		void SetTrackInterpolation(
			Document &document, std::string_view nodeId, std::string_view port, std::string_view rule
		) {
			for (engine::imagegraph::Keyframe &keyframe : document.Keyframes) {
				if (keyframe.NodeId != nodeId || keyframe.Port != port) continue;
				keyframe.Interpolation = rule;
				if (rule == "source") {
					if (!keyframe.Ease) keyframe.Ease = engine::imagegraph::KeyframeEase{};
				} else {
					keyframe.Ease.reset();
				}
			}
			PromoteFormatVersion(document);
		}

		void SetDiagnostic(
			engine::imagegraph::Diagnostic &diagnostic,
			engine::imagegraph::Status code,
			std::string_view nodeId,
			std::string_view port,
			std::string message
		) {
			diagnostic = {code, std::string(nodeId), std::string(port), std::move(message)};
		}

		bool ValidateCapturedEditBudget(
			uint64_t availableBytes,
			engine::imagegraph::Diagnostic &diagnostic,
			std::string_view nodeId = {},
			std::string_view port = {}
		) {
			if (availableBytes && availableBytes <= engine::imagegraph::Limits::MaximumEvaluationBytes)
				return true;
			SetDiagnostic(
				diagnostic,
				engine::imagegraph::Status::LimitExceeded,
				nodeId,
				port,
				"captured edit transaction budget is outside bounds"
			);
			return false;
		}

		bool AdmitCapturedTrackDraft(
			const Document &document,
			const engine::imagegraph::AnimationTrack *existing,
			std::string_view nodeId,
			std::string_view port,
			uint64_t availableBytes,
			engine::imagegraph::Diagnostic &error,
			uint64_t borrowedBytes = 0
		) {
			using namespace engine::imagegraph;
			if (!ValidateCapturedEditBudget(availableBytes, error, nodeId, port)) return false;
			const auto resident = DocumentRetainedPayloadBytes(document);
			uint64_t remaining = availableBytes;
			const auto admit = [&](uint64_t bytes) {
				if (bytes > remaining) return false;
				remaining -= bytes;
				return true;
			};
			const auto text = [&](std::string_view value) {
				return value.size() <= Limits::MaximumTextBytes &&
					   admit(std::max(value.size(), std::string{}.capacity()) + 1);
			};
			if (resident && admit(*resident) && admit(borrowedBytes) && admit(sizeof(AnimationTrack)) &&
				text(existing ? std::string_view(existing->NodeId) : nodeId) &&
				text(existing ? std::string_view(existing->Port) : port) &&
				text(existing ? std::string_view(existing->End) : std::string_view("hold")))
				return true;
			SetDiagnostic(
				error, Status::LimitExceeded, nodeId, port, "captured track draft exceeds the payload budget"
			);
			return false;
		}

		bool EnsureSourceAnimationTrack(
			Document &document,
			std::string_view nodeId,
			std::string_view port,
			engine::imagegraph::Diagnostic &error
		) {
			const auto found =
				std::find_if(document.Tracks.begin(), document.Tracks.end(), [&](const auto &track) {
					return track.NodeId == nodeId && track.Port == port;
				});
			if (found != document.Tracks.end()) return true;
			if (document.Tracks.size() >= engine::imagegraph::Limits::MaximumTracks) {
				SetDiagnostic(
					error,
					engine::imagegraph::Status::LimitExceeded,
					nodeId,
					port,
					"animation track limit reached"
				);
				return false;
			}
			document.Tracks.push_back({std::string(nodeId), std::string(port), "hold", -1});
			PromoteFormatVersion(document);
			return true;
		}

		const Node *FindAuthoredNode(const Document &document, std::string_view id) {
			const auto found =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
					return node.Id == id;
				});
			return found == document.Nodes.end() ? nullptr : &*found;
		}

		Node *FindAuthoredNode(Document &document, std::string_view id) {
			const auto found =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
					return node.Id == id;
				});
			return found == document.Nodes.end() ? nullptr : &*found;
		}
	}

	namespace {
		bool ReadPreviewAudioFile(
			const std::filesystem::path &path,
			uint64_t maximumBytes,
			std::vector<std::byte> &bytes,
			engine::imagegraph::Diagnostic &diagnostic
		) {
			using engine::imagegraph::Status;
			const auto fail = [&](Status code, const char *message) {
				diagnostic = {code, {}, {}, message};
				return false;
			};
			if (path.empty()) return fail(Status::InvalidValue, "enter an audio file path");
			std::ifstream file(path, std::ios::binary | std::ios::ate);
			if (!file) return fail(Status::InvalidValue, "could not open audio file");
			const std::streamoff length = file.tellg();
			if (length < 0 || static_cast<uint64_t>(length) > maximumBytes)
				return fail(Status::LimitExceeded, "audio file exceeds the preview read budget");
			bytes.resize(static_cast<size_t>(length));
			file.seekg(0, std::ios::beg);
			if (!bytes.empty()) {
				file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
				if (file.gcount() != static_cast<std::streamsize>(bytes.size()))
					return fail(Status::InvalidValue, "could not read the complete audio file");
			}
			return true;
		}
	}

	bool LoadImageGraphAudioCapture(
		std::vector<engine::imagegraph::AudioCaptureFrame> &frames,
		ImageGraphPreviewCache &cache,
		const std::filesystem::path &filePath,
		engine::imagegraph::Diagnostic &diagnostic
	) {
		using namespace engine::imagegraph;
		diagnostic = {};
		std::vector<std::byte> bytes;
		if (!ReadPreviewAudioFile(filePath, Limits::MaximumAudioCaptureDocumentBytes, bytes, diagnostic))
			return false;
		std::vector<AudioCaptureFrame> candidate;
		const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
		if (ReadAudioCapture(text, candidate, diagnostic) != Status::Ok) return false;
		frames = std::move(candidate);
		cache.Clear();
		return true;
	}

	void ClearImageGraphAudioCapture(
		std::vector<engine::imagegraph::AudioCaptureFrame> &frames, ImageGraphPreviewCache &cache
	) {
		frames.clear();
		cache.Clear();
	}

	bool ReadImageGraphWavSource(
		std::span<const engine::imagegraph::AudioClipSource> sources,
		std::string_view sourceId,
		const std::filesystem::path &filePath,
		engine::imagegraph::AudioClipSource &loaded,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t byteBudget
	) {
		using namespace engine::imagegraph;
		diagnostic = {};
		const auto fail = [&](Status code, const char *message) {
			diagnostic = {code, {}, {}, message};
			return false;
		};
		if (sourceId.empty() || sourceId.size() > Limits::MaximumTextBytes || filePath.empty())
			return fail(Status::InvalidValue, "enter a source name and WAV file path");
		if (sources.size() > Limits::MaximumNodes || byteBudget > Limits::MaximumEvaluationBytes)
			return fail(Status::LimitExceeded, "WAV sources exceed the preview input budget");
		size_t replacement = sources.size();
		uint64_t retainedBytes = 0;
		const auto retain = [&](uint64_t bytes) {
			if (bytes > byteBudget - retainedBytes) return false;
			retainedBytes += bytes;
			return true;
		};
		for (size_t index = 0; index < sources.size(); index++) {
			const auto &source = sources[index];
			if (source.SourceId == sourceId) {
				if (replacement != sources.size())
					return fail(Status::DuplicateId, "WAV source names must be unique");
				replacement = index;
			}
			if (source.Data.Channels.size() > Limits::MaximumAudioChannels ||
				!retain(sizeof(AudioClipSource)) || !retain(source.SourceId.size()) ||
				!retain(source.Data.Channels.size() * sizeof(std::vector<double>)))
				return fail(Status::LimitExceeded, "WAV sources exceed the preview input budget");
			size_t samples = source.Data.Samples.size();
			if (samples > Limits::MaximumAudioClipSamples || !retain(samples * sizeof(double)))
				return fail(Status::LimitExceeded, "WAV source exceeds the sample budget");
			for (const auto &plane : source.Data.Channels) {
				if (plane.size() > Limits::MaximumAudioClipSamples - samples ||
					!retain(plane.size() * sizeof(double)))
					return fail(Status::LimitExceeded, "WAV source exceeds the sample budget");
				samples += plane.size();
			}
		}
		if (replacement == sources.size() && sources.size() == Limits::MaximumNodes)
			return fail(Status::LimitExceeded, "too many WAV sources");
		const uint64_t assetOverhead = sizeof(AudioClipSource) + sourceId.size();
		if (assetOverhead > byteBudget - retainedBytes)
			return fail(Status::LimitExceeded, "WAV sources exceed the preview input budget");
		constexpr uint64_t maximumFileBytes = 16 * 1024 * 1024;
		const uint64_t remainingBytes = byteBudget - retainedBytes - assetOverhead;
		std::vector<std::byte> bytes;
		if (!ReadPreviewAudioFile(filePath, std::min(maximumFileBytes, remainingBytes), bytes, diagnostic))
			return false;
		AudioBit candidate;
		// The old clip stays resident until conversion succeeds, so replacement has a peak budget too.
		if (DecodeWavClip(
				bytes, WavClipPolicy::PixelComposer, remainingBytes - bytes.size(), candidate, diagnostic
			) != Status::Ok)
			return false;
		loaded = {std::string(sourceId), std::move(candidate)};
		return true;
	}

	bool LoadImageGraphWavSource(
		std::vector<engine::imagegraph::AudioClipSource> &sources,
		ImageGraphPreviewCache &cache,
		std::string_view sourceId,
		const std::filesystem::path &filePath,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t byteBudget
	) {
		engine::imagegraph::AudioClipSource candidate;
		if (!ReadImageGraphWavSource(sources, sourceId, filePath, candidate, diagnostic, byteBudget))
			return false;
		const auto old = std::find_if(sources.begin(), sources.end(), [&](const auto &source) {
			return source.SourceId == sourceId;
		});
		if (old == sources.end())
			sources.push_back(std::move(candidate));
		else
			*old = std::move(candidate);
		cache.Clear();
		return true;
	}

	bool RemoveImageGraphWavSource(
		std::vector<engine::imagegraph::AudioClipSource> &sources,
		ImageGraphPreviewCache &cache,
		std::string_view sourceId
	) {
		const auto found = std::find_if(sources.begin(), sources.end(), [&](const auto &source) {
			return source.SourceId == sourceId;
		});
		if (found == sources.end()) return false;
		sources.erase(found);
		cache.Clear();
		return true;
	}

	const engine::imagegraph::Image *ImageGraphPreviewCache::Find(
		uint64_t revision,
		size_t outputIndex,
		uint64_t tick,
		double subframe,
		bool negativeFrame,
		uint8_t rigidObservation
	) {
		if (rigidObservation > 3 || !engine::imagegraph::ValidFrameTime({tick, subframe, negativeFrame}))
			return nullptr;
		const auto found = std::find_if(Entries.begin(), Entries.end(), [&](const Entry &entry) {
			return entry.Revision == revision && entry.OutputIndex == outputIndex && entry.Tick == tick &&
				   entry.Subframe == subframe && entry.NegativeFrame == negativeFrame &&
				   entry.RigidObservation == rigidObservation;
		});
		if (found == Entries.end()) return nullptr;
		found->LastUsed = ++UseSerial;
		return &found->Image;
	}

	bool ImageGraphPreviewCache::Store(
		uint64_t revision,
		size_t outputIndex,
		uint64_t tick,
		const engine::imagegraph::Image &image,
		double subframe,
		bool negativeFrame,
		uint8_t rigidObservation
	) {
		if (rigidObservation > 3 || !engine::imagegraph::ValidFrameTime({tick, subframe, negativeFrame}) ||
			!engine::imagegraph::ValidSurfaceLayout(
				image, IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION, IMAGE_COMPOSER_PREVIEW_SURFACE_MAXIMUM_BYTES
			) ||
			!engine::imagegraph::FiniteSurfaceSamples(image))
			return false;

		auto found = std::find_if(Entries.begin(), Entries.end(), [&](const Entry &entry) {
			return entry.Revision == revision && entry.OutputIndex == outputIndex && entry.Tick == tick &&
				   entry.Subframe == subframe && entry.NegativeFrame == negativeFrame &&
				   entry.RigidObservation == rigidObservation;
		});
		size_t heldBytes = HeldBytes();
		size_t oldCapacity = found == Entries.end() ? 0 : found->Image.Pixels.capacity();
		if (heldBytes > IMAGE_COMPOSER_PREVIEW_CACHE_MAXIMUM_BYTES ||
			heldBytes > IMAGE_COMPOSER_PREVIEW_CACHE_PEAK_BYTES)
			return false;

		try {
			engine::imagegraph::Image candidate;
			candidate.Width = image.Width;
			candidate.Height = image.Height;
			candidate.Hash = image.Hash;
			candidate.Format = image.Format;
			candidate.Pixels.reserve(image.Pixels.size());
			const size_t candidateCapacity = candidate.Pixels.capacity();
			if (candidateCapacity > IMAGE_COMPOSER_PREVIEW_SURFACE_MAXIMUM_BYTES ||
				candidateCapacity > IMAGE_COMPOSER_PREVIEW_CACHE_PEAK_BYTES - heldBytes)
				return false;
			candidate.Pixels.assign(image.Pixels.begin(), image.Pixels.end());

			if (found != Entries.end()) {
				size_t projectedBytes = heldBytes - oldCapacity + candidateCapacity;
				while (projectedBytes > IMAGE_COMPOSER_PREVIEW_CACHE_MAXIMUM_BYTES) {
					auto victim = Entries.end();
					for (auto entry = Entries.begin(); entry != Entries.end(); ++entry) {
						if (entry == found) continue;
						if (victim == Entries.end() || entry->LastUsed < victim->LastUsed) victim = entry;
					}
					if (victim == Entries.end()) return false;
					projectedBytes -= victim->Image.Pixels.capacity();
					Entries.erase(victim);
					found = std::find_if(Entries.begin(), Entries.end(), [&](const Entry &entry) {
						return entry.Revision == revision && entry.OutputIndex == outputIndex &&
							   entry.Tick == tick && entry.Subframe == subframe &&
							   entry.NegativeFrame == negativeFrame &&
							   entry.RigidObservation == rigidObservation;
					});
				}
			} else {
				size_t projectedBytes = heldBytes + candidateCapacity;
				while (Entries.size() >= IMAGE_COMPOSER_PREVIEW_CACHE_ENTRIES ||
					   projectedBytes > IMAGE_COMPOSER_PREVIEW_CACHE_MAXIMUM_BYTES) {
					const auto victim = std::min_element(
						Entries.begin(), Entries.end(), [](const Entry &left, const Entry &right) {
							return left.LastUsed < right.LastUsed;
						}
					);
					if (victim == Entries.end()) return false;
					projectedBytes -= victim->Image.Pixels.capacity();
					Entries.erase(victim);
				}
				Entries.emplace_back();
				found = Entries.end() - 1;
			}
			found->Revision = revision;
			found->OutputIndex = outputIndex;
			found->Tick = tick;
			found->Subframe = subframe;
			found->NegativeFrame = negativeFrame;
			found->RigidObservation = rigidObservation;
			found->LastUsed = ++UseSerial;
			found->Image = std::move(candidate);
			// The preflight projection includes the candidate's actual capacity.
			return true;
		} catch (const std::bad_alloc &) {
			return false;
		} catch (const std::length_error &) {
			return false;
		}
	}

	void ImageGraphPreviewCache::InvalidateOutput(size_t outputIndex) {
		std::erase_if(Entries, [&](const Entry &entry) { return entry.OutputIndex == outputIndex; });
	}

	void ImageGraphPreviewCache::Clear() {
		Entries.clear();
		UseSerial = 0;
	}

	size_t ImageGraphPreviewCache::HeldBytes() const {
		size_t bytes = 0;
		for (const Entry &entry : Entries)
			bytes += entry.Image.Pixels.capacity();
		return bytes;
	}

	uint64_t ImageGraphPreviewCache::RetainedBytes() const noexcept {
		if (Entries.capacity() > (UINT64_MAX - sizeof(*this)) / sizeof(Entry)) return UINT64_MAX;
		uint64_t bytes = sizeof(*this) + Entries.capacity() * sizeof(Entry);
		for (const auto &entry : Entries) {
			if (entry.Image.Pixels.capacity() > UINT64_MAX - bytes) return UINT64_MAX;
			bytes += entry.Image.Pixels.capacity();
		}
		return bytes;
	}

	engine::imagegraph::FrameTime GetImageGraphFrame(const ImageGraphPlayback &playback) {
		return {playback.CurrentTick, playback.Subframe, playback.NegativeFrame};
	}
	bool SetImageGraphAuthorFrame(ImageGraphPlayback &playback, engine::imagegraph::FrameTime frame) {
		if (!engine::imagegraph::ValidFrameTime(frame) || GetImageGraphFrame(playback) == frame) return false;
		playback.CurrentTick = frame.Tick;
		playback.Subframe = frame.Subframe;
		playback.NegativeFrame = frame.NegativeFrame;
		playback.RealFrame = double(engine::imagegraph::FrameTimeToReal(frame));
		playback.FrameProgress = true;
		playback.LastTime = 0;
		playback.Accumulator = 0;
		return true;
	}
	bool SeekImageGraphAuthorFrame(ImageGraphPlayback &playback, double frame, bool control, bool alt) {
		engine::imagegraph::FrameTime selected;
		if (!std::isfinite(frame) || playback.TotalFrames == 0 ||
			playback.TotalFrames > engine::imagegraph::Limits::MaximumTick + 1)
			return false;
		if (!control) frame = std::clamp(frame, 0.0, static_cast<double>(playback.TotalFrames - 1));
		if (!engine::imagegraph::SplitFrameTime(frame, selected, !alt)) return false;
		return SetImageGraphAuthorFrame(playback, selected);
	}

	void ApplyImageGraphTimeline(const Document &document, ImageGraphPlayback &playback) {
		playback.Playing = false;
		playback.Rendering = false;
		playback.FrameProgress = false;
		playback.LastTime = playback.RealTime = 0;
		playback.Accumulator = 0.0;
		playback.Direction = 1;
		playback.Subframe = 0.0;
		playback.NegativeFrame = false;
		if (document.Timeline) {
			const engine::imagegraph::TimelineSettings &timeline = *document.Timeline;
			engine::imagegraph::TimelineSettings projection = timeline;
			engine::imagegraph::Diagnostic diagnostic;
			if (engine::imagegraph::ProjectSourceTimelineWindow(projection, diagnostic) !=
				engine::imagegraph::Status::Ok) {
				projection.First = 0;
				projection.Last = timeline.Frames ? timeline.Frames - 1 : 0;
			}
			playback.TotalFrames = timeline.Frames;
			playback.StartTick = projection.First;
			playback.EndTick = projection.Last;
			playback.SourceBounds = timeline.SourceBounds;
			playback.FramesPerSecond = timeline.FramesPerSecond;
			playback.Loop = timeline.Playback == "loop";
			playback.PingPong = timeline.Playback == "pingpong";
		} else {
			playback.TotalFrames = 241;
			playback.StartTick = 0;
			playback.EndTick = 240;
			playback.FramesPerSecond = 30.0;
			playback.Loop = true;
			playback.PingPong = false;
			playback.SourceBounds.reset();
		}
		if (!ValidFramesPerSecond(playback.FramesPerSecond)) playback.FramesPerSecond = 30.0;
		const uint64_t maximumFrames = engine::imagegraph::Limits::MaximumTick + 1;
		playback.TotalFrames = std::clamp(playback.TotalFrames, uint64_t{1}, maximumFrames);
		playback.StartTick = std::min(playback.StartTick, playback.TotalFrames - 1);
		playback.EndTick = std::clamp(playback.EndTick, playback.StartTick, playback.TotalFrames - 1);
		playback.CurrentTick = std::clamp(playback.CurrentTick, playback.StartTick, playback.EndTick);
		playback.RealFrame = double(playback.CurrentTick);
	}

	bool SetImageGraphPlaybackFrame(ImageGraphPlayback &playback, double frame) {
		if (!std::isfinite(frame)) return false;
		const uint64_t maximumFrames = engine::imagegraph::Limits::MaximumTick + 1;
		playback.TotalFrames = std::clamp(playback.TotalFrames, uint64_t{1}, maximumFrames);
		playback.StartTick = std::min(playback.StartTick, playback.TotalFrames - 1);
		playback.EndTick = std::clamp(playback.EndTick, playback.StartTick, playback.TotalFrames - 1);
		frame =
			std::clamp(frame, static_cast<double>(playback.StartTick), static_cast<double>(playback.EndTick));
		engine::imagegraph::FrameTime selected;
		if (!engine::imagegraph::SplitFrameTime(frame, selected)) return false;
		return SetImageGraphAuthorFrame(playback, selected);
	}

	bool AdvanceImageGraphPlayback(ImageGraphPlayback &playback, double elapsedSeconds) {
		if (!ValidFramesPerSecond(playback.FramesPerSecond)) playback.FramesPerSecond = 30.0;
		if (!playback.Playing) return false;
		const auto before = GetImageGraphFrame(playback);
		if (playback.NegativeFrame) {
			playback.CurrentTick = playback.StartTick;
			playback.Subframe = 0;
			playback.NegativeFrame = false;
		}
		const uint64_t maximumFrames = engine::imagegraph::Limits::MaximumTick + 1;
		playback.TotalFrames = std::clamp(playback.TotalFrames, uint64_t{1}, maximumFrames);
		playback.StartTick = std::min(playback.StartTick, playback.TotalFrames - 1);
		playback.EndTick = std::clamp(playback.EndTick, playback.StartTick, playback.TotalFrames - 1);
		playback.CurrentTick = std::clamp(playback.CurrentTick, playback.StartTick, playback.EndTick);
		if (!std::isfinite(playback.Subframe) || playback.Subframe < 0.0 || playback.Subframe >= 1.0)
			playback.Subframe = 0.0;
		if (playback.Direction != -1 && playback.Direction != 1) playback.Direction = 1;
		if (!std::isfinite(playback.Accumulator) || playback.Accumulator < 0.0) playback.Accumulator = 0.0;
		if (!playback.Playing) return false;
		playback.Accumulator += std::isfinite(elapsedSeconds) ? std::clamp(elapsedSeconds, 0.0, 0.25) : 0.0;
		const double frameDuration = 1.0 / playback.FramesPerSecond;
		for (size_t step = 0; step < 8 && playback.Accumulator >= frameDuration; step++) {
			playback.Accumulator -= frameDuration;
			if (playback.PingPong) {
				if (playback.Direction > 0 && playback.CurrentTick >= playback.EndTick) {
					playback.Direction = -1;
					if (playback.CurrentTick > playback.StartTick) playback.CurrentTick--;
				} else if (playback.Direction < 0 && playback.CurrentTick <= playback.StartTick) {
					playback.Direction = 1;
					if (playback.CurrentTick < playback.EndTick) playback.CurrentTick++;
				} else if (playback.Direction > 0) {
					playback.CurrentTick++;
				} else if (playback.CurrentTick > playback.StartTick) {
					playback.CurrentTick--;
				}
			} else if (playback.CurrentTick >= playback.EndTick) {
				if (playback.Loop)
					playback.CurrentTick = playback.StartTick;
				else {
					playback.CurrentTick = playback.EndTick;
					playback.Playing = false;
					playback.Accumulator = 0.0;
					break;
				}
			} else {
				playback.CurrentTick++;
			}
		}
		if (playback.CurrentTick == playback.EndTick) playback.Subframe = 0.0;
		return GetImageGraphFrame(playback) != before;
	}

	void RegisterImageGraphNodeTypes() {
		nodegraph::DataType opaque;
		opaque.Id = "pxcx.opaque";
		opaque.Label = "Opaque PXCX";
		opaque.Tint = nodegraph::Colour::Hex(0x858585);
		opaque.Description = "Positional archive connection; native execution is unavailable";
		nodegraph::DataTypes::Register(opaque);
		RegisterDataType(engine::imagegraph::ValueType::Boolean, "Boolean");
		RegisterDataType(engine::imagegraph::ValueType::Integer, "Integer");
		RegisterDataType(engine::imagegraph::ValueType::Scalar, "Scalar");
		RegisterDataType(engine::imagegraph::ValueType::Text, "Text");
		RegisterDataType(engine::imagegraph::ValueType::Colour, "Colour");
		RegisterDataType(engine::imagegraph::ValueType::Vector2, "Vector 2");
		RegisterDataType(engine::imagegraph::ValueType::Image, "Image");
		RegisterDataType(engine::imagegraph::ValueType::Array, "Array");
		RegisterDataType(engine::imagegraph::ValueType::Gradient, "Gradient");
		RegisterDataType(engine::imagegraph::ValueType::Area, "Area");
		RegisterDataType(engine::imagegraph::ValueType::Curve, "Curve");
		RegisterDataType(engine::imagegraph::ValueType::Vector4, "Vector 4");
		RegisterDataType(engine::imagegraph::ValueType::Path2D, "Path 2D");
		RegisterDataType(engine::imagegraph::ValueType::Vector3, "Vector 3");
		RegisterDataType(engine::imagegraph::ValueType::Quaternion, "Quaternion");
		RegisterDataType(engine::imagegraph::ValueType::Enum, "Enum");
		RegisterDataType(engine::imagegraph::ValueType::Mesh, "Mesh");
		RegisterDataType(engine::imagegraph::ValueType::AudioBit, "Audio");
		for (auto index = static_cast<size_t>(engine::imagegraph::ValueType::Mesh2D);
			 index <= static_cast<size_t>(engine::imagegraph::ValueType::Path3D);
			 index++) {
			const auto type = static_cast<engine::imagegraph::ValueType>(index);
			RegisterDataType(type, std::string(engine::imagegraph::ValueTypeName(type)).c_str());
		}

		const auto registerSchema =
			[](const engine::imagegraph::NodeSchema &schema, std::string title, std::string category) {
				nodegraph::NodeType type;
				type.Id = std::string(schema.Type);
				type.Title = schema.Type == "pc.graph_preview" ? "Image Preview" : std::move(title);
				type.Category = std::move(category);
				type.Accent = nodegraph::Colour::Hex(0x262626);
				for (const engine::imagegraph::PortSchema &port : schema.Ports) {
					nodegraph::PortSpec socket{std::string(port.Id), CanvasType(port.Type)};
					if (port.Direction == engine::imagegraph::PortDirection::Input) {
						socket.Suggest =
							imagegraph_choices::Suggested(imagegraph_choices::Input(schema.Type, port.Id));
						type.Inputs.push_back(std::move(socket));
					} else {
						type.Outputs.push_back(std::move(socket));
					}
				}
				nodegraph::NodeTypes::Register(type);
			};
		for (const std::string_view id : IMAGE_NODE_TYPES) {
			if (const engine::imagegraph::NodeSchema *schema = engine::imagegraph::FindSchema(id))
				registerSchema(
					*schema, NodeTitle(schema->Type), std::string(NativeImageNodeCategory(schema->Type))
				);
		}
		// Every source catalogue node is searchable under its documentation family, native executor or not.
		for (const engine::imagegraph::CatalogueEntry &entry : engine::imagegraph::Catalogue())
			registerSchema(
				entry.Schema, std::string(entry.Title), "Pixel Composer/" + std::string(entry.Family)
			);
	}

	std::optional<engine::imagegraph::Value>
	ImageGraphPropertyDefault(std::string_view nodeType, std::string_view propertyId) {
		const engine::imagegraph::NodeSchema *schema = engine::imagegraph::FindSchema(nodeType);
		if (schema == nullptr) return std::nullopt;
		const auto property =
			std::find_if(schema->Properties.begin(), schema->Properties.end(), [&](const auto &candidate) {
				return candidate.Id == propertyId;
			});
		if (property == schema->Properties.end()) return std::nullopt;

		using engine::imagegraph::ValueType;
		if (const auto *entry = engine::imagegraph::FindCatalogueEntry(nodeType)) {
			const auto *input = engine::imagegraph::FindCatalogueInput(*entry, propertyId);
			if (input != nullptr) {
				if (auto value = engine::imagegraph::CatalogueDefault(*input)) return value;
			}
		}
		if (nodeType == "image.transform_3d") {
			if (propertyId == "position" || propertyId == "anchor")
				return engine::imagegraph::Value{engine::imagegraph::Vector3{}};
			if (propertyId == "rotation") return engine::imagegraph::Value{engine::imagegraph::Quaternion{}};
			if (propertyId == "scale")
				return engine::imagegraph::Value{engine::imagegraph::Vector3{1.0, 1.0, 1.0}};
			if (propertyId == "texture_tiling")
				return engine::imagegraph::Value{engine::imagegraph::Vector2{1.0, 1.0}};
			if (propertyId == "projection")
				return engine::imagegraph::Value{engine::imagegraph::EnumValue{1}};
			if (propertyId == "fov") return engine::imagegraph::Value{45.0};
			if (propertyId == "view_range")
				return engine::imagegraph::Value{engine::imagegraph::Vector2{0.001, 10.0}};
			if (propertyId == "depth_range")
				return engine::imagegraph::Value{engine::imagegraph::Vector2{0.0, 1.0}};
		}
		if (nodeType == "image.audio_recording" && propertyId == "source_id")
			return engine::imagegraph::Value{std::string{"mono"}};
		if (propertyId == "width" || propertyId == "height") return engine::imagegraph::Value{int64_t{64}};
		if (propertyId == "colour")
			return engine::imagegraph::Value{engine::imagegraph::Colour{255, 255, 255, 255}};
		if (nodeType == "image.posterize" && propertyId == "palette")
			return engine::imagegraph::Value{
				engine::imagegraph::ArrayValue{ValueType::Colour, {engine::imagegraph::Colour{0, 0, 0, 255}}}
			};
		if (propertyId == "use_mask_dimension") return engine::imagegraph::Value{true};
		if (propertyId == "mode") return engine::imagegraph::Value{int64_t{0}};
		if (propertyId == "type") return engine::imagegraph::Value{int64_t{1}};
		if (propertyId == "axis") return engine::imagegraph::Value{int64_t{1}};
		if (propertyId == "channel") return engine::imagegraph::Value{int64_t{15}};
		if (propertyId == "mask_feather")
			return engine::imagegraph::Value{nodeType == "image.blend" ? 1.0 : 0.0};
		if (propertyId == "mix") return engine::imagegraph::Value{1.0};
		if (propertyId == "factor") return engine::imagegraph::Value{0.5};
		if (propertyId == "minimum") return engine::imagegraph::Value{0.5};
		if (propertyId == "index" || propertyId == "overflow") return engine::imagegraph::Value{int64_t{0}};
		if (propertyId == "swap" || propertyId == "preserve_alpha" || propertyId == "mask_alpha_only")
			return engine::imagegraph::Value{false};
		if (propertyId == "constant_dimension")
			return engine::imagegraph::Value{engine::imagegraph::Vector2{64, 64}};
		if (propertyId == "position") return engine::imagegraph::Value{engine::imagegraph::Vector2{0.5, 0.5}};
		if (propertyId == "opacity") return engine::imagegraph::Value{1.0};
		if (propertyId == "iterations") return engine::imagegraph::Value{int64_t{1}};
		if (propertyId == "steps") return engine::imagegraph::Value{int64_t{4}};
		if (propertyId == "adaptive_radius") return engine::imagegraph::Value{int64_t{4}};
		if (propertyId == "gamma" || propertyId == "iteration_scaling" || propertyId == "iteration_amplitude")
			return engine::imagegraph::Value{1.0};
		if (propertyId == "brightness_threshold" || propertyId == "alpha_threshold")
			return engine::imagegraph::Value{0.5};
		if (propertyId == "scale") return engine::imagegraph::Value{engine::imagegraph::Vector2{1.0, 1.0}};
		if (propertyId == "center") return engine::imagegraph::Value{engine::imagegraph::Vector2{0.5, 0.5}};
		if (propertyId == "shape" || propertyId == "amount")
			return engine::imagegraph::Value{engine::imagegraph::Vector2{1.0, 1.0}};
		if (propertyId == "level_in" || propertyId == "level_out" || propertyId == "color_range_r" ||
			propertyId == "color_range_g" || propertyId == "color_range_b")
			return engine::imagegraph::Value{engine::imagegraph::Vector2{0.0, 1.0}};
		switch (property->Type) {
		case ValueType::Boolean:
			return engine::imagegraph::Value{false};
		case ValueType::Integer:
			return engine::imagegraph::Value{int64_t{0}};
		case ValueType::Scalar:
			return engine::imagegraph::Value{0.0};
		case ValueType::Text:
			return engine::imagegraph::Value{std::string{}};
		case ValueType::Colour:
			return engine::imagegraph::Value{engine::imagegraph::Colour{0, 0, 0, 255}};
		case ValueType::Vector2:
			return engine::imagegraph::Value{engine::imagegraph::Vector2{}};
		case ValueType::Image:
			return std::nullopt;
		case ValueType::Array:
			return engine::imagegraph::Value{engine::imagegraph::ArrayValue{ValueType::Integer, {}}};
		case ValueType::Gradient:
			return engine::imagegraph::Value{engine::imagegraph::Gradient{
				0,
				{{0.0, engine::imagegraph::Colour{0, 0, 0, 255}},
				 {1.0, engine::imagegraph::Colour{255, 255, 255, 255}}}
			}};
		case ValueType::Area:
			return engine::imagegraph::Value{engine::imagegraph::Area{}};
		case ValueType::Curve:
			return engine::imagegraph::Value{engine::imagegraph::Curve{{}, {{}, {}}}};
		case ValueType::Vector4:
			return engine::imagegraph::Value{engine::imagegraph::Vector4{}};
		case ValueType::Path2D:
			return engine::imagegraph::Value{engine::imagegraph::Path2D{}};
		case ValueType::Vector3:
			return engine::imagegraph::Value{engine::imagegraph::Vector3{}};
		case ValueType::Quaternion:
			return engine::imagegraph::Value{engine::imagegraph::Quaternion{}};
		case ValueType::Enum:
			return engine::imagegraph::Value{engine::imagegraph::EnumValue{}};
		default:
			// Meshes, audio and runtime-only sockets have no authored default.
			return std::nullopt;
		}
	}

	std::optional<engine::imagegraph::Value> ImageGraphPropertyDefault(
		const Document &document, std::string_view nodeType, std::string_view property
	) {
		// nodeValue_Palette clones PROJ_PALETTE for these source inputs at creation.
		const bool projectPalette =
			(nodeType == "pc.gradient_palette" && property == "palette") ||
			(nodeType == "pc.gradient_replace_color" && (property == "color_from" || property == "color_to"));
		if (projectPalette) {
			engine::imagegraph::ArrayValue palette;
			palette.ElementType = engine::imagegraph::ValueType::Colour;
			const engine::imagegraph::ProjectSettings defaults;
			const engine::imagegraph::ProjectSettings &project =
				document.Project ? *document.Project : defaults;
			const auto &colours = project.Palette;
			if (colours.size() > engine::imagegraph::Limits::MaximumProjectPaletteEntries)
				return std::nullopt;
			palette.Elements.reserve(colours.size());
			for (const auto &colour : colours)
				palette.Elements.push_back(colour);
			return engine::imagegraph::Value{std::move(palette)};
		}
		return ImageGraphPropertyDefault(nodeType, property);
	}

	bool LoadImageGraphCanvas(
		const engine::imagegraph::Document &document,
		nodegraph::Graph &graph,
		ImageGraphCanvasIds &ids,
		std::string &error
	) {
		graph.Clear();
		ids = {};
		error.clear();
		RegisterImageGraphNodeTypes();

		if (document.Nodes.size() > engine::imagegraph::Limits::MaximumNodes ||
			document.Links.size() > engine::imagegraph::Limits::MaximumLinks ||
			document.Groups.size() > engine::imagegraph::Limits::MaximumGroups ||
			document.Junctions.size() > engine::imagegraph::Limits::MaximumJunctions ||
			document.Outputs.size() > engine::imagegraph::Limits::MaximumOutputs ||
			document.Keyframes.size() > engine::imagegraph::Limits::MaximumKeyframes ||
			document.Nodes.size() >= std::numeric_limits<nodegraph::NodeId>::max()) {
			error = "image graph exceeds canvas limits";
			return false;
		}

		// grug keep foreign interfaces on each candidate node, never in the shared type table.
		using PhysicalPorts = std::pair<std::vector<nodegraph::PortSpec>, std::vector<nodegraph::PortSpec>>;
		std::unordered_map<std::string, PhysicalPorts> opaquePorts;
		for (const auto &node : document.Nodes) {
			if (!node.Type.starts_with("pxcx.opaque/")) continue;
			if (node.Id.size() > engine::imagegraph::Limits::MaximumTextBytes ||
				node.Type.size() > engine::imagegraph::Limits::MaximumTextBytes) {
				error = "opaque canvas identity exceeds text bounds";
				return false;
			}
			opaquePorts.try_emplace(node.Id);
		}
		for (const auto &link : document.Links) {
			const auto from = opaquePorts.find(link.FromNode), to = opaquePorts.find(link.ToNode);
			if ((from != opaquePorts.end() &&
				 (link.FromPort.empty() ||
				  link.FromPort.size() > engine::imagegraph::Limits::MaximumTextBytes)) ||
				(to != opaquePorts.end() &&
				 (link.ToPort.empty() ||
				  link.ToPort.size() > engine::imagegraph::Limits::MaximumTextBytes))) {
				error = "opaque canvas port exceeds text bounds";
				return false;
			}
			if (from != opaquePorts.end()) from->second.second.push_back({link.FromPort, "pxcx.opaque"});
			if (to != opaquePorts.end()) to->second.first.push_back({link.ToPort, "pxcx.opaque"});
		}
		const auto physicalIndex = [](std::string_view name) -> std::optional<uint32_t> {
			if (name.starts_with("input-"))
				name.remove_prefix(6);
			else if (name.starts_with("output-"))
				name.remove_prefix(7);
			else
				return {};
			uint32_t number = 0;
			const auto parsed = std::from_chars(name.data(), name.data() + name.size(), number);
			if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size()) return {};
			return number;
		};
		for (auto &[id, interfaces] : opaquePorts) {
			(void)id;
			for (auto *ports : {&interfaces.first, &interfaces.second}) {
				std::sort(ports->begin(), ports->end(), [&](const auto &a, const auto &b) {
					const auto left = physicalIndex(a.Name), right = physicalIndex(b.Name);
					if (left != right) return left < right;
					return a.Name < b.Name;
				});
				ports->erase(
					std::unique(
						ports->begin(),
						ports->end(),
						[](const auto &a, const auto &b) { return a.Name == b.Name; }
					),
					ports->end()
				);
			}
		}
		std::unordered_map<std::string, const Node *> nodesById;
		nodesById.reserve(document.Nodes.size());
		for (size_t index = 0; index < document.Nodes.size(); index++) {
			const Node &authored = document.Nodes[index];
			if (authored.DynamicInputs.size() >
					engine::imagegraph::MaximumDynamicInputsForType(authored.Type) ||
				authored.DynamicOutputs.size() > engine::imagegraph::Limits::MaximumDynamicOutputsPerNode) {
				error = "image graph node exceeds the dynamic input limit";
				graph.Clear();
				ids = {};
				return false;
			}
			if (authored.Id.empty() || !nodesById.emplace(authored.Id, &authored).second) {
				error = "image graph contains an empty or repeated node id";
				graph.Clear();
				ids = {};
				return false;
			}
			if (!std::isfinite(authored.Position.X) || !std::isfinite(authored.Position.Y) ||
				std::abs(authored.Position.X) > std::numeric_limits<float>::max() ||
				std::abs(authored.Position.Y) > std::numeric_limits<float>::max()) {
				error = "image graph node position is outside canvas range";
				graph.Clear();
				ids = {};
				return false;
			}

			const nodegraph::NodeId canvasId = static_cast<nodegraph::NodeId>(index + 1);
			nodegraph::Node canvasNode;
			canvasNode.Id = canvasId;
			canvasNode.Type = authored.Type;
			const auto physical = opaquePorts.find(authored.Id);
			if (physical != opaquePorts.end()) {
				canvasNode.InputPorts = std::move(physical->second.first);
				canvasNode.OutputPorts = std::move(physical->second.second);
				canvasNode.Label = "PXCX " + authored.Type.substr(std::string_view("pxcx.opaque/").size());
				if (canvasNode.Label.size() > 96) canvasNode.Label = canvasNode.Label.substr(0, 93) + "...";
			}
			canvasNode.X = static_cast<float>(authored.Position.X);
			canvasNode.Y = static_cast<float>(authored.Position.Y);
			canvasNode.DynamicInputs.reserve(authored.DynamicInputs.size());
			for (const engine::imagegraph::DynamicInput &input : authored.DynamicInputs) {
				canvasNode.DynamicInputs.push_back({input.Id, CanvasType(input.Type)});
			}
			if (physical == opaquePorts.end() &&
				(authored.Type == "pc.array_split" || !authored.DynamicOutputs.empty() ||
				 authored.Type == "pc.color_to_rgb" || authored.Type == "pc.color_to_hsv")) {
				canvasNode.OutputPorts.emplace();
				for (const auto &port : detail::ImageGraphOutputPorts(authored))
					canvasNode.OutputPorts->push_back({std::string(port.Id), CanvasType(port.Type)});
			}
			if (const nodegraph::NodeType *type = nodegraph::NodeTypes::Find(authored.Type)) {
				for (const nodegraph::WidgetSpec &widget : type->Widgets) {
					canvasNode.Widgets.emplace(widget.Key, widget.Default);
				}
			}
			graph.Adopt(canvasNode);
			ids.ToCanvas.emplace(authored.Id, canvasId);
			ids.ToDocument.emplace(canvasId, authored.Id);
			ids.IssuedNodeIds.insert(authored.Id);
			ids.OriginalPositions.emplace(authored.Id, authored.Position);
			while (ids.IssuedNodeIds.contains("node-" + std::to_string(ids.NextNodeId)))
				ids.NextNodeId++;
		}
		for (const engine::imagegraph::Link &link : document.Links) {
			ids.IssuedNodeIds.insert(link.FromNode);
			ids.IssuedNodeIds.insert(link.ToNode);
		}
		for (const engine::imagegraph::Output &output : document.Outputs) {
			ids.IssuedNodeIds.insert(output.NodeId);
		}
		for (const engine::imagegraph::Keyframe &keyframe : document.Keyframes) {
			ids.IssuedNodeIds.insert(keyframe.NodeId);
		}
		for (const Node &node : document.Nodes) {
			if (!node.GroupId.empty()) ids.IssuedGroupIds.insert(node.GroupId);
		}

		std::unordered_set<std::string> declaredGroupIds;
		for (const engine::imagegraph::Group &authored : document.Groups) {
			if (authored.Ports.size() > engine::imagegraph::Limits::MaximumGroupPorts) {
				error = "image graph group exceeds the interface socket limit";
				graph.Clear();
				ids = {};
				return false;
			}
			if (authored.Id.empty() || !declaredGroupIds.insert(authored.Id).second) {
				error = "image graph contains an empty or repeated group id";
				graph.Clear();
				ids = {};
				return false;
			}
			ids.IssuedGroupIds.insert(authored.Id);
			while (ids.IssuedGroupIds.contains("group-" + std::to_string(ids.NextGroupId)))
				ids.NextGroupId++;
			std::vector<nodegraph::NodeId> members;
			for (const Node &node : document.Nodes) {
				if (node.GroupId == authored.Id) members.push_back(ids.ToCanvas.at(node.Id));
			}
			if (members.empty()) {
				ids.EmptyGroups.insert(authored.Id);
				continue;
			}
			const nodegraph::GroupId canvasId =
				graph.Group(std::move(members), authored.Name, nodegraph::Colour::Hex(0x262626));
			if (canvasId == nodegraph::NO_GROUP) {
				error = "image graph group could not be represented on the canvas";
				graph.Clear();
				ids = {};
				return false;
			}
			ids.GroupsToCanvas.emplace(authored.Id, canvasId);
			ids.GroupsToDocument.emplace(canvasId, authored.Id);
		}

		for (const engine::imagegraph::Link &authored : document.Links) {
			const auto from = ids.ToCanvas.find(authored.FromNode);
			const auto to = ids.ToCanvas.find(authored.ToNode);
			if (from == ids.ToCanvas.end() || to == ids.ToCanvas.end()) {
				ids.UnmappedLinks.push_back(authored);
				continue;
			}
			if (graph.CanConnect(from->second, authored.FromPort, to->second, authored.ToPort) !=
				nodegraph::LinkResult::Made) {
				ids.UnmappedLinks.push_back(authored);
				continue;
			}
			graph.Attach({from->second, authored.FromPort, to->second, authored.ToPort});
		}
		return true;
	}

	bool SaveImageGraphCanvas(
		const nodegraph::Graph &graph,
		const engine::imagegraph::Document &basis,
		ImageGraphCanvasIds &ids,
		engine::imagegraph::Document &document,
		std::string &error
	) {
		error.clear();
		if (graph.Nodes().size() > engine::imagegraph::Limits::MaximumNodes ||
			graph.Links().size() + ids.UnmappedLinks.size() > engine::imagegraph::Limits::MaximumLinks ||
			graph.Groups().size() > engine::imagegraph::Limits::MaximumGroups ||
			basis.Junctions.size() > engine::imagegraph::Limits::MaximumJunctions ||
			basis.Outputs.size() > engine::imagegraph::Limits::MaximumOutputs ||
			basis.Keyframes.size() > engine::imagegraph::Limits::MaximumKeyframes) {
			error = "image graph exceeds authored document limits";
			return false;
		}

		document = basis;
		document.Nodes.clear();
		document.Links.clear();
		document.Groups.clear();

		std::unordered_map<std::string, const Node *> oldNodes;
		oldNodes.reserve(basis.Nodes.size());
		for (const Node &node : basis.Nodes)
			oldNodes.emplace(node.Id, &node);

		std::unordered_set<std::string> liveDocumentIds;
		liveDocumentIds.reserve(graph.Nodes().size());
		for (const nodegraph::Node &canvasNode : graph.Nodes()) {
			if (canvasNode.DynamicInputs.size() >
				engine::imagegraph::MaximumDynamicInputsForType(canvasNode.Type)) {
				error = "canvas node exceeds the dynamic input limit";
				return false;
			}
			const std::string documentId = EnsureDocumentId(ids, canvasNode.Id);
			liveDocumentIds.insert(documentId);
			Node authored;
			if (const auto old = oldNodes.find(documentId); old != oldNodes.end()) {
				authored = *old->second;
			} else {
				authored.Id = documentId;
				authored.Values = StarterValues(basis, canvasNode.Type);
			}
			authored.Id = documentId;
			authored.Type = canvasNode.Type;
			const bool opaque = authored.Type.starts_with("pxcx.opaque/");
			// grug physical opaque ports stay in the archive; canvas save keeps authored definitions.
			if (!opaque) authored.DynamicInputs.clear();
			authored.DynamicInputs.reserve(canvasNode.DynamicInputs.size());
			for (const nodegraph::PortSpec &input : canvasNode.DynamicInputs) {
				if (opaque) continue;
				const engine::imagegraph::NodeSchema *schema = engine::imagegraph::FindSchema(authored.Type);
				const std::optional<engine::imagegraph::ValueType> valueType =
					ValueTypeFromCanvas(input.Type);
				if (schema == nullptr || !schema->DynamicInputs || !valueType.has_value()) {
					error = "canvas dynamic inputs do not match the node schema";
					return false;
				}
				engine::imagegraph::DynamicInput authoredInput{input.Name, *valueType, std::nullopt};
				if (const Node *old = FindAuthoredNode(basis, documentId)) {
					const auto oldInput = std::find_if(
						old->DynamicInputs.begin(), old->DynamicInputs.end(), [&](const auto &candidate) {
							return candidate.Id == input.Name;
						}
					);
					if (oldInput != old->DynamicInputs.end() && oldInput->Type == *valueType)
						authoredInput = *oldInput;
				}
				authored.DynamicInputs.push_back(std::move(authoredInput));
			}

			if (canvasNode.OutputPorts && !opaque) {
				const auto *schema = engine::imagegraph::FindSchema(authored.Type);
				if (!schema ||
					canvasNode.OutputPorts->size() > engine::imagegraph::Limits::MaximumArrayElements) {
					error = "canvas output interface exceeds schema bounds";
					return false;
				}
				authored.DynamicOutputs.clear();
				for (const auto &port : *canvasNode.OutputPorts) {
					if (std::any_of(schema->Ports.begin(), schema->Ports.end(), [&](const auto &fixed) {
							return fixed.Direction == engine::imagegraph::PortDirection::Output &&
								   fixed.Id == port.Name;
						}))
						continue;
					const auto type = ValueTypeFromCanvas(port.Type);
					if (!schema->DynamicOutputs || !type) {
						error = "canvas dynamic output is not supported by its schema";
						return false;
					}
					authored.DynamicOutputs.push_back({port.Name, *type});
				}
			}

			const auto oldPosition = ids.OriginalPositions.find(documentId);
			if (oldPosition != ids.OriginalPositions.end() &&
				IsSamePosition(oldPosition->second, canvasNode)) {
				authored.Position = oldPosition->second;
			} else {
				authored.Position = {static_cast<double>(canvasNode.X), static_cast<double>(canvasNode.Y)};
				ids.OriginalPositions[documentId] = authored.Position;
			}

			const std::string oldGroup = authored.GroupId;
			const nodegraph::GroupId canvasGroup = graph.GroupOf(canvasNode.Id);
			if (canvasGroup != nodegraph::NO_GROUP) {
				authored.GroupId = EnsureGroupDocumentId(ids, canvasGroup);
			} else if (const auto mapped = ids.GroupsToCanvas.find(oldGroup);
					   !oldGroup.empty() && mapped != ids.GroupsToCanvas.end()) {
				authored.GroupId.clear();
			}
			document.Nodes.push_back(std::move(authored));
		}

		std::unordered_map<nodegraph::GroupId, const nodegraph::Group *> liveGroups;
		liveGroups.reserve(graph.Groups().size());
		for (const nodegraph::Group &group : graph.Groups())
			liveGroups.emplace(group.Id, &group);
		std::unordered_set<nodegraph::GroupId> savedCanvasGroups;
		for (const engine::imagegraph::Group &old : basis.Groups) {
			if (ids.EmptyGroups.contains(old.Id)) {
				document.Groups.push_back(old);
				continue;
			}
			const auto canvasId = ids.GroupsToCanvas.find(old.Id);
			if (canvasId == ids.GroupsToCanvas.end()) continue;
			const auto current = liveGroups.find(canvasId->second);
			if (current == liveGroups.end()) continue;
			engine::imagegraph::Group authored = old;
			authored.Name = current->second->Title;
			document.Groups.push_back(std::move(authored));
			savedCanvasGroups.insert(canvasId->second);
		}
		for (const nodegraph::Group &group : graph.Groups()) {
			if (savedCanvasGroups.contains(group.Id)) continue;
			const std::string documentId = EnsureGroupDocumentId(ids, group.Id);
			document.Groups.push_back({documentId, group.Title, {}, {}});
			savedCanvasGroups.insert(group.Id);
		}
		for (const engine::imagegraph::Group &group : document.Groups) {
			if (group.Ports.size() > engine::imagegraph::Limits::MaximumGroupPorts) {
				error = "authored group exceeds the interface socket limit";
				return false;
			}
		}

		std::vector<engine::imagegraph::Link> canvasLinks;
		canvasLinks.reserve(graph.Links().size());
		for (const nodegraph::Link &link : graph.Links()) {
			const auto from = ids.ToDocument.find(link.From);
			const auto to = ids.ToDocument.find(link.To);
			if (from == ids.ToDocument.end() || to == ids.ToDocument.end()) continue;
			canvasLinks.push_back({from->second, link.FromPort, to->second, link.ToPort});
		}
		for (const engine::imagegraph::Link &old : basis.Links) {
			if (ids.UnmappedLinks.end() !=
				std::find(ids.UnmappedLinks.begin(), ids.UnmappedLinks.end(), old)) {
				document.Links.push_back(old);
				continue;
			}
			const auto from = ids.ToCanvas.find(old.FromNode);
			const auto to = ids.ToCanvas.find(old.ToNode);
			if (from == ids.ToCanvas.end() || to == ids.ToCanvas.end()) {
				document.Links.push_back(old);
				continue;
			}
			const nodegraph::Link mapped{from->second, old.FromPort, to->second, old.ToPort};
			if (HasLink(graph, mapped)) document.Links.push_back(old);
		}
		for (const engine::imagegraph::Link &link : canvasLinks) {
			if (!HasAuthoredLink(document.Links, link)) document.Links.push_back(link);
		}

		for (auto it = ids.ToCanvas.begin(); it != ids.ToCanvas.end();) {
			if (liveDocumentIds.contains(it->first)) {
				++it;
				continue;
			}
			ids.ToDocument.erase(it->second);
			ids.OriginalPositions.erase(it->first);
			it = ids.ToCanvas.erase(it);
		}
		for (auto it = ids.GroupsToCanvas.begin(); it != ids.GroupsToCanvas.end();) {
			if (savedCanvasGroups.contains(it->second)) {
				++it;
				continue;
			}
			ids.GroupsToDocument.erase(it->second);
			it = ids.GroupsToCanvas.erase(it);
		}
		PromoteFormatVersion(document);

		return true;
	}

	bool CheckImageComposerPreviewBudget(
		const engine::imagegraph::Document &document, engine::imagegraph::Diagnostic &diagnostic
	) {
		diagnostic = {};
		for (const Node &node : document.Nodes) {
			if (node.Type != "image.solid") continue;
			for (const AuthoredValue &value : node.Values) {
				if (value.Port != "width" && value.Port != "height") continue;
				const int64_t *dimension = std::get_if<int64_t>(&value.Data);
				if (dimension != nullptr && *dimension > IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION) {
					diagnostic = {
						engine::imagegraph::Status::LimitExceeded,
						node.Id,
						value.Port,
						"preview is limited to 128 by 128 pixels; reduce this dimension"
					};
					return false;
				}
			}
		}
		return true;
	}

	engine::imagegraph::Status EvaluateImageGraphPreview(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		std::string_view outputId,
		const engine::imagegraph::EvaluationRequest &request,
		ImageGraphPreviewValue &preview,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		using namespace engine::imagegraph;
		if (request.MaximumImageDimension == 0 || request.MaximumImageDimension > Limits::MaximumDimension) {
			diagnostic = {
				Status::InvalidValue,
				{},
				"image_limit",
				"request image dimension budget is outside the supported range"
			};
			return diagnostic.Code;
		}

		const auto output =
			std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const Output &candidate) {
				return candidate.Id == outputId;
			});
		if (output == document.Outputs.end()) {
			diagnostic = {Status::InvalidOutput, {}, std::string(outputId), "selected output does not exist"};
			return diagnostic.Code;
		}
		const auto node =
			std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &candidate) {
				return candidate.Id == output->NodeId;
			});
		const NodeSchema *schema = node == document.Nodes.end() ? nullptr : FindSchema(node->Type);
		if (schema == nullptr) {
			diagnostic = {
				Status::UnknownNode, output->NodeId, output->Port, "output node has no registered schema"
			};
			return diagnostic.Code;
		}
		const auto outputPorts = detail::ImageGraphOutputPorts(*node);
		const auto port =
			std::find_if(outputPorts.begin(), outputPorts.end(), [&](const PortSchema &candidate) {
				return candidate.Direction == PortDirection::Output && candidate.Id == output->Port;
			});
		if (port == outputPorts.end()) {
			diagnostic = {Status::InvalidOutput, output->NodeId, output->Port, "output port is not declared"};
			return diagnostic.Code;
		}
		EvaluationRequest boundedRequest = request;
		boundedRequest.MaximumImageDimension =
			std::min(request.MaximumImageDimension, IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION);
		if (port->Type != ValueType::Image && !detail::ImageGraphValuePreviewSupported(port->Type)) {
			diagnostic = {
				Status::UnsupportedExecution,
				output->NodeId,
				output->Port,
				"selected resource requires its dedicated preview panel"
			};
			return diagnostic.Code;
		}
		const uint64_t previous = detail::ImageGraphPreviewBytes(preview);
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes || previous >= maximumBytes) {
			diagnostic = {
				Status::LimitExceeded,
				output->NodeId,
				output->Port,
				"preview replacement exceeds caller byte allowance"
			};
			return diagnostic.Code;
		}
		StatefulEvaluationResult result;
		const auto status = EvaluateStateful(
			document, plan, std::string(outputId), boundedRequest, result, diagnostic, maximumBytes - previous
		);
		if (status != Status::Ok) return status;
		ImageGraphPreviewValue candidate =
			std::visit([](auto &value) -> ImageGraphPreviewValue { return std::move(value); }, result.Output);
		if (detail::ImageGraphPreviewBytes(candidate) > maximumBytes - previous) {
			diagnostic = {
				Status::LimitExceeded,
				output->NodeId,
				output->Port,
				"preview retained capacities exceed caller byte allowance"
			};
			return diagnostic.Code;
		}
		if (!detail::ValidateImageGraphPreview(candidate, diagnostic)) {
			diagnostic.NodeId = output->NodeId;
			diagnostic.Port = output->Port;
			return diagnostic.Code;
		}
		preview = std::move(candidate);
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {engine::imagegraph::Status::LimitExceeded, {}, {}, "preview allocation refused"};
		return diagnostic.Code;
	}

	bool CheckImageGraphArrayPreview(
		const engine::imagegraph::ArrayValue &array, engine::imagegraph::Diagnostic &diagnostic
	) {
		using namespace engine::imagegraph;
		diagnostic = {};
		const auto fail = [&](Status code, std::string message) {
			diagnostic = {code, {}, {}, std::move(message)};
			return false;
		};
		if (array.ElementType != ValueType::Scalar && array.ElementType != ValueType::Integer)
			return fail(Status::UnsupportedExecution, "array preview requires numeric elements");
		if (!array.Elements.empty() && !array.Nested.empty())
			return fail(Status::InvalidValue, "array preview cannot combine flat samples and channels");
		if (array.Nested.size() > Limits::MaximumAudioChannels)
			return fail(Status::LimitExceeded, "array preview exceeds the channel limit");
		size_t samples = 0;
		const auto checkRow = [&](const std::vector<ElementValue> &row) {
			if (row.size() > Limits::MaximumArrayElements - samples)
				return fail(Status::LimitExceeded, "array preview exceeds the sample limit");
			samples += row.size();
			for (const ElementValue &sample : row) {
				if (array.ElementType == ValueType::Integer && std::holds_alternative<int64_t>(sample))
					continue;
				const double *number = std::get_if<double>(&sample);
				if (array.ElementType != ValueType::Scalar || !number || !std::isfinite(*number))
					return fail(
						Status::InvalidValue,
						"array preview sample is not a finite number of its declared type"
					);
			}
			return true;
		};
		if (!checkRow(array.Elements)) return false;
		for (const auto &channel : array.Nested) {
			if (!checkRow(channel)) return false;
		}
		return true;
	}

	bool SetImageGraphValue(
		Document &document,
		std::string_view nodeId,
		std::string_view property,
		engine::imagegraph::Value value,
		engine::imagegraph::Diagnostic &error
	) {
		error = {};
		const auto fail = [&](engine::imagegraph::Status code, std::string message) {
			SetDiagnostic(error, code, nodeId, property, std::move(message));
			return false;
		};
		Node *node = FindAuthoredNode(document, nodeId);
		if (node == nullptr) return fail(engine::imagegraph::Status::UnknownNode, "node does not exist");
		const engine::imagegraph::NodeSchema *schema = engine::imagegraph::FindSchema(node->Type);
		if (schema == nullptr) return fail(engine::imagegraph::Status::UnknownNode, "node type is unknown");
		const auto declared =
			std::find_if(schema->Properties.begin(), schema->Properties.end(), [&](const auto &entry) {
				return entry.Id == property;
			});
		if (declared == schema->Properties.end())
			return fail(engine::imagegraph::Status::UnknownPort, "property is not declared");
		const auto valueType = TypeOf(value);
		const auto *sourceInput = imagegraph_choices::Input(node->Type, property);
		const bool sourceChoice =
			sourceInput && engine::imagegraph::CatalogueSourceEnumValue(*sourceInput, value);
		const auto *sourceEntry = engine::imagegraph::FindCatalogueEntry(node->Type);
		const auto *array = std::get_if<engine::imagegraph::ArrayValue>(&value);
		const bool sourceArray =
			sourceEntry && sourceInput && array &&
			engine::imagegraph::CatalogueAuthoredArray(*sourceEntry, *sourceInput, *array);
		if (!sourceChoice && !sourceArray &&
			!(sourceInput && engine::imagegraph::CatalogueSourceRawValue(*sourceInput, value)) &&
			(!valueType || *valueType != declared->Type))
			return fail(engine::imagegraph::Status::TypeMismatch, "value type does not match the schema");
		if (const auto *text = std::get_if<std::string>(&value);
			text != nullptr && text->size() > engine::imagegraph::Limits::MaximumTextBytes) {
			return fail(engine::imagegraph::Status::LimitExceeded, "text value exceeds the document limit");
		}
		if (const auto *scalar = std::get_if<double>(&value); scalar != nullptr && !std::isfinite(*scalar))
			return fail(engine::imagegraph::Status::InvalidValue, "scalar value must be finite");
		if (const auto *vector = std::get_if<engine::imagegraph::Vector2>(&value);
			vector != nullptr && (!std::isfinite(vector->X) || !std::isfinite(vector->Y))) {
			return fail(engine::imagegraph::Status::InvalidValue, "vector value must be finite");
		}
		auto existing = std::find_if(node->Values.begin(), node->Values.end(), [&](const auto &entry) {
			return entry.Port == property;
		});
		if (existing == node->Values.end()) {
			if (node->Values.size() >= engine::imagegraph::Limits::MaximumPropertiesPerNode)
				return fail(engine::imagegraph::Status::LimitExceeded, "node property limit reached");
			node->Values.push_back({std::string(property), std::move(value)});
		} else {
			existing->Data = std::move(value);
		}
		PromoteFormatVersion(document);
		return true;
	}

	bool SetImageGraphProjectSettings(
		Document &document,
		const engine::imagegraph::ProjectSettings &settings,
		engine::imagegraph::Diagnostic &error
	) {
		error = {};
		using engine::imagegraph::Limits;
		if (settings.SurfaceWidth < 1 || settings.SurfaceHeight < 1 ||
			settings.SurfaceWidth > Limits::MaximumDimension ||
			settings.SurfaceHeight > Limits::MaximumDimension || settings.Interpolation < 0 ||
			settings.Interpolation > 6 || settings.Oversample < 0 || settings.Oversample > 12) {
			SetDiagnostic(
				error,
				engine::imagegraph::Status::InvalidValue,
				{},
				{},
				"project settings are outside their ranges"
			);
			return false;
		}
		if (settings.Palette.size() > Limits::MaximumProjectPaletteEntries) {
			SetDiagnostic(
				error,
				engine::imagegraph::Status::LimitExceeded,
				{},
				{},
				"project palette exceeds the document limit"
			);
			return false;
		}
		if (!engine::imagegraph::ValidProjectPreviewSettings(settings)) {
			SetDiagnostic(
				error,
				engine::imagegraph::Status::InvalidValue,
				{},
				"preview_grid",
				"preview grid and guides are invalid or exceed their limits"
			);
			return false;
		}
		document.Project = settings;
		PromoteFormatVersion(document);
		return true;
	}

	void RemoveImageGraphProjectSettings(Document &document) {
		document.Project.reset();
	}

	bool SetImageGraphDynamicInput(
		Document &document,
		std::string_view nodeId,
		engine::imagegraph::DynamicInput input,
		engine::imagegraph::Diagnostic &error
	) {
		error = {};
		const auto fail = [&](engine::imagegraph::Status code, std::string message) {
			SetDiagnostic(error, code, nodeId, input.Id, std::move(message));
			return false;
		};
		Node *node = FindAuthoredNode(document, nodeId);
		if (node == nullptr) return fail(engine::imagegraph::Status::UnknownNode, "node does not exist");
		const engine::imagegraph::NodeSchema *schema = engine::imagegraph::FindSchema(node->Type);
		if (schema == nullptr || !schema->DynamicInputs)
			return fail(engine::imagegraph::Status::UnknownPort, "node type has no dynamic inputs");
		if (input.Id.empty() || input.Id.size() > engine::imagegraph::Limits::MaximumTextBytes)
			return fail(engine::imagegraph::Status::InvalidValue, "dynamic input name is empty or too long");
		if (input.SourceLayerName.size() > engine::imagegraph::Limits::MaximumTextBytes)
			return fail(engine::imagegraph::Status::LimitExceeded, "source layer binding exceeds text limit");
		if (CanvasType(input.Type) == "imagegraph.unknown")
			return fail(engine::imagegraph::Status::InvalidValue, "dynamic input type is invalid");
		if (std::any_of(schema->Ports.begin(), schema->Ports.end(), [&](const auto &port) {
				return port.Id == input.Id;
			}))
			return fail(engine::imagegraph::Status::DuplicateId, "dynamic input duplicates a schema port");
		const auto existing =
			std::find_if(node->DynamicInputs.begin(), node->DynamicInputs.end(), [&](const auto &held) {
				return held.Id == input.Id;
			});
		if (existing == node->DynamicInputs.end() &&
			node->DynamicInputs.size() >= engine::imagegraph::MaximumDynamicInputsForType(node->Type))
			return fail(engine::imagegraph::Status::LimitExceeded, "dynamic input limit reached");
		const auto *sourceEntry = engine::imagegraph::FindCatalogueEntry(node->Type);
		size_t sourceGroup = 0;
		const auto *sourceInput =
			sourceEntry ? engine::imagegraph::FindDynamicTemplate(*sourceEntry, input.Id, sourceGroup)
						: nullptr;
		if (sourceInput &&
			input.Type != detail::SourceArgumentType(*node, input.Id).value_or(sourceInput->Type))
			return fail(
				engine::imagegraph::Status::TypeMismatch, "source input type is fixed by its template"
			);
		if (input.Default) {
			const auto valueType = TypeOf(*input.Default);
			if (input.Type != engine::imagegraph::ValueType::Any &&
				!detail::SourceLuaArgumentType(*node, input.Id) &&
				!detail::HlslRawArgumentValue(*node, input.Id, *input.Default) &&
				(!valueType || *valueType != input.Type) &&
				!(sourceInput &&
				  (engine::imagegraph::CatalogueSourceRawValue(*sourceInput, *input.Default) ||
				   engine::imagegraph::CatalogueSourceEnumValue(*sourceInput, *input.Default) ||
				   (std::holds_alternative<engine::imagegraph::ArrayValue>(*input.Default) &&
					engine::imagegraph::CatalogueAuthoredArray(
						*sourceEntry, *sourceInput, std::get<engine::imagegraph::ArrayValue>(*input.Default)
					)))))
				return fail(
					engine::imagegraph::Status::TypeMismatch, "dynamic input default has the wrong type"
				);
			if (const auto *scalar = std::get_if<double>(&*input.Default); scalar && !std::isfinite(*scalar))
				return fail(engine::imagegraph::Status::InvalidValue, "dynamic input default must be finite");
			if (const auto *vector = std::get_if<engine::imagegraph::Vector2>(&*input.Default);
				vector && (!std::isfinite(vector->X) || !std::isfinite(vector->Y)))
				return fail(engine::imagegraph::Status::InvalidValue, "dynamic input default must be finite");
		}
		if (sourceInput && sourceInput->SourceIndex >= 0 && input.Default && node->InstanceBase.empty() &&
			std::find(node->SourceStaticInputs.begin(), node->SourceStaticInputs.end(), input.Id) !=
				node->SourceStaticInputs.end()) {
			auto key =
				std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &frame) {
					return frame.NodeId == nodeId && frame.Port == input.Id;
				});
			if (key == document.Keyframes.end()) {
				const bool haveTrack =
					std::any_of(document.Tracks.begin(), document.Tracks.end(), [&](const auto &track) {
						return track.NodeId == nodeId && track.Port == input.Id;
					});
				if (document.Keyframes.size() >= engine::imagegraph::Limits::MaximumKeyframes ||
					(!haveTrack && document.Tracks.size() >= engine::imagegraph::Limits::MaximumTracks))
					return fail(
						engine::imagegraph::Status::LimitExceeded, "source animator storage exceeds its limit"
					);
				document.Keyframes.push_back(
					{node->Id, input.Id, 0, *input.Default, "source", engine::imagegraph::KeyframeEase{}}
				);
				if (!haveTrack) document.Tracks.push_back({node->Id, input.Id, "wrap", -1});
			} else {
				// Static source getters read the first original animator key, including raw Lua literals.
				key->Data = *input.Default;
			}
		}
		if (existing == node->DynamicInputs.end()) {
			node->DynamicInputs.push_back(std::move(input));
		} else {
			if (existing->Type != input.Type) {
				std::erase_if(document.Links, [&](const auto &link) {
					return link.ToNode == nodeId && link.ToPort == input.Id;
				});
			}
			*existing = std::move(input);
		}
		if (node->Type.starts_with("pc.lua_") || node->Type == "pc.hlsl") {
			for (auto &argument : node->DynamicInputs) {
				const auto type = detail::SourceArgumentType(*node, argument.Id);
				if (!type || argument.Type == *type) continue;
				argument.Type = *type;
				std::erase_if(document.Links, [&](const auto &link) {
					return link.ToNode == nodeId && link.ToPort == argument.Id;
				});
			}
		}
		PromoteFormatVersion(document);
		return true;
	}

	bool RemoveImageGraphDynamicInput(
		Document &document,
		std::string_view nodeId,
		std::string_view inputId,
		engine::imagegraph::Diagnostic &error
	) {
		error = {};
		Node *node = FindAuthoredNode(document, nodeId);
		if (node == nullptr) {
			SetDiagnostic(
				error, engine::imagegraph::Status::UnknownNode, nodeId, inputId, "node does not exist"
			);
			return false;
		}
		const auto input =
			std::find_if(node->DynamicInputs.begin(), node->DynamicInputs.end(), [&](const auto &held) {
				return held.Id == inputId;
			});
		if (input == node->DynamicInputs.end()) {
			SetDiagnostic(
				error,
				engine::imagegraph::Status::UnknownPort,
				nodeId,
				inputId,
				"dynamic input does not exist"
			);
			return false;
		}
		if (node->SourceVec2Defaults) {
			std::erase_if(node->SourceVec2Defaults->Inputs, [&](const auto &value) {
				return value.Port == inputId;
			});
			if (node->SourceVec2Defaults->Inputs.empty()) node->SourceVec2Defaults = {};
		}
		node->DynamicInputs.erase(input);
		std::erase_if(document.Links, [&](const engine::imagegraph::Link &link) {
			return link.ToNode == nodeId && link.ToPort == inputId;
		});
		return true;
	}

	bool SetImageGraphSplitOutputCount(
		Document &document,
		std::string_view nodeId,
		size_t count,
		engine::imagegraph::Diagnostic &error,
		bool authorMinimum
	) try {
		using namespace engine::imagegraph;
		error = {};
		const auto *node = FindAuthoredNode(document, nodeId);
		if (!node || node->Type != "pc.array_split") {
			SetDiagnostic(error, Status::UnknownNode, nodeId, {}, "output count requires Array Split");
			return false;
		}
		if (count > Limits::MaximumArrayElements) {
			SetDiagnostic(error, Status::LimitExceeded, nodeId, {}, "output count must be 0 through 4096");
			return false;
		}
		const auto bytes = DocumentRetainedPayloadBytes(document);
		if (!bytes || *bytes > Limits::MaximumEvaluationBytes / 2) {
			SetDiagnostic(
				error,
				Status::LimitExceeded,
				nodeId,
				{},
				"output edit document copy exceeds live payload budget"
			);
			return false;
		}
		Document staged = document;
		auto *target = FindAuthoredNode(staged, nodeId);
		target->DynamicOutputs.clear();
		target->DynamicOutputs.reserve(count ? count - 1 : 0);
		for (size_t index = 1; index < count; ++index)
			target->DynamicOutputs.push_back({"val_" + std::to_string(index), ValueType::Any});
		if ((authorMinimum &&
			 !SetImageGraphValue(staged, nodeId, "minimum_outputs", int64_t(count), error)) ||
			!SetImageGraphValue(staged, nodeId, "attribute_output_amount", double(count), error))
			return false;
		const auto valid = [&](std::string_view port) {
			return (count && port == "val_0") || std::any_of(
													 target->DynamicOutputs.begin(),
													 target->DynamicOutputs.end(),
													 [&](const auto &output) { return output.Id == port; }
												 );
		};
		std::erase_if(staged.Links, [&](const auto &link) {
			return link.FromNode == nodeId && !valid(link.FromPort);
		});
		std::erase_if(staged.Outputs, [&](const auto &output) {
			return output.NodeId == nodeId && !valid(output.Port);
		});
		staged.FormatVersion = std::max(staged.FormatVersion, 9u);
		document = std::move(staged);
		return true;
	} catch (const std::bad_alloc &) {
		error = {
			engine::imagegraph::Status::LimitExceeded,
			std::string(nodeId),
			{},
			"output interface allocation failed"
		};
		return false;
	}

	bool AddImageGraphGroup(
		Document &document,
		std::string groupId,
		std::string name,
		std::string_view parentId,
		engine::imagegraph::Diagnostic &error
	) {
		error = {};
		if (groupId.empty() || name.empty() ||
			groupId.size() > engine::imagegraph::Limits::MaximumTextBytes ||
			name.size() > engine::imagegraph::Limits::MaximumTextBytes) {
			SetDiagnostic(
				error, engine::imagegraph::Status::InvalidGroup, groupId, {}, "group id or name is invalid"
			);
			return false;
		}
		if (document.Groups.size() >= engine::imagegraph::Limits::MaximumGroups) {
			SetDiagnostic(
				error, engine::imagegraph::Status::LimitExceeded, groupId, {}, "group limit reached"
			);
			return false;
		}
		if (std::any_of(
				document.Groups.begin(),
				document.Groups.end(),
				[&](const auto &group) { return group.Id == groupId; }
			) ||
			std::any_of(
				document.Nodes.begin(),
				document.Nodes.end(),
				[&](const Node &node) { return node.Id == groupId; }
			) ||
			std::any_of(document.Junctions.begin(), document.Junctions.end(), [&](const auto &junction) {
				return junction.Id == groupId;
			})) {
			SetDiagnostic(
				error, engine::imagegraph::Status::DuplicateId, groupId, {}, "group id is already used"
			);
			return false;
		}
		if (!parentId.empty() &&
			std::none_of(document.Groups.begin(), document.Groups.end(), [&](const auto &group) {
				return group.Id == parentId;
			})) {
			SetDiagnostic(
				error,
				engine::imagegraph::Status::InvalidGroup,
				groupId,
				parentId,
				"parent group does not exist"
			);
			return false;
		}
		document.FormatVersion = std::max(document.FormatVersion, 2u);
		document.Groups.push_back({std::move(groupId), std::move(name), std::string(parentId), {}});
		return true;
	}

	bool AddImageGraphGroupPort(
		Document &document,
		std::string_view groupId,
		engine::imagegraph::GroupPort port,
		engine::imagegraph::Junction junction,
		engine::imagegraph::Diagnostic &error
	) {
		error = {};
		const auto group =
			std::find_if(document.Groups.begin(), document.Groups.end(), [&](const auto &candidate) {
				return candidate.Id == groupId;
			});
		if (group == document.Groups.end()) {
			SetDiagnostic(
				error, engine::imagegraph::Status::InvalidGroup, groupId, {}, "group does not exist"
			);
			return false;
		}
		if (group->Ports.size() >= engine::imagegraph::Limits::MaximumGroupPorts ||
			document.Junctions.size() >= engine::imagegraph::Limits::MaximumJunctions) {
			SetDiagnostic(
				error, engine::imagegraph::Status::LimitExceeded, groupId, {}, "group port limit reached"
			);
			return false;
		}
		if (port.Id.empty() || port.JunctionId.empty() ||
			port.Id.size() > engine::imagegraph::Limits::MaximumTextBytes ||
			port.JunctionId.size() > engine::imagegraph::Limits::MaximumTextBytes ||
			port.JunctionId != junction.Id || junction.GroupId != groupId ||
			CanvasType(junction.Type) == "imagegraph.unknown" ||
			(port.Direction != engine::imagegraph::PortDirection::Input &&
			 port.Direction != engine::imagegraph::PortDirection::Output) ||
			std::any_of(
				group->Ports.begin(), group->Ports.end(), [&](const auto &held) { return held.Id == port.Id; }
			) ||
			std::any_of(
				document.Junctions.begin(),
				document.Junctions.end(),
				[&](const auto &held) { return held.Id == junction.Id; }
			) ||
			std::any_of(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
				return node.Id == junction.Id;
			})) {
			SetDiagnostic(
				error,
				engine::imagegraph::Status::InvalidGroup,
				groupId,
				port.Id,
				"group port or junction is invalid"
			);
			return false;
		}
		if (junction.Default) {
			const auto type = TypeOf(*junction.Default);
			if (!type || *type != junction.Type) {
				SetDiagnostic(
					error,
					engine::imagegraph::Status::TypeMismatch,
					junction.Id,
					"value",
					"junction default has the wrong type"
				);
				return false;
			}
		}
		group->Ports.push_back(std::move(port));
		document.Junctions.push_back(std::move(junction));
		PromoteFormatVersion(document);
		return true;
	}

	bool AddImageGraphRoute(
		Document &document, engine::imagegraph::Link route, engine::imagegraph::Diagnostic &error
	) {
		error = {};
		const auto endpointExists = [&](std::string_view id) {
			return std::any_of(
					   document.Nodes.begin(),
					   document.Nodes.end(),
					   [&](const Node &node) { return node.Id == id; }
				   ) ||
				   std::any_of(
					   document.Junctions.begin(), document.Junctions.end(), [&](const auto &junction) {
						   return junction.Id == id;
					   }
				   );
		};
		if (document.Links.size() >= engine::imagegraph::Limits::MaximumLinks) {
			SetDiagnostic(
				error,
				engine::imagegraph::Status::LimitExceeded,
				route.ToNode,
				route.ToPort,
				"link limit reached"
			);
			return false;
		}
		if (route.FromNode.empty() || route.ToNode.empty() || route.FromPort.empty() ||
			route.ToPort.empty() || !endpointExists(route.FromNode) || !endpointExists(route.ToNode)) {
			SetDiagnostic(
				error,
				engine::imagegraph::Status::UnknownPort,
				route.ToNode,
				route.ToPort,
				"route endpoint or port is missing"
			);
			return false;
		}
		if (std::find(document.Links.begin(), document.Links.end(), route) != document.Links.end()) {
			SetDiagnostic(
				error,
				engine::imagegraph::Status::DuplicateId,
				route.ToNode,
				route.ToPort,
				"route already exists"
			);
			return false;
		}
		document.Links.push_back(std::move(route));
		PromoteFormatVersion(document);
		return true;
	}

	bool RemoveImageGraphRoute(Document &document, size_t index, engine::imagegraph::Diagnostic &error) {
		error = {};
		if (index >= document.Links.size()) {
			SetDiagnostic(
				error, engine::imagegraph::Status::UnknownPort, {}, {}, "route index is outside the document"
			);
			return false;
		}
		document.Links.erase(document.Links.begin() + static_cast<std::ptrdiff_t>(index));
		return true;
	}

	bool SetImageGraphKeyframe(
		Document &document,
		std::string_view nodeId,
		std::string_view property,
		uint64_t tick,
		std::string_view interpolation,
		engine::imagegraph::Diagnostic &error,
		double subframe,
		bool negativeFrame,
		uint64_t availableBytes
	) {
		error = {};
		const auto fail = [&](engine::imagegraph::Status code, std::string message) {
			SetDiagnostic(error, code, nodeId, property, std::move(message));
			return false;
		};
		if (interpolation != "step" && interpolation != "linear" && interpolation != "cubic" &&
			interpolation != "source")
			return fail(engine::imagegraph::Status::InvalidValue, "unsupported interpolation rule");
		const engine::imagegraph::FrameTime time{tick, subframe, negativeFrame};
		if (!engine::imagegraph::ValidFrameTime(time))
			return fail(engine::imagegraph::Status::InvalidValue, "key time is invalid");
		const Node *node = FindAuthoredNode(document, nodeId);
		if (node == nullptr) return fail(engine::imagegraph::Status::UnknownNode, "node does not exist");
		const engine::imagegraph::NodeSchema *schema = engine::imagegraph::FindSchema(node->Type);
		if (schema == nullptr) return fail(engine::imagegraph::Status::UnknownNode, "node type is unknown");
		const auto declared =
			std::find_if(schema->Properties.begin(), schema->Properties.end(), [&](const auto &entry) {
				return entry.Id == property;
			});
		const auto dynamic =
			std::find_if(node->DynamicInputs.begin(), node->DynamicInputs.end(), [&](const auto &input) {
				return input.Id == property;
			});
		if (declared == schema->Properties.end() && dynamic == node->DynamicInputs.end())
			return fail(engine::imagegraph::Status::UnknownPort, "property is not declared");
		const auto value = std::find_if(node->Values.begin(), node->Values.end(), [&](const auto &entry) {
			return entry.Port == property;
		});
		const engine::imagegraph::Value *data =
			value != node->Values.end()
				? &value->Data
				: (dynamic != node->DynamicInputs.end() && dynamic->Default ? &*dynamic->Default : nullptr);
		if (!data)
			return fail(engine::imagegraph::Status::InvalidValue, "initialize the property before keying it");
		const auto declaredType = dynamic != node->DynamicInputs.end() ? dynamic->Type : declared->Type;
		const auto valueType = TypeOf(*data);
		const auto *sourceInput = imagegraph_choices::Input(node->Type, property);
		const auto *sourceEntry = engine::imagegraph::FindCatalogueEntry(node->Type);
		const auto *array = std::get_if<engine::imagegraph::ArrayValue>(data);
		const bool sourceValue =
			sourceInput && (engine::imagegraph::CatalogueSourceEnumValue(*sourceInput, *data) ||
							engine::imagegraph::CatalogueSourceRawValue(*sourceInput, *data) ||
							(sourceEntry && array &&
							 engine::imagegraph::CatalogueAuthoredArray(*sourceEntry, *sourceInput, *array)));
		if (!sourceValue && declaredType != engine::imagegraph::ValueType::Any &&
			(!valueType || *valueType != declaredType))
			return fail(engine::imagegraph::Status::TypeMismatch, "authored value does not match the schema");
		auto frame =
			std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &entry) {
				return entry.NodeId == nodeId && entry.Port == property &&
					   engine::imagegraph::GetFrameTime(entry) == time;
			});
		if (frame == document.Keyframes.end()) {
			if (document.Keyframes.size() >= engine::imagegraph::Limits::MaximumKeyframes)
				return fail(engine::imagegraph::Status::LimitExceeded, "keyframe limit reached");
		}
		if (document.SourceAnimators) {
			using namespace engine::imagegraph;
			if (!ValidateCapturedEditBudget(availableBytes, error, nodeId, property)) return false;
			const auto resident = DocumentRetainedPayloadBytes(document),
					   valueBytes = ValueClonePayloadBytes(*data);
			if (!resident || !valueBytes || *resident > availableBytes ||
				*valueBytes > availableBytes - *resident)
				return fail(Status::LimitExceeded, "captured key value draft exceeds the payload budget");
			uint64_t remaining = availableBytes - *resident - *valueBytes;
			size_t count = 0;
			for (const auto &key : document.Keyframes) {
				if (key.NodeId != nodeId || key.Port != property) continue;
				const auto bytes = KeyframePayloadBytes(key);
				if (!bytes || *bytes > remaining / 2)
					return fail(Status::LimitExceeded, "captured key track draft exceeds the payload budget");
				remaining -= 2 * *bytes;
				++count;
			}
			const bool fresh = frame == document.Keyframes.end();
			const uint64_t extra = (count + size_t(fresh)) * sizeof(SourceKeyframeEdit) +
								   2 * sizeof(Keyframe) + 2 * (nodeId.size() + property.size() + 128);
			if (extra > remaining)
				return fail(Status::LimitExceeded, "captured key draft records exceed the payload budget");
			std::optional<Keyframe> created;
			if (fresh) {
				created.emplace(
					std::string(nodeId), std::string(property), tick, *data, std::string(interpolation)
				);
				(void)SetFrameTime(*created, time);
				if (interpolation == "source") created->Ease = KeyframeEase{};
			}
			std::vector<Keyframe> replacements;
			std::vector<SourceKeyframeEdit> edits;
			replacements.reserve(count);
			edits.reserve(count + size_t(fresh));
			for (const auto &key : document.Keyframes) {
				if (key.NodeId != nodeId || key.Port != property) continue;
				replacements.push_back(key);
				auto &replacement = replacements.back();
				replacement.Interpolation = interpolation;
				if (interpolation == "source") {
					if (!replacement.Ease) replacement.Ease = KeyframeEase{};
				} else
					replacement.Ease.reset();
				if (GetFrameTime(key) == time) replacement.Data = *data;
				edits.push_back({&key, &replacement});
			}
			if (fresh) edits.push_back({&*created, &*created, true});
			const uint64_t spareSlots = (replacements.capacity() - replacements.size()) * sizeof(Keyframe) +
										(edits.capacity() - edits.size()) * sizeof(SourceKeyframeEdit);
			if (spareSlots > availableBytes)
				return fail(
					Status::LimitExceeded, "captured key draft allocation exceeds the payload budget"
				);
			return ApplySourceKeyframeEdits(document, edits, document, error, availableBytes - spareSlots) ==
				   Status::Ok;
		}
		if (interpolation == "source" && !EnsureSourceAnimationTrack(document, nodeId, property, error))
			return false;
		if (frame == document.Keyframes.end()) {
			document.Keyframes.push_back(
				{std::string(nodeId),
				 std::string(property),
				 tick,
				 *data,
				 std::string(interpolation),
				 std::nullopt}
			);
			(void)engine::imagegraph::SetFrameTime(document.Keyframes.back(), time);
		} else {
			frame->Data = *data;
		}
		if (subframe != 0 || negativeFrame)
			document.FormatVersion = std::max(document.FormatVersion, uint32_t{9});
		SetTrackInterpolation(document, nodeId, property, interpolation);
		return true;
	}

	bool RemoveImageGraphKeyframe(
		Document &document,
		std::string_view nodeId,
		std::string_view property,
		uint64_t tick,
		engine::imagegraph::Diagnostic &error,
		double subframe,
		bool negativeFrame,
		uint64_t availableBytes
	) {
		error = {};
		const auto fail = [&](engine::imagegraph::Status code, std::string message) {
			SetDiagnostic(error, code, nodeId, property, std::move(message));
			return false;
		};
		const engine::imagegraph::FrameTime time{tick, subframe, negativeFrame};
		if (!engine::imagegraph::ValidFrameTime(time))
			return fail(engine::imagegraph::Status::InvalidValue, "key time is invalid");
		const Node *node = FindAuthoredNode(document, nodeId);
		if (node == nullptr) return fail(engine::imagegraph::Status::UnknownNode, "node does not exist");
		const engine::imagegraph::NodeSchema *schema = engine::imagegraph::FindSchema(node->Type);
		if (schema == nullptr) return fail(engine::imagegraph::Status::UnknownNode, "node type is unknown");
		if (std::none_of(schema->Properties.begin(), schema->Properties.end(), [&](const auto &entry) {
				return entry.Id == property;
			})) {
			return fail(engine::imagegraph::Status::UnknownPort, "property is not declared");
		}
		if (document.SourceAnimators) {
			const auto found =
				std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &key) {
					return key.NodeId == nodeId && key.Port == property &&
						   engine::imagegraph::GetFrameTime(key) == time;
				});
			if (found == document.Keyframes.end()) return false;
			const engine::imagegraph::SourceKeyframeEdit edit{&*found};
			Document candidate;
			if (engine::imagegraph::ApplySourceKeyframeEdits(
					document, {&edit, 1}, candidate, error, availableBytes
				) != engine::imagegraph::Status::Ok)
				return false;
			document = std::move(candidate);
			return true;
		}
		const size_t before = document.Keyframes.size();
		std::erase_if(document.Keyframes, [&](const auto &entry) {
			return entry.NodeId == nodeId && entry.Port == property &&
				   engine::imagegraph::GetFrameTime(entry) == time;
		});
		if (document.Keyframes.size() == before) return false;
		const bool stillKeyed =
			std::any_of(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &entry) {
				return entry.NodeId == nodeId && entry.Port == property;
			});
		if (!stillKeyed) {
			std::erase_if(document.Tracks, [&](const auto &track) {
				return track.NodeId == nodeId && track.Port == property;
			});
		}
		return true;
	}

	bool CaptureImageGraphKeyframes(
		const Document &document,
		std::span<const ImageGraphKeyframeIdentity> selection,
		std::vector<engine::imagegraph::Keyframe> &result,
		engine::imagegraph::Diagnostic &error,
		uint64_t availableBytes
	) {
		using namespace engine::imagegraph;
		error = {};
		const auto fail = [&](Status status, const char *message) {
			SetDiagnostic(error, status, {}, {}, message);
			return false;
		};
		if (selection.empty() || selection.size() > Limits::MaximumKeyframes ||
			document.Keyframes.size() > Limits::MaximumKeyframes)
			return fail(Status::LimitExceeded, "key selection is empty or exceeds the limit");
		if (std::any_of(selection.begin(), selection.end(), [](const auto &id) { return id.Axis != -1; })) {
			const uint64_t scratch =
				selection.size() * (sizeof(SourceKeyframeIdentity) + sizeof(ImageGraphKeyframeIdentity));
			if (scratch > availableBytes)
				return fail(Status::LimitExceeded, "axis key capture identities exceed the payload budget");
			try {
				std::vector<SourceKeyframeIdentity> identities;
				identities.reserve(selection.size());
				for (const auto &id : selection)
					identities.push_back({id.NodeId, id.Port, id.Time, id.Axis});
				return CaptureSourceKeyframes(
						   document,
						   identities,
						   result,
						   error,
						   std::min(availableBytes - scratch, Limits::MaximumEvaluationBytes)
					   ) == Status::Ok;
			} catch (const std::bad_alloc &) {
				return fail(Status::LimitExceeded, "axis key capture identity allocation failed");
			}
		}
		uint64_t remaining = std::min(availableBytes, Limits::MaximumEvaluationBytes);
		for (const auto &key : result) {
			const auto bytes = KeyframePayloadBytes(key);
			if (!bytes || *bytes > remaining)
				return fail(Status::LimitExceeded, "retained key snapshot exceeds the payload budget");
			remaining -= *bytes;
		}
		for (size_t index = 0; index < selection.size(); ++index) {
			const auto &identity = selection[index];
			if (!ValidFrameTime(identity.Time) ||
				std::find(selection.begin(), selection.begin() + index, identity) !=
					selection.begin() + index)
				return fail(Status::InvalidValue, "key selection contains an invalid or repeated identity");
			const auto found =
				std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &key) {
					return key.NodeId == identity.NodeId && key.Port == identity.Port &&
						   GetFrameTime(key) == identity.Time;
				});
			if (found == document.Keyframes.end())
				return fail(Status::InvalidValue, "selected key no longer exists");
			const auto bytes = KeyframePayloadBytes(*found);
			if (!bytes || *bytes > remaining)
				return fail(Status::LimitExceeded, "key snapshot exceeds the payload budget");
			remaining -= *bytes;
		}
		std::vector<Keyframe> candidate;
		candidate.reserve(selection.size());
		for (const auto &identity : selection) {
			const auto found =
				std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &key) {
					return key.NodeId == identity.NodeId && key.Port == identity.Port &&
						   GetFrameTime(key) == identity.Time;
				});
			candidate.push_back(*found);
		}
		result = std::move(candidate);
		return true;
	}

	bool TransferImageGraphKeyframes(
		Document &document,
		std::span<const engine::imagegraph::Keyframe> originals,
		const engine::imagegraph::FrameTime &oldAnchor,
		const engine::imagegraph::FrameTime &newAnchor,
		bool copy,
		engine::imagegraph::Diagnostic &error,
		uint64_t availableBytes,
		std::span<const int8_t> axes
	) {
		using namespace engine::imagegraph;
		if (!ValidFrameTime(oldAnchor) || !ValidFrameTime(newAnchor) ||
			originals.size() > Limits::MaximumKeyframes) {
			SetDiagnostic(
				error, Status::InvalidValue, {}, {}, "key transfer has an invalid count or authored frame"
			);
			return false;
		}
		if (originals.size() * sizeof(FrameTime) > availableBytes) {
			SetDiagnostic(error, Status::LimitExceeded, {}, {}, "key destinations exceed the payload budget");
			return false;
		}
		std::vector<FrameTime> destinations;
		destinations.reserve(originals.size());
		for (const auto &key : originals) {
			FrameTime time;
			if (!ValidFrameTime(GetFrameTime(key)) ||
				!ShiftFrameTime(GetFrameTime(key), oldAnchor, newAnchor, time)) {
				SetDiagnostic(
					error, Status::InvalidValue, {}, {}, "shifted key frame exceeds the authored range"
				);
				return false;
			}
			destinations.push_back(time);
		}
		return RetimeImageGraphKeyframes(
			document,
			originals,
			destinations,
			copy,
			error,
			availableBytes - destinations.size() * sizeof(FrameTime),
			true,
			axes
		);
	}

	bool RetimeImageGraphKeyframes(
		Document &document,
		std::span<const engine::imagegraph::Keyframe> originals,
		std::span<const engine::imagegraph::FrameTime> destinations,
		bool copy,
		engine::imagegraph::Diagnostic &error,
		uint64_t availableBytes,
		bool clampZero,
		std::span<const int8_t> axes
	) {
		using namespace engine::imagegraph;
		error = {};
		const auto fail = [&](Status status, const char *message) {
			SetDiagnostic(error, status, {}, {}, message);
			return false;
		};
		if (originals.empty() || originals.size() > Limits::MaximumKeyframes ||
			document.Keyframes.size() > Limits::MaximumKeyframes || destinations.size() != originals.size() ||
			(!axes.empty() && axes.size() != originals.size()))
			return fail(Status::InvalidValue, "key transfer has an invalid count or authored frame");
		uint64_t remaining = std::min(availableBytes, Limits::MaximumEvaluationBytes);
		for (const auto &key : document.Keyframes) {
			const auto bytes = KeyframePayloadBytes(key);
			if (!bytes || *bytes > remaining)
				return fail(Status::LimitExceeded, "key transfer exceeds the payload budget");
			remaining -= *bytes;
		}
		if (document.SourceAnimators ||
			std::any_of(axes.begin(), axes.end(), [](int8_t axis) { return axis != -1; })) {
			for (size_t index = 0; index < originals.size(); ++index) {
				const auto bytes = KeyframePayloadBytes(originals[index]);
				if (!bytes || *bytes > remaining)
					return fail(Status::LimitExceeded, "source key gesture copies exceed the payload budget");
				remaining -= *bytes;
				if (!ValidFrameTime(destinations[index]))
					return fail(Status::InvalidValue, "source key destination exceeds the authored range");
			}
			const uint64_t scratch = originals.size() * (sizeof(Keyframe) + sizeof(SourceKeyframeEdit));
			if (scratch > remaining)
				return fail(Status::LimitExceeded, "source key gesture scratch exceeds the payload budget");
			std::vector<Keyframe> replacements;
			std::vector<SourceKeyframeEdit> edits;
			replacements.reserve(originals.size());
			edits.reserve(originals.size());
			for (size_t index = 0; index < originals.size(); ++index) {
				replacements.push_back(originals[index]);
				(void)SetFrameTime(
					replacements.back(),
					clampZero && destinations[index].NegativeFrame ? FrameTime{} : destinations[index]
				);
				edits.push_back(
					{&originals[index], &replacements.back(), copy, axes.empty() ? int8_t{-1} : axes[index]}
				);
			}
			Document candidate;
			if (ApplySourceKeyframeEdits(document, edits, candidate, error, remaining - scratch) !=
				Status::Ok)
				return false;
			document = std::move(candidate);
			return true;
		}
		for (size_t index = 0; index < originals.size(); ++index) {
			const auto &key = originals[index];
			const auto bytes = KeyframePayloadBytes(key);
			if (!bytes || *bytes > remaining)
				return fail(Status::LimitExceeded, "key transfer exceeds the payload budget");
			remaining -= *bytes;
			if (!ValidFrameTime(GetFrameTime(key)) || !ValidFrameTime(destinations[index]))
				return fail(Status::InvalidValue, "shifted key frame exceeds the authored range");
			for (size_t prior = 0; prior < index; ++prior)
				if (key.NodeId == originals[prior].NodeId && key.Port == originals[prior].Port &&
					GetFrameTime(key) == GetFrameTime(originals[prior]))
					return fail(Status::InvalidValue, "key transfer repeats an original");
			if (!copy && std::find(document.Keyframes.begin(), document.Keyframes.end(), key) ==
							 document.Keyframes.end())
				return fail(Status::InvalidValue, "key changed while its move editor was open");
		}
		// Remove the whole move selection before replacement so one moved key cannot erase
		// another pinned original merely because its destination equals that original's old time.
		Document candidate = document;
		if (!copy)
			std::erase_if(candidate.Keyframes, [&](const auto &key) {
				return std::find(originals.begin(), originals.end(), key) != originals.end();
			});
		for (size_t index = 0; index < originals.size(); ++index) {
			const auto &original = originals[index];
			const FrameTime destination =
				clampZero && destinations[index].NegativeFrame ? FrameTime{} : destinations[index];
			bool alreadyMoved = false;
			if (!copy)
				for (size_t prior = 0; prior < index; ++prior) {
					const FrameTime previous =
						clampZero && destinations[prior].NegativeFrame ? FrameTime{} : destinations[prior];
					if (original.NodeId == originals[prior].NodeId &&
						original.Port == originals[prior].Port && destination == previous)
						alreadyMoved = true;
				}
			// Source release replaces the collision with the first selected moved object;
			// later selected objects removed by that replacement cannot be moved again.
			if (alreadyMoved) continue;
			Keyframe key = original;
			(void)SetFrameTime(key, destination);
			if (copy) {
				key.SourceKeyId.clear();
				key.SourceDriver.reset();
				// The native legacy driver also starts absent on a clone.
				key.SineDriver.reset();
			}
			std::erase_if(candidate.Keyframes, [&](const auto &existing) {
				return existing.NodeId == key.NodeId && existing.Port == key.Port &&
					   GetFrameTime(existing) == destination;
			});
			if (candidate.Keyframes.size() >= Limits::MaximumKeyframes)
				return fail(Status::LimitExceeded, "keyframe limit reached");
			if (key.Interpolation == "source" &&
				!EnsureSourceAnimationTrack(candidate, key.NodeId, key.Port, error))
				return false;
			candidate.Keyframes.push_back(std::move(key));
		}
		std::stable_sort(
			candidate.Keyframes.begin(), candidate.Keyframes.end(), [](const auto &left, const auto &right) {
				return CompareFrameTime(GetFrameTime(left), GetFrameTime(right)) < 0;
			}
		);
		PromoteFormatVersion(candidate);
		Plan plan;
		if (Compile(candidate, plan, error) != Status::Ok) return false;
		document = std::move(candidate);
		return true;
	}

	bool PasteImageGraphKeyframesToProperty(
		Document &document,
		std::span<const engine::imagegraph::Keyframe> clipboard,
		const engine::imagegraph::FrameTime &cursor,
		std::string_view nodeId,
		std::string_view property,
		engine::imagegraph::Diagnostic &error,
		uint64_t availableBytes,
		std::span<const int8_t> axes,
		int8_t targetAxis
	) {
		using namespace engine::imagegraph;
		error = {};
		const auto fail = [&](Status status, const char *message) {
			SetDiagnostic(error, status, nodeId, property, message);
			return false;
		};
		if ((!axes.empty() && axes.size() != clipboard.size()) || targetAxis < -1 || targetAxis > 1 ||
			std::any_of(axes.begin(), axes.end(), [](int8_t axis) { return axis < -1 || axis > 1; }))
			return fail(Status::InvalidValue, "clipboard component selectors are invalid");
		if (targetAxis < 0 && std::any_of(axes.begin(), axes.end(), [](int8_t axis) { return axis >= 0; }))
			return fail(Status::TypeMismatch, "scalar axis keys require a component paste target");
		if (clipboard.empty() || clipboard.size() > Limits::MaximumKeyframes || !ValidFrameTime(cursor))
			return fail(Status::InvalidValue, "clipboard or paste frame is invalid");
		const auto *target = FindAuthoredNode(document, nodeId);
		if (!target) return fail(Status::UnknownNode, "paste target no longer exists");
		const auto *targetSchema = FindSchema(target->Type);
		if (!targetSchema) return fail(Status::UnknownNode, "paste target type is unknown");
		const bool staticTarget = std::any_of(
			targetSchema->Properties.begin(), targetSchema->Properties.end(), [&](const auto &entry) {
				return entry.Id == property;
			}
		);
		const bool dynamicTarget =
			std::any_of(target->DynamicInputs.begin(), target->DynamicInputs.end(), [&](const auto &entry) {
				return entry.Id == property && IsAuthoredValueType(entry.Type);
			});
		if (!staticTarget && !dynamicTarget)
			return fail(Status::UnknownPort, "paste target property no longer exists");
		bool multiple = false;
		FrameTime anchor = GetFrameTime(clipboard.front());
		uint64_t remaining = std::min(availableBytes, Limits::MaximumEvaluationBytes);
		if (targetAxis >= 0 && clipboard.size() > remaining)
			return fail(Status::LimitExceeded, "mapped component selectors exceed the payload budget");
		if (targetAxis >= 0) remaining -= clipboard.size();
		size_t longestPort = property.size();
		for (const auto &entry : targetSchema->Properties)
			longestPort = std::max(longestPort, entry.Id.size());
		for (const auto &entry : target->DynamicInputs)
			longestPort = std::max(longestPort, entry.Id.size());
		for (const auto &key : clipboard) {
			if (targetAxis >= 0) {
				const auto *scalar = std::get_if<double>(&key.Data);
				if ((!scalar || !std::isfinite(*scalar)) && !std::holds_alternative<int64_t>(key.Data))
					return fail(Status::TypeMismatch, "component paste requires finite scalar keys");
			}
			multiple = targetAxis < 0 && (multiple || key.NodeId != clipboard.front().NodeId ||
										  key.Port != clipboard.front().Port);
			if (CompareFrameTime(GetFrameTime(key), anchor) < 0) anchor = GetFrameTime(key);
			const auto bytes = KeyframePayloadBytes(key);
			const uint64_t names = target->Id.size() + longestPort;
			if (!bytes || *bytes > remaining || names > remaining - *bytes)
				return fail(Status::LimitExceeded, "mapped clipboard exceeds the key payload budget");
			remaining -= *bytes + names;
		}
		std::vector<Keyframe> mapped;
		mapped.reserve(clipboard.size());
		const uint64_t spareKeys = (mapped.capacity() - clipboard.size()) * sizeof(Keyframe);
		if (spareKeys > remaining)
			return fail(Status::LimitExceeded, "mapped key capacity exceeds the payload budget");
		remaining -= spareKeys;
		std::vector<int8_t> mappedAxes;
		if (targetAxis >= 0) {
			mappedAxes.reserve(clipboard.size());
			if (mappedAxes.capacity() - clipboard.size() > remaining)
				return fail(Status::LimitExceeded, "mapped component capacity exceeds the payload budget");
			remaining -= mappedAxes.capacity() - clipboard.size();
		}
		Diagnostic skipped;
		for (const auto &key : clipboard) {
			std::string_view destination = property;
			if (multiple) {
				const auto *source = FindAuthoredNode(document, key.NodeId);
				if (!source) return fail(Status::UnknownNode, "copied source property no longer exists");
				const auto *sourceEntry = FindCatalogueEntry(source->Type),
						   *targetEntry = FindCatalogueEntry(target->Type);
				std::string_view name = key.Port;
				if (sourceEntry) {
					const auto input = std::find_if(
						sourceEntry->Inputs.begin(), sourceEntry->Inputs.end(), [&](const auto &entry) {
							return entry.Id == key.Port;
						}
					);
					if (input == sourceEntry->Inputs.end())
						return fail(
							Status::UnsupportedExecution, "source property display name is not represented"
						);
					name = input->Name;
				}
				destination = {};
				if (targetEntry) {
					const auto input = std::find_if(
						targetEntry->Inputs.begin(), targetEntry->Inputs.end(), [&](const auto &entry) {
							return entry.Name == name;
						}
					);
					if (input != targetEntry->Inputs.end()) destination = input->Id;
				} else {
					// Native-only schemas have durable property names rather than source display names.
					const auto input = std::find_if(
						targetSchema->Properties.begin(),
						targetSchema->Properties.end(),
						[&](const auto &entry) { return entry.Id == name; }
					);
					if (input != targetSchema->Properties.end()) destination = input->Id;
				}
				if (destination.empty()) {
					skipped = {
						Status::UnknownPort,
						std::string(nodeId),
						key.Port,
						"some copied properties have no matching target name"
					};
					continue;
				}
			}
			Keyframe clone;
			Diagnostic diagnostic;
			Status status;
			if (targetAxis >= 0) {
				clone = key;
				clone.NodeId = nodeId;
				clone.Port = destination;
				status = Status::Ok;
			} else
				status =
					PrepareKeyframeCloneForProperty(document, key, nodeId, destination, clone, diagnostic);
			if (status == Status::TypeMismatch) {
				skipped = std::move(diagnostic);
				continue;
			}
			if (status != Status::Ok) {
				error = std::move(diagnostic);
				return false;
			}
			clone.SourceDriver.reset();
			clone.SineDriver.reset();
			mapped.push_back(std::move(clone));
			if (targetAxis >= 0) mappedAxes.push_back(targetAxis);
		}
		if (mapped.empty()) {
			error = std::move(skipped);
			return targetAxis < 0;
		}
		if (!TransferImageGraphKeyframes(
				document, mapped, anchor, cursor, true, error, remaining, mappedAxes
			))
			return false;
		error = std::move(skipped);
		return true;
	}

	bool SetImageGraphKeyframeKind(
		Document &document,
		size_t index,
		engine::imagegraph::KeyframeKind kind,
		engine::imagegraph::Diagnostic &error,
		uint64_t availableBytes
	) {
		using engine::imagegraph::KeyframeKind;
		error = {};
		if (document.Keyframes.size() > engine::imagegraph::Limits::MaximumKeyframes) {
			SetDiagnostic(
				error, engine::imagegraph::Status::LimitExceeded, {}, {}, "keyframe limit exceeded"
			);
			return false;
		}
		if (index >= document.Keyframes.size() ||
			(kind != KeyframeKind::Normal && kind != KeyframeKind::Adder)) {
			SetDiagnostic(
				error, engine::imagegraph::Status::InvalidValue, {}, {}, "keyframe index or kind is invalid"
			);
			return false;
		}
		if (document.SourceAnimators && !ValidateCapturedEditBudget(availableBytes, error)) return false;
		if (document.SourceAnimators)
			return EditCapturedImageGraphKeys(
				document,
				document.Keyframes,
				[&](const auto &, size_t current) { return current == index; },
				[&](auto &key, size_t) { key.Kind = kind; },
				error,
				0,
				0,
				availableBytes
			);
		document.Keyframes[index].Kind = kind;
		if (kind == KeyframeKind::Adder)
			document.FormatVersion = std::max(document.FormatVersion, uint32_t{9});
		return true;
	}

	bool SetImageGraphKeyframeInterpolation(
		Document &document,
		size_t index,
		std::string_view rule,
		engine::imagegraph::Diagnostic &error,
		uint64_t availableBytes
	) {
		error = {};
		if (index >= document.Keyframes.size()) {
			SetDiagnostic(
				error, engine::imagegraph::Status::InvalidValue, {}, {}, "keyframe index is out of range"
			);
			return false;
		}
		if (rule != "step" && rule != "linear" && rule != "cubic" && rule != "source") {
			const engine::imagegraph::Keyframe &frame = document.Keyframes[index];
			SetDiagnostic(
				error,
				engine::imagegraph::Status::InvalidValue,
				frame.NodeId,
				frame.Port,
				"unsupported interpolation rule"
			);
			return false;
		}
		const engine::imagegraph::Keyframe &frame = document.Keyframes[index];
		if (rule != "source") {
			const bool sourceControls =
				std::any_of(
					document.Keyframes.begin(),
					document.Keyframes.end(),
					[&](const auto &key) {
						return key.NodeId == frame.NodeId && key.Port == frame.Port && key.SourceDriver;
					}
				) ||
				std::any_of(document.Tracks.begin(), document.Tracks.end(), [&](const auto &track) {
					return track.NodeId == frame.NodeId && track.Port == frame.Port && track.QuaternionMode;
				});
			if (sourceControls) {
				SetDiagnostic(
					error,
					engine::imagegraph::Status::InvalidValue,
					frame.NodeId,
					frame.Port,
					"remove source drivers and quaternion mode before changing interpolation"
				);
				return false;
			}
		}
		if (document.SourceAnimators &&
			!ValidateCapturedEditBudget(availableBytes, error, frame.NodeId, frame.Port))
			return false;
		if (document.SourceAnimators)
			return EditCapturedImageGraphKeys(
				document,
				document.Keyframes,
				[&](const auto &key, size_t) { return key.NodeId == frame.NodeId && key.Port == frame.Port; },
				[&](auto &key, size_t) {
					key.Interpolation = rule;
					if (rule == "source") {
						if (!key.Ease) key.Ease = engine::imagegraph::KeyframeEase{};
					} else
						key.Ease.reset();
				},
				error,
				0,
				0,
				availableBytes
			);
		if (rule == "source" && !EnsureSourceAnimationTrack(document, frame.NodeId, frame.Port, error))
			return false;
		SetTrackInterpolation(document, frame.NodeId, frame.Port, rule);
		return true;
	}

	bool SetImageGraphKeyframeEase(
		Document &document,
		size_t index,
		engine::imagegraph::KeyframeEase ease,
		engine::imagegraph::Diagnostic &error,
		uint64_t availableBytes
	) {
		error = {};
		if (index >= document.Keyframes.size()) {
			SetDiagnostic(
				error, engine::imagegraph::Status::InvalidValue, {}, {}, "keyframe index is out of range"
			);
			return false;
		}
		const auto validSide = [](const std::string &type) {
			return type == "linear" || type == "bezier" || type == "cut";
		};
		if (!validSide(ease.InType) || !validSide(ease.OutType) || !std::isfinite(ease.In.X) ||
			!std::isfinite(ease.In.Y) || !std::isfinite(ease.Out.X) || !std::isfinite(ease.Out.Y)) {
			const engine::imagegraph::Keyframe &frame = document.Keyframes[index];
			SetDiagnostic(
				error,
				engine::imagegraph::Status::InvalidValue,
				frame.NodeId,
				frame.Port,
				"keyframe easing needs registered side types and finite handles"
			);
			return false;
		}
		if (document.SourceAnimators) {
			const auto &frame = document.Keyframes[index];
			if (!ValidateCapturedEditBudget(availableBytes, error, frame.NodeId, frame.Port)) return false;
			return EditCapturedImageGraphKeys(
				document,
				document.Keyframes,
				[&](const auto &key, size_t) { return key.NodeId == frame.NodeId && key.Port == frame.Port; },
				[&](auto &key, size_t current) {
					key.Interpolation = "source";
					if (!key.Ease) key.Ease = engine::imagegraph::KeyframeEase{};
					if (current == index) key.Ease = ease;
				},
				error,
				0,
				0,
				availableBytes
			);
		}
		const std::string nodeId = document.Keyframes[index].NodeId;
		const std::string port = document.Keyframes[index].Port;
		if (!EnsureSourceAnimationTrack(document, nodeId, port, error)) return false;
		SetTrackInterpolation(document, nodeId, port, "source");
		document.Keyframes[index].Ease = std::move(ease);
		return true;
	}

	bool SetImageGraphKeyframeSourceDriver(
		Document &document,
		size_t index,
		const std::optional<engine::imagegraph::KeyframeSourceDriver> &driver,
		engine::imagegraph::Diagnostic &error,
		uint64_t availableBytes
	) {
		using namespace engine::imagegraph;
		error = {};
		if (index >= document.Keyframes.size()) {
			SetDiagnostic(error, Status::InvalidValue, {}, {}, "keyframe index is out of range");
			return false;
		}
		const auto &frame = document.Keyframes[index];
		if (driver && (frame.SineDriver || !ValidKeyframeSourceDriver(*driver))) {
			SetDiagnostic(
				error,
				Status::InvalidValue,
				frame.NodeId,
				frame.Port,
				"source driver needs valid controls and no legacy sine driver"
			);
			return false;
		}
		if (document.SourceAnimators) {
			if (!ValidateCapturedEditBudget(availableBytes, error, frame.NodeId, frame.Port)) return false;
			uint64_t extra = 0;
			if (driver) {
				if (const auto *curve = std::get_if<KeyframeCurveDriver>(&*driver)) {
					if (curve->Data.Anchors.capacity() >
						Limits::MaximumEvaluationBytes / sizeof(std::array<double, 6>)) {
						SetDiagnostic(
							error,
							Status::LimitExceeded,
							frame.NodeId,
							frame.Port,
							"source driver retained curve exceeds bounds"
						);
						return false;
					}
					extra = curve->Data.Anchors.capacity() * sizeof(std::array<double, 6>);
				}
				if (const auto *audio = std::get_if<KeyframeAudioDriver>(&*driver)) {
					if (audio->SourceId.capacity() > Limits::MaximumEvaluationBytes ||
						audio->Metric.capacity() >
							Limits::MaximumEvaluationBytes - audio->SourceId.capacity()) {
						SetDiagnostic(
							error,
							Status::LimitExceeded,
							frame.NodeId,
							frame.Port,
							"source driver retained text exceeds bounds"
						);
						return false;
					}
					extra = audio->SourceId.capacity() + audio->Metric.capacity();
				}
			}
			return EditCapturedImageGraphKeys(
				document,
				document.Keyframes,
				[&](const auto &key, size_t current) {
					return driver ? key.NodeId == frame.NodeId && key.Port == frame.Port : current == index;
				},
				[&](auto &key, size_t current) {
					if (driver) {
						key.Interpolation = "source";
						if (!key.Ease) key.Ease = KeyframeEase{};
					}
					if (current == index) key.SourceDriver = driver;
				},
				error,
				extra,
				extra + sizeof(driver),
				availableBytes
			);
		}
		Document candidate = document;
		if (driver) {
			if (!EnsureSourceAnimationTrack(candidate, frame.NodeId, frame.Port, error)) return false;
			SetTrackInterpolation(candidate, frame.NodeId, frame.Port, "source");
		}
		candidate.Keyframes[index].SourceDriver = driver;
		PromoteFormatVersion(candidate);
		Plan plan;
		if (Compile(candidate, plan, error) != Status::Ok) return false;
		document.Keyframes = std::move(candidate.Keyframes);
		document.Tracks = std::move(candidate.Tracks);
		document.FormatVersion = candidate.FormatVersion;
		return true;
	}

	bool SetImageGraphTrackQuaternionMode(
		Document &document,
		std::string_view nodeId,
		std::string_view property,
		std::optional<int64_t> mode,
		engine::imagegraph::Diagnostic &error,
		uint64_t availableBytes
	) {
		using namespace engine::imagegraph;
		error = {};
		if (mode && (*mode < 0 || *mode > 1)) {
			SetDiagnostic(error, Status::InvalidValue, nodeId, property, "quaternion mode is raw or Euler");
			return false;
		}
		if (document.SourceAnimators) {
			const auto existing =
				std::find_if(document.Tracks.begin(), document.Tracks.end(), [&](const auto &track) {
					return track.NodeId == nodeId && track.Port == property;
				});
			if (!AdmitCapturedTrackDraft(
					document,
					existing != document.Tracks.end() ? &*existing : nullptr,
					nodeId,
					property,
					availableBytes,
					error
				))
				return false;
			AnimationTrack replacement =
				existing != document.Tracks.end()
					? *existing
					: AnimationTrack{std::string(nodeId), std::string(property), "hold", -1};
			replacement.QuaternionMode = mode;
			const SourceTrackTransition transition{nodeId, property, &replacement, mode.has_value()};
			return ApplySourceTrackTransition(document, transition, document, error, availableBytes) ==
				   Status::Ok;
		}
		Document candidate = document;
		if (!EnsureSourceAnimationTrack(candidate, nodeId, property, error)) return false;
		if (mode) SetTrackInterpolation(candidate, nodeId, property, "source");
		const auto track =
			std::find_if(candidate.Tracks.begin(), candidate.Tracks.end(), [&](const auto &entry) {
				return entry.NodeId == nodeId && entry.Port == property;
			});
		track->QuaternionMode = mode;
		PromoteFormatVersion(candidate);
		Plan plan;
		if (Compile(candidate, plan, error) != Status::Ok) return false;
		document.Keyframes = std::move(candidate.Keyframes);
		document.Tracks = std::move(candidate.Tracks);
		document.FormatVersion = candidate.FormatVersion;
		return true;
	}

	bool SetImageGraphKeyframeSineDriver(
		Document &document,
		size_t index,
		std::optional<engine::imagegraph::KeyframeSineDriver> driver,
		engine::imagegraph::Diagnostic &error,
		uint64_t availableBytes
	) {
		error = {};
		if (index >= document.Keyframes.size()) {
			SetDiagnostic(
				error, engine::imagegraph::Status::InvalidValue, {}, {}, "keyframe index is out of range"
			);
			return false;
		}
		if (driver && document.Keyframes[index].SourceDriver) {
			const auto &frame = document.Keyframes[index];
			SetDiagnostic(
				error,
				engine::imagegraph::Status::InvalidValue,
				frame.NodeId,
				frame.Port,
				"remove the source driver before adding a legacy sine driver"
			);
			return false;
		}
		if (driver) {
			const engine::imagegraph::Keyframe &frame = document.Keyframes[index];
			if (!std::holds_alternative<double>(frame.Data)) {
				SetDiagnostic(
					error,
					engine::imagegraph::Status::TypeMismatch,
					frame.NodeId,
					frame.Port,
					"sine keyframe driver requires a scalar value"
				);
				return false;
			}
			if (!std::isfinite(driver->Frequency) || !std::isfinite(driver->Amplitude) ||
				!std::isfinite(driver->Phase) || !std::isfinite(driver->Smooth) || driver->Smooth < 0.0 ||
				driver->Smooth > 1.0) {
				SetDiagnostic(
					error,
					engine::imagegraph::Status::InvalidValue,
					frame.NodeId,
					frame.Port,
					"sine driver parameters must be finite and smooth must be in [0, 1]"
				);
				return false;
			}
		}
		if (document.SourceAnimators && !ValidateCapturedEditBudget(availableBytes, error)) return false;
		if (document.SourceAnimators)
			return EditCapturedImageGraphKeys(
				document,
				document.Keyframes,
				[&](const auto &, size_t current) { return current == index; },
				[&](auto &key, size_t) { key.SineDriver = driver; },
				error,
				0,
				0,
				availableBytes
			);
		document.Keyframes[index].SineDriver = std::move(driver);
		if (document.Keyframes[index].SineDriver)
			document.FormatVersion = std::max(document.FormatVersion, 6u);
		return true;
	}

	bool SetImageGraphTimeline(
		Document &document,
		engine::imagegraph::TimelineSettings timeline,
		engine::imagegraph::Diagnostic &error
	) {
		error = {};
		if (timeline.Frames == 0 || timeline.Frames > engine::imagegraph::Limits::MaximumTick + 1 ||
			timeline.First > timeline.Last || timeline.Last >= timeline.Frames ||
			(timeline.Playback != "loop" && timeline.Playback != "stop" && timeline.Playback != "pingpong") ||
			!ValidFramesPerSecond(timeline.FramesPerSecond) ||
			(timeline.SourceBounds &&
			 !engine::imagegraph::ValidSourceAuthoringFrameBounds(*timeline.SourceBounds))) {
			SetDiagnostic(
				error,
				engine::imagegraph::Status::InvalidValue,
				{},
				"timeline",
				"timeline range, playback mode or frame rate is invalid"
			);
			return false;
		}
		for (const engine::imagegraph::AnimationTrack &track : document.Tracks) {
			if (track.End != "wrap") continue;
			for (const engine::imagegraph::Keyframe &keyframe : document.Keyframes) {
				if (keyframe.NodeId == track.NodeId && keyframe.Port == track.Port &&
					keyframe.Tick >= timeline.Frames) {
					SetDiagnostic(
						error,
						engine::imagegraph::Status::InvalidValue,
						track.NodeId,
						track.Port,
						"timeline frame count must include every wrap key"
					);
					return false;
				}
			}
		}
		document.Timeline = std::move(timeline);
		PromoteFormatVersion(document);
		return true;
	}

	bool RemoveImageGraphTimeline(Document &document, engine::imagegraph::Diagnostic &error) {
		error = {};
		const auto wrap = std::find_if(document.Tracks.begin(), document.Tracks.end(), [](const auto &track) {
			return track.End == "wrap";
		});
		if (wrap != document.Tracks.end()) {
			SetDiagnostic(
				error,
				engine::imagegraph::Status::InvalidValue,
				wrap->NodeId,
				wrap->Port,
				"timeline frame count is required by this wrap track"
			);
			return false;
		}
		document.Timeline.reset();
		return true;
	}

	bool SetImageGraphAnimationTrack(
		Document &document,
		std::string_view nodeId,
		std::string_view property,
		std::string end,
		int64_t loopRange,
		engine::imagegraph::Diagnostic &error,
		uint64_t availableBytes
	) {
		error = {};
		const auto fail = [&](engine::imagegraph::Status code, std::string message) {
			SetDiagnostic(error, code, nodeId, property, std::move(message));
			return false;
		};
		const Node *node = FindAuthoredNode(document, nodeId);
		if (node == nullptr) return fail(engine::imagegraph::Status::UnknownNode, "node does not exist");
		const engine::imagegraph::NodeSchema *schema = engine::imagegraph::FindSchema(node->Type);
		if (schema == nullptr) return fail(engine::imagegraph::Status::UnknownNode, "node type is unknown");
		if (std::none_of(
				schema->Properties.begin(),
				schema->Properties.end(),
				[&](const auto &entry) { return entry.Id == property; }
			) &&
			(!schema->DynamicInputs ||
			 std::none_of(node->DynamicInputs.begin(), node->DynamicInputs.end(), [&](const auto &entry) {
				 return entry.Id == property;
			 })))
			return fail(engine::imagegraph::Status::UnknownPort, "property is not declared");
		if (end != "hold" && end != "loop" && end != "ping" && end != "wrap")
			return fail(engine::imagegraph::Status::InvalidValue, "unsupported animation track end policy");
		const size_t keyCount = static_cast<size_t>(
			std::count_if(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &keyframe) {
				return keyframe.NodeId == nodeId && keyframe.Port == property;
			})
		);
		if (keyCount == 0) return fail(engine::imagegraph::Status::InvalidValue, "property has no keyframes");
		if (loopRange < -1 || loopRange >= static_cast<int64_t>(keyCount)) {
			return fail(
				engine::imagegraph::Status::InvalidValue, "animation track loop tail is outside its key range"
			);
		}
		if (end == "wrap") {
			if (!document.Timeline)
				return fail(
					engine::imagegraph::Status::InvalidValue, "wrap policy needs a saved timeline frame count"
				);
			if (std::any_of(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &keyframe) {
					return keyframe.NodeId == nodeId && keyframe.Port == property &&
						   keyframe.Tick >= document.Timeline->Frames;
				})) {
				return fail(
					engine::imagegraph::Status::InvalidValue,
					"timeline frame count must include every wrap key"
				);
			}
		}
		auto track = std::find_if(document.Tracks.begin(), document.Tracks.end(), [&](const auto &candidate) {
			return candidate.NodeId == nodeId && candidate.Port == property;
		});
		if (document.SourceAnimators) {
			if (!AdmitCapturedTrackDraft(
					document,
					track != document.Tracks.end() ? &*track : nullptr,
					nodeId,
					property,
					availableBytes,
					error,
					end.capacity()
				))
				return false;
			engine::imagegraph::AnimationTrack replacement =
				track != document.Tracks.end() ? *track
											   : engine::imagegraph::AnimationTrack{
													 std::string(nodeId), std::string(property), "hold", -1
												 };
			replacement.End = std::move(end);
			replacement.LoopRange = loopRange;
			const engine::imagegraph::SourceTrackTransition transition{nodeId, property, &replacement};
			return engine::imagegraph::ApplySourceTrackTransition(
					   document, transition, document, error, availableBytes
				   ) == engine::imagegraph::Status::Ok;
		}
		if (track == document.Tracks.end()) {
			if (document.Tracks.size() >= engine::imagegraph::Limits::MaximumTracks)
				return fail(engine::imagegraph::Status::LimitExceeded, "animation track limit reached");
			document.Tracks.push_back(
				{std::string(nodeId), std::string(property), std::move(end), loopRange}
			);
		} else {
			track->End = std::move(end);
			track->LoopRange = loopRange;
		}
		PromoteFormatVersion(document);
		return true;
	}

	bool RemoveImageGraphAnimationTrack(
		Document &document,
		std::string_view nodeId,
		std::string_view property,
		engine::imagegraph::Diagnostic &error,
		uint64_t availableBytes
	) {
		error = {};
		const auto found =
			std::find_if(document.Tracks.begin(), document.Tracks.end(), [&](const auto &track) {
				return track.NodeId == nodeId && track.Port == property;
			});
		if (found == document.Tracks.end()) {
			SetDiagnostic(
				error,
				engine::imagegraph::Status::InvalidValue,
				nodeId,
				property,
				"animation track override does not exist"
			);
			return false;
		}
		if (std::any_of(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &keyframe) {
				return keyframe.NodeId == nodeId && keyframe.Port == property &&
					   keyframe.Interpolation == "source";
			})) {
			SetDiagnostic(
				error,
				engine::imagegraph::Status::InvalidValue,
				nodeId,
				property,
				"source easing requires an animation track; change interpolation before removing it"
			);
			return false;
		}
		if (document.SourceAnimators) {
			const engine::imagegraph::SourceTrackTransition transition{nodeId, property};
			return engine::imagegraph::ApplySourceTrackTransition(
					   document, transition, document, error, availableBytes
				   ) == engine::imagegraph::Status::Ok;
		}
		document.Tracks.erase(found);
		return true;
	}

	bool SetImageGraphOutput(
		Document &document,
		std::string_view outputId,
		std::string_view nodeId,
		std::string_view port,
		engine::imagegraph::Diagnostic &error
	) {
		error = {};
		const auto fail = [&](engine::imagegraph::Status code, std::string message) {
			SetDiagnostic(error, code, nodeId, port, std::move(message));
			return false;
		};
		auto output = std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const auto &entry) {
			return entry.Id == outputId;
		});
		if (output == document.Outputs.end())
			return fail(engine::imagegraph::Status::InvalidOutput, "output does not exist");
		const Node *node = FindAuthoredNode(document, nodeId);
		if (node == nullptr) return fail(engine::imagegraph::Status::UnknownNode, "node does not exist");
		const engine::imagegraph::NodeSchema *schema = engine::imagegraph::FindSchema(node->Type);
		if (schema == nullptr) return fail(engine::imagegraph::Status::UnknownNode, "node type is unknown");
		const auto ports = detail::ImageGraphOutputPorts(*node);
		if (std::none_of(ports.begin(), ports.end(), [&](const auto &entry) { return entry.Id == port; }))
			return fail(engine::imagegraph::Status::UnknownPort, "preview output port is not declared");
		output->NodeId = nodeId;
		output->Port = port;
		return true;
	}

	std::optional<engine::imagegraph::FrameTime>
	PreviousImageGraphKey(const Document &document, engine::imagegraph::FrameTime frame) {
		std::optional<engine::imagegraph::FrameTime> result;
		for (const auto &key : document.Keyframes) {
			const auto time = engine::imagegraph::GetFrameTime(key);
			if (engine::imagegraph::CompareFrameTime(time, frame) < 0 &&
				(!result || engine::imagegraph::CompareFrameTime(time, *result) > 0))
				result = time;
		}
		return result;
	}
	std::optional<engine::imagegraph::FrameTime>
	NextImageGraphKey(const Document &document, engine::imagegraph::FrameTime frame) {
		std::optional<engine::imagegraph::FrameTime> result;
		for (const auto &key : document.Keyframes) {
			const auto time = engine::imagegraph::GetFrameTime(key);
			if (engine::imagegraph::CompareFrameTime(time, frame) > 0 &&
				(!result || engine::imagegraph::CompareFrameTime(time, *result) < 0))
				result = time;
		}
		return result;
	}
	std::optional<uint64_t> PreviousImageGraphKey(const Document &document, uint64_t tick) {
		std::optional<uint64_t> previous;
		for (const engine::imagegraph::Keyframe &frame : document.Keyframes) {
			if (!frame.NegativeFrame && frame.Subframe == 0 && frame.Tick < tick &&
				(!previous || frame.Tick > *previous))
				previous = frame.Tick;
		}
		return previous;
	}

	std::optional<uint64_t> NextImageGraphKey(const Document &document, uint64_t tick) {
		std::optional<uint64_t> next;
		for (const engine::imagegraph::Keyframe &frame : document.Keyframes) {
			if (!frame.NegativeFrame && frame.Subframe == 0 && frame.Tick > tick &&
				(!next || frame.Tick < *next))
				next = frame.Tick;
		}
		return next;
	}

	std::vector<ImageComposerSinkRow>
	DescribeImageComposerSinks(const Document &document, std::string_view selectedOutput) {
		std::vector<ImageComposerSinkRow> rows;
		rows.reserve(document.Outputs.size());
		for (const engine::imagegraph::Output &output : document.Outputs) {
			rows.push_back(
				{output.Id,
				 output.NodeId,
				 output.Port,
				 output.Id == selectedOutput,
				 FindAuthoredNode(document, output.NodeId) != nullptr}
			);
		}
		return rows;
	}

	bool IsSafeImageGraphName(std::string_view name) {
		if (name.empty() || name.size() > 255) return false;
		return std::all_of(name.begin(), name.end(), [](char character) {
			return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
				   (character >= '0' && character <= '9') || character == '_' || character == '-';
		});
	}

	std::filesystem::path
	ImageGraphDocumentPath(const std::filesystem::path &assetsDirectory, std::string_view name) {
		if (!IsSafeImageGraphName(name)) return {};
		return assetsDirectory / "imagegraphs" / (std::string(name) + ".graph");
	}

	bool ReadImageGraphDocument(
		const std::filesystem::path &assetsDirectory,
		std::string_view name,
		Document &document,
		std::string &error
	) {
		error.clear();
		const std::filesystem::path path = ImageGraphDocumentPath(assetsDirectory, name);
		if (path.empty()) {
			error = "graph name must be 1 to 255 ASCII letters, digits, hyphens or underscores";
			return false;
		}

		std::error_code filesystemError;
		const uintmax_t size = std::filesystem::file_size(path, filesystemError);
		if (filesystemError) {
			error = "could not read native graph file size: " + filesystemError.message();
			return false;
		}
		if (size > IMAGE_GRAPH_FILE_MAXIMUM_BYTES) {
			error = "native graph document exceeds the 8 MiB Studio limit";
			return false;
		}

		std::ifstream file(path, std::ios::binary);
		if (!file) {
			error = "could not open native graph document";
			return false;
		}
		std::string text(static_cast<size_t>(size), '\0');
		if (!text.empty()) {
			file.read(text.data(), static_cast<std::streamsize>(text.size()));
			if (!file || static_cast<size_t>(file.gcount()) != text.size()) {
				error = "native graph file changed or ended while it was being read";
				return false;
			}
		}

		Document candidate;
		engine::imagegraph::Diagnostic diagnostic;
		if (engine::imagegraph::Read(text, candidate, diagnostic) != engine::imagegraph::Status::Ok) {
			error = diagnostic.Message.empty() ? "native graph document is malformed" : diagnostic.Message;
			return false;
		}
		if (engine::imagegraph::Migrate(candidate, diagnostic) != engine::imagegraph::Status::Ok) {
			error =
				diagnostic.Message.empty() ? "native graph document cannot be migrated" : diagnostic.Message;
			return false;
		}
		document = std::move(candidate);
		return true;
	}

	bool BuildImageGraphBinding(
		const ImageGraphBindingDraft &draft,
		const Document &document,
		engine::scene::ImageGraphBinding &binding,
		std::string &error
	) {
		error.clear();
		const auto validSelector = [](std::string_view value) {
			return !value.empty() && value.size() <= engine::scene::IMAGE_GRAPH_BINDING_MAXIMUM_NAME_BYTES &&
				   value.find('\0') == std::string_view::npos;
		};
		if (!IsSafeImageGraphName(draft.Graph)) {
			error = "graph name must be 1 to 255 ASCII letters, digits, hyphens or underscores";
			return false;
		}
		if (!validSelector(draft.Output)) {
			error = "output id must be 1 to 255 bytes and contain no NUL";
			return false;
		}
		if (!validSelector(draft.Texture)) {
			error = "texture name must be 1 to 255 bytes and contain no NUL";
			return false;
		}
		if (draft.FixedTick > engine::imagegraph::Limits::MaximumTick) {
			error = "fixed tick exceeds the image graph timeline limit";
			return false;
		}
		const auto output =
			std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const auto &candidate) {
				return candidate.Id == draft.Output;
			});
		if (output == document.Outputs.end()) {
			error = "the selected output id is not present in the native graph document";
			return false;
		}
		engine::imagegraph::Plan plan;
		engine::imagegraph::Diagnostic diagnostic;
		if (engine::imagegraph::Compile(document, plan, diagnostic) != engine::imagegraph::Status::Ok) {
			error =
				diagnostic.Message.empty() ? "native graph document cannot be compiled" : diagnostic.Message;
			return false;
		}
		const Node *outputNode = FindAuthoredNode(document, output->NodeId);
		const engine::imagegraph::NodeSchema *outputSchema =
			outputNode == nullptr ? nullptr : engine::imagegraph::FindSchema(outputNode->Type);
		if (outputSchema == nullptr ||
			std::none_of(outputSchema->Ports.begin(), outputSchema->Ports.end(), [&](const auto &port) {
				return port.Id == output->Port &&
					   port.Direction == engine::imagegraph::PortDirection::Output &&
					   port.Type == engine::imagegraph::ValueType::Image;
			})) {
			error = "the selected output does not produce a single image for this texture binding";
			return false;
		}

		engine::scene::ImageGraphBinding candidate;
		candidate.Graph = engine::core::Name(draft.Graph);
		candidate.Output = engine::core::Name(draft.Output);
		candidate.Texture = engine::core::Name(draft.Texture);
		candidate.Seed = draft.Seed;
		candidate.FixedTick = draft.FixedTick;
		candidate.TickPolicy = draft.TickPolicy;
		candidate.ColorSpace = draft.ColorSpace;
		if (!engine::scene::IsValidImageGraphBinding(candidate)) {
			error = "tick policy or binding selectors are invalid";
			return false;
		}

		binding = candidate;
		return true;
	}

	std::string PxcxOpaqueNodeType(std::string_view foreignType) {
		return "pxcx.opaque/" + std::string(foreignType);
	}

	std::string PxcxInputPortId(uint32_t inputIndex) {
		return "input-" + std::to_string(inputIndex);
	}

	std::string PxcxOutputPortId(uint32_t outputIndex) {
		return "output-" + std::to_string(outputIndex);
	}

	bool ProjectPxcxImageGraph(
		const engine::bake::PxcxArchive &archive, PxcxImageGraphProjection &projection, std::string &error
	) {
		error.clear();
		if (archive.Nodes.size() > engine::imagegraph::Limits::MaximumNodes ||
			archive.Links.size() > engine::imagegraph::Limits::MaximumLinks) {
			error = "PXCX graph exceeds Studio canvas limits; the source archive remains available";
			return false;
		}

		PxcxImageGraphProjection candidate;
		candidate.Graph.Nodes.reserve(archive.Nodes.size());
		candidate.Graph.Links.reserve(archive.Links.size());
		candidate.Diagnostics.reserve(archive.Nodes.size());
		std::unordered_set<std::string> nodeIds;
		nodeIds.reserve(archive.Nodes.size());
		for (const engine::bake::PxcxNodeFact &fact : archive.Nodes) {
			if (fact.Id.empty() || fact.Type.empty() || !nodeIds.insert(fact.Id).second ||
				fact.Id.size() > engine::imagegraph::Limits::MaximumTextBytes ||
				fact.Type.size() + 12 > engine::imagegraph::Limits::MaximumTextBytes ||
				!std::isfinite(fact.X) || !std::isfinite(fact.Y)) {
				error = "PXCX node facts cannot be represented as durable imagegraph ids and positions";
				return false;
			}
			candidate.Graph.Nodes.push_back(
				{fact.Id, PxcxOpaqueNodeType(fact.Type), {}, {fact.X, fact.Y}, {}, {}}
			);
			candidate.Diagnostics.push_back(
				{engine::imagegraph::Status::UnknownNode,
				 fact.Id,
				 {},
				 "PXCX node type '" + fact.Type +
					 "' is preserved in the source archive and has no native execution schema"}
			);
		}
		for (const engine::bake::PxcxLinkFact &fact : archive.Links) {
			if (!nodeIds.contains(fact.FromNode) || !nodeIds.contains(fact.ToNode)) {
				error = "PXCX link references a node outside its imported graph";
				return false;
			}
			candidate.Graph.Links.push_back(
				{fact.FromNode,
				 PxcxOutputPortId(fact.FromIndex),
				 fact.ToNode,
				 PxcxInputPortId(fact.ToInputIndex)}
			);
		}
		projection = std::move(candidate);
		return true;
	}

	ImageGraphHistory::ImageGraphHistory(size_t capacity, size_t byteCapacity)
		: Capacity(capacity), ByteCapacity(byteCapacity) {}

	std::optional<size_t> ImageGraphHistory::CollectionBytes(const CollectionSnapshot &items) const {
		if (!items) return size_t{0};
		if (items->size() > engine::imagegraph::Limits::MaximumNodes ||
			items->capacity() > ByteCapacity / sizeof(CollectionMetadata))
			return {};
		size_t remaining = ByteCapacity - items->capacity() * sizeof(CollectionMetadata);
		for (const auto &item : *items) {
			if (item.NodeId.empty() || item.NodeId.size() > engine::bake::PxcxLimits::MaximumNodeTextBytes ||
				item.MetadataJson.size() > engine::bake::PxcxLimits::MaximumMetadataBytes ||
				item.NodeId.capacity() > remaining)
				return {};
			remaining -= item.NodeId.capacity();
			if (item.MetadataJson.capacity() > remaining) return {};
			remaining -= item.MetadataJson.capacity();
		}
		return ByteCapacity - remaining;
	}

	bool ImageGraphHistory::Fits(
		std::span<const Snapshot> undo,
		std::span<const Snapshot> redo,
		const SourceSnapshot &current,
		const Snapshot *extra,
		const SourceSnapshot &baseline,
		size_t extraTextBytes,
		const CollectionSnapshot &currentCollections,
		const CollectionSnapshot &baselineCollections
	) const {
		if (!ByteCapacity || extraTextBytes > ByteCapacity) return false;
		size_t remaining = ByteCapacity - extraTextBytes;
		// grug source epochs share bytes. pointer identity counts each retained vector once.
		std::array<const std::vector<std::byte> *, engine::imagegraph::Limits::MaximumNodes + 2> sources{};
		size_t sourceCount = 0;
		const auto chargeSource = [&](const SourceSnapshot &source) {
			if (!source) return true;
			if (source->empty() || source->capacity() > engine::bake::PxcxLimits::MaximumArchiveBytes)
				return false;
			if (std::find(sources.begin(), sources.begin() + sourceCount, source.get()) !=
				sources.begin() + sourceCount)
				return true;
			if (sourceCount == sources.size() || source->capacity() > remaining) return false;
			remaining -= source->capacity();
			sources[sourceCount++] = source.get();
			return true;
		};
		std::array<const std::vector<CollectionMetadata> *, engine::imagegraph::Limits::MaximumNodes + 2>
			managers{};
		size_t managerCount = 0;
		const auto chargeCollections = [&](const CollectionSnapshot &items) {
			if (!items) return true;
			if (std::find(managers.begin(), managers.begin() + managerCount, items.get()) !=
				managers.begin() + managerCount)
				return true;
			const auto bytes = CollectionBytes(items);
			if (managerCount == managers.size() || !bytes || *bytes > remaining) return false;
			remaining -= *bytes;
			managers[managerCount++] = items.get();
			return true;
		};
		const auto charge = [&](const Snapshot &entry) {
			if (entry.Text.size() > remaining) return false;
			remaining -= entry.Text.size();
			return chargeSource(entry.Source ? entry.Source : baseline) &&
				   chargeCollections(entry.Collections ? entry.Collections : baselineCollections);
		};
		if (!chargeSource(current) || !chargeCollections(currentCollections)) return false;
		for (const auto &entry : undo)
			if (!charge(entry)) return false;
		for (const auto &entry : redo)
			if (!charge(entry)) return false;
		return !extra || charge(*extra);
	}

	void ImageGraphHistory::Record(const Document &before, const Document &after) {
		if (before == after) return;
		const auto drop = [&] {
			UndoSnapshots.clear();
			RedoSnapshots.clear();
		};
		if (!Capacity || !ByteCapacity) {
			drop();
			return;
		}
		// grug legacy Record stores before only. restoring an oversized after may still refuse.
		Snapshot snapshot{engine::imagegraph::Write(before), CurrentSource, Collections};
		size_t discard = UndoSnapshots.size() >= Capacity ? UndoSnapshots.size() - Capacity + 1 : 0;
		while (
			!Fits(std::span(UndoSnapshots).subspan(discard), {}, CurrentSource, &snapshot, {}, 0, Collections)
		) {
			if (discard == UndoSnapshots.size()) {
				drop();
				return;
			}
			++discard;
		}
		UndoSnapshots.reserve(UndoSnapshots.size() + 1);
		RedoSnapshots.clear();
		UndoSnapshots.erase(UndoSnapshots.begin(), UndoSnapshots.begin() + discard);
		UndoSnapshots.push_back(std::move(snapshot));
	}

	bool ImageGraphHistory::TryRecord(const Document &before, const Document &after) {
		return TryRecord(before, after, Admission{});
	}
	bool
	ImageGraphHistory::TryRecord(const Document &before, const Document &after, const Admission &admit) try {
		const SourceAdmission sourceAdmission =
			admit ? SourceAdmission{[&](const Document &old,
										const Document &changed,
										const SourceSnapshot &,
										const SourceSnapshot &) { return admit(old, changed); }}
				  : SourceAdmission{};
		return TryRecord(before, after, CurrentSource, CurrentSource, sourceAdmission);
	} catch (const std::bad_alloc &) {
		return false;
	}

	bool ImageGraphHistory::TryRecord(
		const Document &before,
		const Document &after,
		const SourceSnapshot &beforeSource,
		const SourceSnapshot &afterSource,
		const SourceAdmission &admit
	) try {
		return TryRecord(before, after, beforeSource, afterSource, Collections, Collections, admit);
	} catch (const std::bad_alloc &) {
		return false;
	}

	bool ImageGraphHistory::TryRecord(
		const Document &before,
		const Document &after,
		const SourceSnapshot &beforeSource,
		const SourceSnapshot &afterSource,
		const CollectionSnapshot &beforeCollections,
		const CollectionSnapshot &afterCollections,
		const SourceAdmission &admit
	) try {
		const auto sourceFits = [&](const SourceSnapshot &source) {
			return !source || (!source->empty() && source->capacity() <= ByteCapacity &&
							   source->capacity() <= engine::bake::PxcxLimits::MaximumArchiveBytes);
		};
		if (!sourceFits(beforeSource) || !sourceFits(afterSource) || !CollectionBytes(beforeCollections) ||
			!CollectionBytes(afterCollections))
			return false;
		const auto sameSource = [](const SourceSnapshot &a, const SourceSnapshot &b) {
			return a == b || (a && b && *a == *b);
		};
		const bool initializeSource =
			!CurrentSource && beforeSource &&
			std::none_of(
				UndoSnapshots.begin(),
				UndoSnapshots.end(),
				[](const auto &entry) { return bool(entry.Source); }
			) &&
			std::none_of(RedoSnapshots.begin(), RedoSnapshots.end(), [](const auto &entry) {
				return bool(entry.Source);
			});
		if (!initializeSource && !sameSource(CurrentSource, beforeSource)) return false;
		const auto sameCollections = [](const CollectionSnapshot &a, const CollectionSnapshot &b) {
			return a == b || (a && b && *a == *b);
		};
		const bool initializeCollections =
			!Collections && beforeCollections &&
			std::none_of(
				UndoSnapshots.begin(),
				UndoSnapshots.end(),
				[](const auto &entry) { return bool(entry.Collections); }
			) &&
			std::none_of(RedoSnapshots.begin(), RedoSnapshots.end(), [](const auto &entry) {
				return bool(entry.Collections);
			});
		if (!initializeCollections && !sameCollections(Collections, beforeCollections)) return false;
		if (!initializeCollections && before == after && sameSource(beforeSource, afterSource) &&
			sameCollections(beforeCollections, afterCollections))
			return true;
		if (!Capacity || !ByteCapacity) return false;
		Snapshot snapshot{engine::imagegraph::Write(before), beforeSource, beforeCollections};
		const std::string afterText = engine::imagegraph::Write(after);
		if (snapshot.Text.empty() || afterText.empty() || snapshot.Text.size() > ByteCapacity ||
			afterText.size() > ByteCapacity)
			return false;
		const auto baseline = initializeSource ? beforeSource : SourceSnapshot{};
		const size_t extraBytes =
			afterText.size() > snapshot.Text.size() ? afterText.size() - snapshot.Text.size() : 0;
		size_t discard = UndoSnapshots.size() >= Capacity ? UndoSnapshots.size() - Capacity + 1 : 0;
		while (!Fits(
			std::span(UndoSnapshots).subspan(discard),
			{},
			afterSource,
			&snapshot,
			baseline,
			extraBytes,
			afterCollections,
			initializeCollections ? beforeCollections : CollectionSnapshot{}
		)) {
			if (discard == UndoSnapshots.size()) return false;
			++discard;
		}
		UndoSnapshots.reserve(UndoSnapshots.size() + 1);
		if (admit && !admit(before, after, beforeSource, afterSource)) return false;
		// grug no fallible work follows admission. source and native history become one transition.
		static_assert(std::is_nothrow_move_constructible_v<Snapshot>);
		static_assert(std::is_nothrow_move_assignable_v<Snapshot>);
		static_assert(std::is_nothrow_copy_assignable_v<SourceSnapshot>);
		RedoSnapshots.clear();
		UndoSnapshots.erase(UndoSnapshots.begin(), UndoSnapshots.begin() + discard);
		if (initializeSource)
			for (auto &entry : UndoSnapshots)
				entry.Source = beforeSource;
		if (initializeCollections)
			for (auto &entry : UndoSnapshots)
				entry.Collections = beforeCollections;
		UndoSnapshots.push_back(std::move(snapshot));
		CurrentSource = afterSource;
		Collections = afterCollections;
		return true;
	} catch (const std::bad_alloc &) {
		return false;
	}

	bool ImageGraphHistory::Undo(Document &document) {
		return Restore(document, false, SourceAdmission{});
	}
	bool ImageGraphHistory::Redo(Document &document) {
		return Restore(document, true, SourceAdmission{});
	}
	bool ImageGraphHistory::Undo(Document &document, const Admission &admit) try {
		return Undo(
			document,
			SourceAdmission{[&](const Document &before,
								const Document &after,
								const SourceSnapshot &,
								const SourceSnapshot &) { return !admit || admit(before, after); }}
		);
	} catch (const std::bad_alloc &) {
		return false;
	}
	bool ImageGraphHistory::Redo(Document &document, const Admission &admit) try {
		return Redo(
			document,
			SourceAdmission{[&](const Document &before,
								const Document &after,
								const SourceSnapshot &,
								const SourceSnapshot &) { return !admit || admit(before, after); }}
		);
	} catch (const std::bad_alloc &) {
		return false;
	}
	bool ImageGraphHistory::Undo(Document &document, const SourceAdmission &admit) {
		return Restore(document, false, admit);
	}
	bool ImageGraphHistory::Redo(Document &document, const SourceAdmission &admit) {
		return Restore(document, true, admit);
	}
	bool ImageGraphHistory::Restore(Document &document, bool redo, const SourceAdmission &admit) try {
		auto &source = redo ? RedoSnapshots : UndoSnapshots;
		auto &destination = redo ? UndoSnapshots : RedoSnapshots;
		if (source.empty()) return false;
		Snapshot current{engine::imagegraph::Write(document), CurrentSource, Collections};
		if (current.Text.empty()) return false;
		const auto targetSource = source.back().Source;
		const auto targetCollections = source.back().Collections;
		if (!Fits(
				std::span(source).first(source.size() - 1),
				destination,
				targetSource,
				&current,
				{},
				0,
				targetCollections
			))
			return false;
		Document restored;
		engine::imagegraph::Diagnostic diagnostic;
		if (engine::imagegraph::Read(source.back().Text, restored, diagnostic) !=
			engine::imagegraph::Status::Ok)
			return false;
		destination.reserve(destination.size() + 1);
		if (admit && !admit(document, restored, CurrentSource, targetSource)) return false;
		static_assert(std::is_nothrow_move_assignable_v<Document>);
		static_assert(std::is_nothrow_move_constructible_v<Snapshot>);
		destination.push_back(std::move(current));
		document = std::move(restored);
		CurrentSource = targetSource;
		Collections = targetCollections;
		source.pop_back();
		return true;
	} catch (const std::bad_alloc &) {
		return false;
	}

	ImageGraphHistory::SourceSnapshot ImageGraphHistory::CurrentSourceBytes() const {
		return CurrentSource;
	}
	ImageGraphHistory::CollectionSnapshot ImageGraphHistory::CurrentCollections() const {
		return Collections;
	}
	ImageGraphHistory::CollectionSnapshot ImageGraphHistory::TargetCollections(bool redo) const {
		const auto &source = redo ? RedoSnapshots : UndoSnapshots;
		return source.empty() ? CollectionSnapshot{} : source.back().Collections;
	}
	void ImageGraphHistory::Clear() {
		UndoSnapshots.clear();
		RedoSnapshots.clear();
		CurrentSource.reset();
		Collections.reset();
	}
	bool ImageGraphHistory::CanUndo() const {
		return !UndoSnapshots.empty();
	}
	bool ImageGraphHistory::CanRedo() const {
		return !RedoSnapshots.empty();
	}
}
