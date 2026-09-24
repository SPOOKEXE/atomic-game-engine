#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <nodegraph/Registry.hpp>
#include <string_view>
#include <studio/ImageGraph.hpp>
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
			using engine::imagegraph::ValueType;
			switch (type) {
			case ValueType::Boolean:
				return "imagegraph.boolean";
			case ValueType::Integer:
				return "imagegraph.integer";
			case ValueType::Scalar:
				return "imagegraph.scalar";
			case ValueType::Text:
				return "imagegraph.text";
			case ValueType::Colour:
				return "imagegraph.colour";
			case ValueType::Vector2:
				return "imagegraph.vector2";
			case ValueType::Image:
				return std::string(IMAGE_PORT_TYPE);
			case ValueType::Array:
				return "imagegraph.array";
			case ValueType::Gradient:
				return "imagegraph.gradient";
			case ValueType::Area:
				return "imagegraph.area";
			case ValueType::Curve:
				return "imagegraph.curve";
			case ValueType::Vector4:
				return "imagegraph.vector4";
			case ValueType::Path2D:
				return "imagegraph.path2d";
			case ValueType::Vector3:
				return "imagegraph.vector3";
			case ValueType::Quaternion:
				return "imagegraph.quaternion";
			case ValueType::Enum:
				return "imagegraph.enum";
			case ValueType::Mesh:
				return "imagegraph.mesh";
			case ValueType::AudioBit:
				return "imagegraph.audiobit";
			}
			return "imagegraph.unknown";
		}

		std::optional<engine::imagegraph::ValueType> ValueTypeFromCanvas(std::string_view type) {
			using engine::imagegraph::ValueType;
			for (const ValueType candidate : {
					 ValueType::Boolean,
					 ValueType::Integer,
					 ValueType::Scalar,
					 ValueType::Text,
					 ValueType::Colour,
					 ValueType::Vector2,
					 ValueType::Image,
					 ValueType::Array,
					 ValueType::Gradient,
					 ValueType::Area,
					 ValueType::Curve,
					 ValueType::Vector4,
					 ValueType::Path2D,
					 ValueType::Vector3,
					 ValueType::Quaternion,
					 ValueType::Enum,
					 ValueType::Mesh,
					 ValueType::AudioBit,
				 }) {
				if (CanvasType(candidate) == type) return candidate;
			}
			return std::nullopt;
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
			}
			return nodegraph::Colour::Hex(0xFFFFFF);
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

		std::vector<AuthoredValue> StarterValues(std::string_view type) {
			const engine::imagegraph::NodeSchema *schema = engine::imagegraph::FindSchema(type);
			if (schema == nullptr) return {};

			std::vector<AuthoredValue> values;
			values.reserve(schema->Properties.size());
			for (const engine::imagegraph::PropertySchema &property : schema->Properties) {
				if (auto value = ImageGraphPropertyDefault(type, property.Id)) {
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
				for (const AuthoredValue &value : node.Values) {
					if (const auto type = TypeOf(value.Data); type && *type >= ValueType::Gradient)
						required = std::max(required, *type >= ValueType::Vector3 ? 6u : 3u);
				}
				for (const engine::imagegraph::DynamicInput &input : node.DynamicInputs) {
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
				if (keyframe.Ease || keyframe.Interpolation == "source") required = 4;
				if (keyframe.SineDriver) required = std::max(required, 6u);
			}
			if (!document.Tracks.empty()) required = std::max(required, 4u);
			if (document.Timeline) required = std::max(required, 5u);
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

	const engine::imagegraph::Image *
	ImageGraphPreviewCache::Find(uint64_t revision, size_t outputIndex, uint64_t tick, double subframe) {
		if (!std::isfinite(subframe) || subframe < 0.0 || subframe >= 1.0) return nullptr;
		const auto found = std::find_if(Entries.begin(), Entries.end(), [&](const Entry &entry) {
			return entry.Revision == revision && entry.OutputIndex == outputIndex && entry.Tick == tick &&
				   entry.Subframe == subframe;
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
		double subframe
	) {
		if (!std::isfinite(subframe) || subframe < 0.0 || subframe >= 1.0) return false;
		const size_t byteLimit = static_cast<size_t>(IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION) *
								 IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION * 4;
		if (image.Width == 0 || image.Height == 0 || image.Width > IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION ||
			image.Height > IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION || image.Pixels.size() > byteLimit ||
			image.Pixels.size() != static_cast<size_t>(image.Width) * image.Height * 4)
			return false;

		auto found = std::find_if(Entries.begin(), Entries.end(), [&](const Entry &entry) {
			return entry.Revision == revision && entry.OutputIndex == outputIndex && entry.Tick == tick &&
				   entry.Subframe == subframe;
		});
		if (found == Entries.end()) {
			if (Entries.size() == IMAGE_COMPOSER_PREVIEW_CACHE_ENTRIES) {
				found = std::min_element(
					Entries.begin(), Entries.end(), [](const Entry &left, const Entry &right) {
						return left.LastUsed < right.LastUsed;
					}
				);
			} else {
				Entries.emplace_back();
				found = Entries.end() - 1;
			}
		}
		found->Revision = revision;
		found->OutputIndex = outputIndex;
		found->Tick = tick;
		found->Subframe = subframe;
		found->LastUsed = ++UseSerial;
		found->Image = image;
		return true;
	}

	void ImageGraphPreviewCache::Clear() {
		Entries.clear();
		UseSerial = 0;
	}

	size_t ImageGraphPreviewCache::HeldBytes() const {
		size_t bytes = 0;
		for (const Entry &entry : Entries)
			bytes += entry.Image.Pixels.size();
		return bytes;
	}

	void ApplyImageGraphTimeline(const Document &document, ImageGraphPlayback &playback) {
		playback.Playing = false;
		playback.Accumulator = 0.0;
		playback.Direction = 1;
		playback.Subframe = 0.0;
		if (document.Timeline) {
			const engine::imagegraph::TimelineSettings &timeline = *document.Timeline;
			playback.TotalFrames = timeline.Frames;
			playback.StartTick = timeline.First;
			playback.EndTick = timeline.Last;
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
		}
		if (!ValidFramesPerSecond(playback.FramesPerSecond)) playback.FramesPerSecond = 30.0;
		const uint64_t maximumFrames = engine::imagegraph::Limits::MaximumTick + 1;
		playback.TotalFrames = std::clamp(playback.TotalFrames, uint64_t{1}, maximumFrames);
		playback.StartTick = std::min(playback.StartTick, playback.TotalFrames - 1);
		playback.EndTick = std::clamp(playback.EndTick, playback.StartTick, playback.TotalFrames - 1);
		playback.CurrentTick = std::clamp(playback.CurrentTick, playback.StartTick, playback.EndTick);
	}

	bool SetImageGraphPlaybackFrame(ImageGraphPlayback &playback, double frame) {
		if (!std::isfinite(frame)) return false;
		const uint64_t maximumFrames = engine::imagegraph::Limits::MaximumTick + 1;
		playback.TotalFrames = std::clamp(playback.TotalFrames, uint64_t{1}, maximumFrames);
		playback.StartTick = std::min(playback.StartTick, playback.TotalFrames - 1);
		playback.EndTick = std::clamp(playback.EndTick, playback.StartTick, playback.TotalFrames - 1);
		frame =
			std::clamp(frame, static_cast<double>(playback.StartTick), static_cast<double>(playback.EndTick));
		double wholeFrame = 0.0;
		const double subframe = std::modf(frame, &wholeFrame);
		const uint64_t tick = static_cast<uint64_t>(wholeFrame);
		const double boundedSubframe = tick == playback.EndTick ? 0.0 : subframe;
		const bool changed = playback.CurrentTick != tick || playback.Subframe != boundedSubframe;
		playback.CurrentTick = tick;
		playback.Subframe = boundedSubframe;
		if (changed) playback.Accumulator = 0.0;
		return changed;
	}

	bool AdvanceImageGraphPlayback(ImageGraphPlayback &playback, double elapsedSeconds) {
		const uint64_t beforeTick = playback.CurrentTick;
		const double beforeSubframe = playback.Subframe;
		if (!ValidFramesPerSecond(playback.FramesPerSecond)) {
			playback.FramesPerSecond = 30.0;
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
		return playback.CurrentTick != beforeTick || playback.Subframe != beforeSubframe;
	}

	void RegisterImageGraphNodeTypes() {
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

		for (const std::string_view id : IMAGE_NODE_TYPES) {
			const engine::imagegraph::NodeSchema *schema = engine::imagegraph::FindSchema(id);
			if (schema == nullptr) continue;

			nodegraph::NodeType type;
			type.Id = std::string(schema->Type);
			type.Title = NodeTitle(schema->Type);
			type.Category = "Image";
			type.Accent = nodegraph::Colour::Hex(0x262626);
			for (const engine::imagegraph::PortSchema &port : schema->Ports) {
				nodegraph::PortSpec socket{std::string(port.Id), CanvasType(port.Type)};
				if (port.Direction == engine::imagegraph::PortDirection::Input) {
					type.Inputs.push_back(std::move(socket));
				} else {
					type.Outputs.push_back(std::move(socket));
				}
			}
			nodegraph::NodeTypes::Register(type);
		}
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
		case ValueType::Mesh:
		case ValueType::AudioBit:
			return std::nullopt;
		}
		return std::nullopt;
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

		std::unordered_map<std::string, const Node *> nodesById;
		nodesById.reserve(document.Nodes.size());
		for (size_t index = 0; index < document.Nodes.size(); index++) {
			const Node &authored = document.Nodes[index];
			if (authored.DynamicInputs.size() > engine::imagegraph::Limits::MaximumDynamicInputsPerNode) {
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
			canvasNode.X = static_cast<float>(authored.Position.X);
			canvasNode.Y = static_cast<float>(authored.Position.Y);
			canvasNode.DynamicInputs.reserve(authored.DynamicInputs.size());
			for (const engine::imagegraph::DynamicInput &input : authored.DynamicInputs) {
				canvasNode.DynamicInputs.push_back({input.Id, CanvasType(input.Type)});
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
			if (canvasNode.DynamicInputs.size() > engine::imagegraph::Limits::MaximumDynamicInputsPerNode) {
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
				authored.Values = StarterValues(canvasNode.Type);
			}
			authored.Id = documentId;
			authored.Type = canvasNode.Type;
			authored.DynamicInputs.clear();
			authored.DynamicInputs.reserve(canvasNode.DynamicInputs.size());
			for (const nodegraph::PortSpec &input : canvasNode.DynamicInputs) {
				const engine::imagegraph::NodeSchema *schema = engine::imagegraph::FindSchema(authored.Type);
				const std::optional<engine::imagegraph::ValueType> valueType =
					ValueTypeFromCanvas(input.Type);
				if (schema == nullptr || !schema->DynamicInputs || !valueType.has_value()) {
					error = "canvas dynamic inputs do not match the node schema";
					return false;
				}
				std::optional<engine::imagegraph::Value> defaultValue;
				if (const Node *old = FindAuthoredNode(basis, documentId)) {
					const auto oldInput = std::find_if(
						old->DynamicInputs.begin(), old->DynamicInputs.end(), [&](const auto &candidate) {
							return candidate.Id == input.Name;
						}
					);
					if (oldInput != old->DynamicInputs.end() && oldInput->Type == *valueType)
						defaultValue = oldInput->Default;
				}
				authored.DynamicInputs.push_back({input.Name, *valueType, std::move(defaultValue)});
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
		engine::imagegraph::Diagnostic &diagnostic
	) {
		using namespace engine::imagegraph;
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
		const auto port =
			std::find_if(schema->Ports.begin(), schema->Ports.end(), [&](const PortSchema &candidate) {
				return candidate.Direction == PortDirection::Output && candidate.Id == output->Port;
			});
		if (port == schema->Ports.end()) {
			diagnostic = {Status::InvalidOutput, output->NodeId, output->Port, "output port is not declared"};
			return diagnostic.Code;
		}
		if (port->Type == ValueType::Image) {
			Image image;
			const Status status = Evaluate(document, plan, std::string(outputId), request, image, diagnostic);
			if (status == Status::Ok) preview = std::move(image);
			return status;
		}
		if (port->Type != ValueType::Scalar) {
			diagnostic = {
				Status::UnsupportedExecution,
				output->NodeId,
				output->Port,
				"Studio preview supports image and scalar outputs"
			};
			return diagnostic.Code;
		}
		EvaluatedValue value;
		const Status status =
			EvaluateValue(document, plan, std::string(outputId), request, value, diagnostic);
		if (status == Status::Ok) preview = std::move(value);
		return status;
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
		if (!valueType || *valueType != declared->Type)
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
			node->DynamicInputs.size() >= engine::imagegraph::Limits::MaximumDynamicInputsPerNode)
			return fail(engine::imagegraph::Status::LimitExceeded, "dynamic input limit reached");
		if (input.Default) {
			const auto valueType = TypeOf(*input.Default);
			if (!valueType || *valueType != input.Type)
				return fail(
					engine::imagegraph::Status::TypeMismatch, "dynamic input default has the wrong type"
				);
			if (const auto *scalar = std::get_if<double>(&*input.Default); scalar && !std::isfinite(*scalar))
				return fail(engine::imagegraph::Status::InvalidValue, "dynamic input default must be finite");
			if (const auto *vector = std::get_if<engine::imagegraph::Vector2>(&*input.Default);
				vector && (!std::isfinite(vector->X) || !std::isfinite(vector->Y)))
				return fail(engine::imagegraph::Status::InvalidValue, "dynamic input default must be finite");
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
		node->DynamicInputs.erase(input);
		std::erase_if(document.Links, [&](const engine::imagegraph::Link &link) {
			return link.ToNode == nodeId && link.ToPort == inputId;
		});
		return true;
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
		engine::imagegraph::Diagnostic &error
	) {
		error = {};
		const auto fail = [&](engine::imagegraph::Status code, std::string message) {
			SetDiagnostic(error, code, nodeId, property, std::move(message));
			return false;
		};
		if (interpolation != "step" && interpolation != "linear" && interpolation != "cubic" &&
			interpolation != "source")
			return fail(engine::imagegraph::Status::InvalidValue, "unsupported interpolation rule");
		const Node *node = FindAuthoredNode(document, nodeId);
		if (node == nullptr) return fail(engine::imagegraph::Status::UnknownNode, "node does not exist");
		const engine::imagegraph::NodeSchema *schema = engine::imagegraph::FindSchema(node->Type);
		if (schema == nullptr) return fail(engine::imagegraph::Status::UnknownNode, "node type is unknown");
		const auto declared =
			std::find_if(schema->Properties.begin(), schema->Properties.end(), [&](const auto &entry) {
				return entry.Id == property;
			});
		if (declared == schema->Properties.end())
			return fail(engine::imagegraph::Status::UnknownPort, "property is not declared");
		const auto value = std::find_if(node->Values.begin(), node->Values.end(), [&](const auto &entry) {
			return entry.Port == property;
		});
		if (value == node->Values.end())
			return fail(engine::imagegraph::Status::InvalidValue, "initialize the property before keying it");
		const auto valueType = TypeOf(value->Data);
		if (!valueType || *valueType != declared->Type)
			return fail(engine::imagegraph::Status::TypeMismatch, "authored value does not match the schema");
		auto frame =
			std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &entry) {
				return entry.NodeId == nodeId && entry.Port == property && entry.Tick == tick;
			});
		if (frame == document.Keyframes.end()) {
			if (document.Keyframes.size() >= engine::imagegraph::Limits::MaximumKeyframes)
				return fail(engine::imagegraph::Status::LimitExceeded, "keyframe limit reached");
		}
		if (interpolation == "source" && !EnsureSourceAnimationTrack(document, nodeId, property, error))
			return false;
		if (frame == document.Keyframes.end()) {
			document.Keyframes.push_back(
				{std::string(nodeId),
				 std::string(property),
				 tick,
				 value->Data,
				 std::string(interpolation),
				 std::nullopt}
			);
		} else {
			frame->Data = value->Data;
		}
		SetTrackInterpolation(document, nodeId, property, interpolation);
		return true;
	}

	bool RemoveImageGraphKeyframe(
		Document &document,
		std::string_view nodeId,
		std::string_view property,
		uint64_t tick,
		engine::imagegraph::Diagnostic &error
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
		if (std::none_of(schema->Properties.begin(), schema->Properties.end(), [&](const auto &entry) {
				return entry.Id == property;
			})) {
			return fail(engine::imagegraph::Status::UnknownPort, "property is not declared");
		}
		const size_t before = document.Keyframes.size();
		std::erase_if(document.Keyframes, [&](const auto &entry) {
			return entry.NodeId == nodeId && entry.Port == property && entry.Tick == tick;
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

	bool SetImageGraphKeyframeInterpolation(
		Document &document, size_t index, std::string_view rule, engine::imagegraph::Diagnostic &error
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
		if (rule == "source" && !EnsureSourceAnimationTrack(document, frame.NodeId, frame.Port, error))
			return false;
		SetTrackInterpolation(document, frame.NodeId, frame.Port, rule);
		return true;
	}

	bool SetImageGraphKeyframeEase(
		Document &document,
		size_t index,
		engine::imagegraph::KeyframeEase ease,
		engine::imagegraph::Diagnostic &error
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
		const std::string nodeId = document.Keyframes[index].NodeId;
		const std::string port = document.Keyframes[index].Port;
		if (!EnsureSourceAnimationTrack(document, nodeId, port, error)) return false;
		SetTrackInterpolation(document, nodeId, port, "source");
		document.Keyframes[index].Ease = std::move(ease);
		return true;
	}

	bool SetImageGraphKeyframeSineDriver(
		Document &document,
		size_t index,
		std::optional<engine::imagegraph::KeyframeSineDriver> driver,
		engine::imagegraph::Diagnostic &error
	) {
		error = {};
		if (index >= document.Keyframes.size()) {
			SetDiagnostic(
				error, engine::imagegraph::Status::InvalidValue, {}, {}, "keyframe index is out of range"
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
			!ValidFramesPerSecond(timeline.FramesPerSecond)) {
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
		engine::imagegraph::Diagnostic &error
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
		if (std::none_of(schema->Properties.begin(), schema->Properties.end(), [&](const auto &entry) {
				return entry.Id == property;
			}))
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
		engine::imagegraph::Diagnostic &error
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
		const auto declared =
			std::find_if(schema->Ports.begin(), schema->Ports.end(), [&](const auto &entry) {
				return entry.Id == port && entry.Direction == engine::imagegraph::PortDirection::Output &&
					   entry.Type == engine::imagegraph::ValueType::Image;
			});
		if (declared == schema->Ports.end())
			return fail(engine::imagegraph::Status::UnknownPort, "image output port is not declared");
		output->NodeId = nodeId;
		output->Port = port;
		return true;
	}

	std::optional<uint64_t> PreviousImageGraphKey(const Document &document, uint64_t tick) {
		std::optional<uint64_t> previous;
		for (const engine::imagegraph::Keyframe &frame : document.Keyframes) {
			if (frame.Tick < tick && (!previous || frame.Tick > *previous)) previous = frame.Tick;
		}
		return previous;
	}

	std::optional<uint64_t> NextImageGraphKey(const Document &document, uint64_t tick) {
		std::optional<uint64_t> next;
		for (const engine::imagegraph::Keyframe &frame : document.Keyframes) {
			if (frame.Tick > tick && (!next || frame.Tick < *next)) next = frame.Tick;
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

	void RegisterPxcxCanvasNodeTypes(const engine::bake::PxcxArchive &archive) {
		constexpr std::string_view OPAQUE_TYPE = "pxcx.opaque";
		nodegraph::DataType dataType;
		dataType.Id = std::string(OPAQUE_TYPE);
		dataType.Label = "Opaque PXCX";
		dataType.Tint = nodegraph::Colour::Hex(0x858585);
		dataType.Description = "Positional archive connection; native execution is unavailable";
		nodegraph::DataTypes::Register(dataType);

		struct SocketIndexes {
			std::map<uint32_t, bool> Inputs;
			std::map<uint32_t, bool> Outputs;
		};
		std::unordered_map<std::string, std::string> labels;
		std::unordered_map<std::string, SocketIndexes> sockets;
		std::unordered_map<std::string, std::string> typeByNode;
		typeByNode.reserve(archive.Nodes.size());
		for (const engine::bake::PxcxNodeFact &fact : archive.Nodes) {
			const std::string nodeType = PxcxOpaqueNodeType(fact.Type);
			labels.try_emplace(nodeType, fact.Type);
			sockets.try_emplace(nodeType);
			typeByNode.emplace(fact.Id, nodeType);
		}
		for (const engine::bake::PxcxLinkFact &link : archive.Links) {
			if (const auto from = typeByNode.find(link.FromNode); from != typeByNode.end())
				sockets[from->second].Outputs.emplace(link.FromIndex, true);
			if (const auto to = typeByNode.find(link.ToNode); to != typeByNode.end())
				sockets[to->second].Inputs.emplace(link.ToInputIndex, true);
		}

		for (const auto &[id, foreignLabel] : labels) {
			nodegraph::NodeType type;
			type.Id = id;
			type.Title = "PXCX " + foreignLabel;
			if (type.Title.size() > 96) {
				type.Title.resize(93);
				type.Title += "...";
			}
			type.Category = "Imported PXCX";
			type.Accent = nodegraph::Colour::Hex(0x585858);
			type.Subtitle = "opaque, no execution";
			const SocketIndexes &indexes = sockets.at(id);
			for (const auto &[index, unused] : indexes.Inputs) {
				(void)unused;
				type.Inputs.push_back({PxcxInputPortId(index), std::string(OPAQUE_TYPE)});
			}
			for (const auto &[index, unused] : indexes.Outputs) {
				(void)unused;
				type.Outputs.push_back({PxcxOutputPortId(index), std::string(OPAQUE_TYPE)});
			}
			nodegraph::NodeTypes::Register(type);
		}
	}

	ImageGraphHistory::ImageGraphHistory(size_t capacity, size_t byteCapacity)
		: Capacity(capacity), ByteCapacity(byteCapacity) {}

	void ImageGraphHistory::Record(
		const engine::imagegraph::Document &before, const engine::imagegraph::Document &after
	) {
		if (before == after) return;
		for (const std::string &entry : RedoText)
			RetainedBytes -= entry.size();
		RedoText.clear();
		if (Capacity == 0 || ByteCapacity == 0) {
			Clear();
			return;
		}
		std::string snapshot = engine::imagegraph::Write(before);
		if (snapshot.size() > ByteCapacity) {
			Clear();
			return;
		}
		RetainedBytes += snapshot.size();
		UndoText.push_back(std::move(snapshot));
		while (UndoText.size() + RedoText.size() > Capacity || RetainedBytes > ByteCapacity) {
			RetainedBytes -= UndoText.front().size();
			UndoText.erase(UndoText.begin());
		}
	}

	bool ImageGraphHistory::Undo(engine::imagegraph::Document &document) {
		if (UndoText.empty()) return false;
		std::string currentText = engine::imagegraph::Write(document);
		const size_t replacedBytes = UndoText.back().size();
		if (currentText.size() > ByteCapacity - (RetainedBytes - replacedBytes)) return false;
		engine::imagegraph::Document restored;
		engine::imagegraph::Diagnostic diagnostic;
		if (engine::imagegraph::Read(UndoText.back(), restored, diagnostic) != engine::imagegraph::Status::Ok)
			return false;
		RetainedBytes -= replacedBytes;
		RetainedBytes += currentText.size();
		RedoText.push_back(std::move(currentText));
		document = std::move(restored);
		UndoText.pop_back();
		return true;
	}

	bool ImageGraphHistory::Redo(engine::imagegraph::Document &document) {
		if (RedoText.empty()) return false;
		std::string currentText = engine::imagegraph::Write(document);
		const size_t replacedBytes = RedoText.back().size();
		if (currentText.size() > ByteCapacity - (RetainedBytes - replacedBytes)) return false;
		engine::imagegraph::Document restored;
		engine::imagegraph::Diagnostic diagnostic;
		if (engine::imagegraph::Read(RedoText.back(), restored, diagnostic) != engine::imagegraph::Status::Ok)
			return false;
		RetainedBytes -= replacedBytes;
		RetainedBytes += currentText.size();
		UndoText.push_back(std::move(currentText));
		document = std::move(restored);
		RedoText.pop_back();
		return true;
	}

	void ImageGraphHistory::Clear() {
		UndoText.clear();
		RedoText.clear();
		RetainedBytes = 0;
	}

	bool ImageGraphHistory::CanUndo() const {
		return !UndoText.empty();
	}

	bool ImageGraphHistory::CanRedo() const {
		return !RedoText.empty();
	}
}
