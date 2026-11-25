#include "RenderPipelineDocumentAdapter.hpp"

#include <engine/core/Chars.hpp>
#include <engine/graph/PipelineCatalogue.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <studio/RenderPipelineGraph.hpp>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace studio {

	namespace {
		using engine::core::Name;
		using namespace engine::graph;
		namespace ang = engine::ang;
		using render_pipeline::AuthoredNode;
		using render_pipeline::Binding;

		constexpr std::string_view TYPE_PREFIX = "render.pass.";
		constexpr std::string_view INTERNAL_PREFIX = "__render.";

		std::string TypeId(Name kind) {
			return std::string(TYPE_PREFIX) + std::string(kind.Text());
		}

		Name KindOf(std::string_view def) {
			if (!def.starts_with(TYPE_PREFIX)) {
				return {};
			}
			return Name(def.substr(TYPE_PREFIX.size()));
		}

		Name KindOf(const ang::NodeInstance &node) {
			return KindOf(node.Def);
		}

		bool IsImageResource(ResourceKind kind) {
			return kind == ResourceKind::Colour || kind == ResourceKind::Depth ||
				   kind == ResourceKind::Texture || kind == ResourceKind::Storage;
		}

		constexpr const char *PREVIEW_ENABLED = "preview.enabled";
		constexpr const char *PREVIEW_REVERSE = "preview.reverse-spectrum";
		constexpr const char *ORIGINAL_NAME = "__render.authoring.original-name";
		constexpr const char *AUTHORING_COMMENT = "__render.authoring.comment";
		constexpr const char *AUTHORING_MUTED = "__render.authoring.muted";
		constexpr const char *AUTHORING_PREVIEW = "__render.authoring.preview";
		constexpr const char *AUTHORING_PREVIEW_PORT = "__render.authoring.preview-port";

		std::string ResourceType(ResourceKind kind) {
			switch (kind) {
			case ResourceKind::Colour:
			case ResourceKind::Depth:
			case ResourceKind::Texture:
			case ResourceKind::Storage:
				return "render.image";
			case ResourceKind::Buffer:
				return "render.buffer";
			case ResourceKind::Camera:
				return "render.camera";
			case ResourceKind::Entities:
				return "render.entities";
			}
			return std::string(ang::ANY_TYPE);
		}

		std::string Key(std::string_view group, std::string_view name) {
			return std::string(INTERNAL_PREFIX) + std::string(group) + "." + std::string(name);
		}

		using Params = std::map<std::string, ang::Value>;

		std::string TextOf(const ang::NodeInstance &node, const std::string &key) {
			const ang::Value value = RenderPipelineParam(node, key);
			const std::string *text = value.Get<std::string>();
			return text == nullptr ? std::string() : *text;
		}

		bool ToggleOf(const ang::NodeInstance &node, const char *key, bool fallback) {
			const ang::Value value = RenderPipelineParam(node, key);
			const bool *flag = value.Get<bool>();
			return flag == nullptr ? fallback : *flag;
		}

		std::string SelectOf(const ang::NodeInstance &node, const char *key, std::string fallback) {
			std::string text = TextOf(node, key);
			return text.empty() ? fallback : text;
		}

		uint32_t NumberOf(const ang::NodeInstance &node, const std::string &key, uint32_t fallback) {
			const ang::Value value = RenderPipelineParam(node, key);
			const double number = value.AsNumber(std::numeric_limits<double>::quiet_NaN());
			if (value.Empty() || !std::isfinite(number)) {
				return fallback;
			}
			return static_cast<uint32_t>(
				std::clamp(number, 1.0, static_cast<double>(std::numeric_limits<uint32_t>::max()))
			);
		}

		std::string ResourceKey(std::string_view setting, std::string_view port) {
			return "resource." + std::string(port) + "." + std::string(setting);
		}

		std::string BindingMetadataKey(bool write, size_t index, std::string_view field) {
			return std::string(INTERNAL_PREFIX) + "binding." + (write ? "write." : "read.") +
				   std::to_string(index) + "." + std::string(field);
		}

		std::string BindingCountKey(bool write) {
			return std::string(INTERNAL_PREFIX) + "binding." + (write ? "write" : "read") + ".count";
		}

		void PutBindings(Params &params, bool write, const std::vector<Binding> &bindings) {
			params[BindingCountKey(write)] = ang::Value(static_cast<int64_t>(bindings.size()));
			for (size_t index = 0; index < bindings.size(); index++) {
				params[BindingMetadataKey(write, index, "port")] = ang::Value(bindings[index].Port);
				params[BindingMetadataKey(write, index, "resource")] =
					ang::Value(std::string(bindings[index].Resource.Text()));
			}
		}

		size_t BindingCount(const ang::NodeInstance &node, bool write) {
			const auto found = node.Params.find(BindingCountKey(write));
			if (found == node.Params.end()) {
				return 0;
			}
			const double count = found->second.AsNumber(0.0);
			return std::isfinite(count) && count > 0.0 ? static_cast<size_t>(count) : 0;
		}

		std::string
		BindingMetadata(const ang::NodeInstance &node, bool write, size_t index, std::string_view field) {
			return TextOf(node, BindingMetadataKey(write, index, field));
		}

		bool HasBinding(const ang::NodeInstance &node, bool write, std::string_view port) {
			for (size_t index = 0; index < BindingCount(node, write); index++) {
				if (BindingMetadata(node, write, index, "port") == port) {
					return true;
				}
			}
			return false;
		}

		std::string
		OriginalBindingResource(const ang::NodeInstance &node, bool write, std::string_view port) {
			std::string resource;
			for (size_t index = 0; index < BindingCount(node, write); index++) {
				if (BindingMetadata(node, write, index, "port") == port) {
					resource = BindingMetadata(node, write, index, "resource");
				}
			}
			return resource;
		}

		std::string ResolutionOf(const Edit &resource) {
			return resource.Width > 0 && resource.Height > 0 ? "fixed"
				   : resource.Divisor >= 8					 ? "eighth"
				   : resource.Divisor >= 4					 ? "quarter"
				   : resource.Divisor >= 2					 ? "half"
															 : "full";
		}

		std::string LifetimeOf(const Edit &resource) {
			return resource.External || resource.Lifetime != ResourceLifetime::Transient ? "external"
																						 : "transient";
		}

		std::string BindingPort(
			const render_pipeline::Binding &binding,
			const NodeKindSpec &kind,
			PortDirection direction,
			size_t index
		) {
			if (!binding.Port.empty()) return binding.Port;
			const auto *port = kind.DeclaredPort(direction, index);
			return port ? port->Id : std::string{};
		}

		const PortSpec *PortNamed(const NodeKindSpec &kind, PortDirection direction, std::string_view name) {
			const auto *declaration = kind.DeclaredPort(direction, name);
			if (!declaration) return nullptr;
			const auto &metadata = direction == PortDirection::Input ? kind.Inputs : kind.Outputs;
			for (const auto &port : metadata)
				if (port.Name.Text() == declaration->Id) return &port;
			return nullptr;
		}

		const ParameterSpec *ParameterNamed(const NodeKindSpec &kind, std::string_view name) {
			for (const ParameterSpec &parameter : kind.Params) {
				if (parameter.Name.Text() == name) {
					return &parameter;
				}
			}
			return nullptr;
		}

		double ParsedNumber(std::string_view text, double fallback) {
			double value = fallback;
			const auto parsed = engine::core::FromChars(text.data(), text.data() + text.size(), value);
			return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(value)
					   ? value
					   : fallback;
		}

		double ParameterNumber(const ParameterSpec &parameter, std::string_view text) {
			double value = ParsedNumber(text, ParsedNumber(parameter.Default, 0.0));
			if (parameter.HasRange) {
				value = std::clamp(value, parameter.Minimum, parameter.Maximum);
			}
			return value;
		}

		std::string
		ParameterNumberText(const ang::NodeInstance &node, const char *key, const ParameterSpec &parameter) {
			const ang::Value held = RenderPipelineParam(node, key);
			const double fallback = ParameterNumber(parameter, parameter.Default);
			const double number = held.AsNumber(std::numeric_limits<double>::quiet_NaN());
			double value = held.Empty() || !std::isfinite(number) ? fallback : number;
			if (parameter.HasRange) {
				value = std::clamp(value, parameter.Minimum, parameter.Maximum);
			}
			std::array<char, 64> written{};
			const auto encoded = std::to_chars(written.data(), written.data() + written.size(), value);
			return encoded.ec == std::errc{} ? std::string(written.data(), encoded.ptr) : parameter.Default;
		}

		Name UniqueNodeName(const ang::NodeInstance &node, Name kind, std::unordered_set<uint32_t> &used) {
			const std::string label = TextOf(node, RENDER_PIPELINE_LABEL);
			const std::string base = label.empty() ? std::string(kind.Text()) : label;
			Name candidate(base);
			uint32_t suffix = 2;
			while (!used.insert(candidate.Id()).second) {
				candidate = Name(base + "." + std::to_string(suffix++));
			}
			return candidate;
		}

		const ang::Link *LinkInto(const ang::Graph &graph, const std::string &node, std::string_view port) {
			for (const ang::Link &link : graph.Links()) {
				if (link.ToNode == node && link.ToPort == port) {
					return &link;
				}
			}
			return nullptr;
		}

		ang::ParamDef Toggle(std::string name, std::string label, bool fallback) {
			return {
				.Name = std::move(name),
				.Type = "bool",
				.Default = ang::Value(fallback),
				.Label = std::move(label),
				.Widget = "toggle"
			};
		}

		ang::ParamDef
		Select(std::string name, std::string label, std::vector<std::string> choices, size_t chosen) {
			ang::Value fallback =
				chosen < choices.size() ? ang::Value(choices[chosen]) : ang::Value(std::string());
			return {
				.Name = std::move(name),
				.Type = "string",
				.Default = std::move(fallback),
				.Label = std::move(label),
				.Widget = "select",
				.Choices = std::move(choices)
			};
		}

		ang::ParamDef Number(std::string name, std::string label, double fallback) {
			return {
				.Name = std::move(name),
				.Type = "number",
				.Default = ang::Value(fallback),
				.Label = std::move(label),
				.Widget = "number"
			};
		}

		ang::ParamDef Text(std::string name, std::string label, std::string fallback) {
			return {
				.Name = std::move(name),
				.Type = "string",
				.Default = ang::Value(std::move(fallback)),
				.Label = std::move(label),
				.Widget = "text"
			};
		}

		void AppendEditedAuthoringMetadata(
			const PipelineDocument &basis,
			const ang::Graph &graph,
			const std::unordered_map<std::string, Name> &names,
			PipelineDocument &document
		) {
			using namespace engine::graph;
			std::unordered_set<uint32_t> basisNodeNames;
			std::unordered_map<uint32_t, std::string> basisComments;
			std::unordered_map<uint32_t, bool> basisMutes;
			for (const Edit &edit : basis.Edits()) {
				if (edit.Kind == EditKind::AddNode) {
					basisNodeNames.insert(edit.Name.Id());
				} else if (edit.Kind == EditKind::Comment) {
					basisComments[edit.Name.Id()] = edit.Value;
				} else if (edit.Kind == EditKind::Mute) {
					basisMutes[edit.Name.Id()] = edit.Enabled;
				}
			}

			std::unordered_map<uint32_t, const ang::NodeInstance *> nodesByOriginalName;
			for (const ang::NodeInstance &node : graph.Nodes()) {
				const std::string original = TextOf(node, ORIGINAL_NAME);
				if (!original.empty()) {
					nodesByOriginalName[Name(original).Id()] = &node;
				}
			}

			const auto groupKey = [](std::string_view group, std::string_view member) {
				return std::string(group) + '\n' + std::string(member);
			};
			std::unordered_map<std::string, size_t> recordedGroupMembers;
			std::unordered_set<uint32_t> originalComments;
			std::unordered_set<uint32_t> originalMutes;
			std::unordered_set<std::string> originalPreviews;
			std::unordered_map<uint32_t, std::vector<std::pair<std::string, std::string>>> writers;
			for (const ang::NodeInstance &node : graph.Nodes()) {
				for (size_t index = 0; index < BindingCount(node, true); index++) {
					const std::string resource = BindingMetadata(node, true, index, "resource");
					const std::string port = BindingMetadata(node, true, index, "port");
					if (!resource.empty()) {
						writers[Name(resource).Id()].push_back({node.Id, port});
					}
				}
			}

			const auto recordGroup = [&](Name title, Name target) {
				Edit edit;
				edit.Kind = EditKind::Group;
				edit.Name = title;
				edit.Target = target;
				document.Record(std::move(edit));
			};
			const auto groupTitle = [](const ang::Group &group) {
				return Name(group.Label.empty() ? group.Id : group.Label);
			};
			for (const Edit &edit : basis.Edits()) {
				switch (edit.Kind) {
				case EditKind::Group: {
					const auto found = nodesByOriginalName.find(edit.Target.Id());
					if (found == nodesByOriginalName.end()) {
						if (!basisNodeNames.contains(edit.Target.Id())) {
							document.Record(edit);
						}
						break;
					}
					const ang::NodeInstance &node = *found->second;
					const ang::Group *group = node.Group.empty() ? nullptr : graph.FindGroup(node.Group);
					if (group != nullptr) {
						recordGroup(groupTitle(*group), names.at(node.Id));
						recordedGroupMembers[groupKey(group->Id, node.Id)]++;
					}
					break;
				}
				case EditKind::Comment: {
					originalComments.insert(edit.Name.Id());
					const auto found = nodesByOriginalName.find(edit.Name.Id());
					if (found == nodesByOriginalName.end()) {
						if (!basisNodeNames.contains(edit.Name.Id())) document.Record(edit);
						break;
					}
					const ang::NodeInstance &node = *found->second;
					if (node.Params.contains(AUTHORING_COMMENT)) {
						Edit updated = edit;
						updated.Name = names.at(node.Id);
						if (TextOf(node, AUTHORING_COMMENT) != basisComments.at(edit.Name.Id())) {
							updated.Value = TextOf(node, AUTHORING_COMMENT);
						}
						document.Record(std::move(updated));
					}
					break;
				}
				case EditKind::Mute: {
					originalMutes.insert(edit.Name.Id());
					const auto found = nodesByOriginalName.find(edit.Name.Id());
					if (found == nodesByOriginalName.end()) {
						if (!basisNodeNames.contains(edit.Name.Id())) document.Record(edit);
						break;
					}
					const ang::NodeInstance &node = *found->second;
					if (node.Params.contains(AUTHORING_MUTED)) {
						Edit updated = edit;
						updated.Name = names.at(node.Id);
						if (ToggleOf(node, AUTHORING_MUTED, edit.Enabled) != basisMutes.at(edit.Name.Id())) {
							updated.Enabled = ToggleOf(node, AUTHORING_MUTED, edit.Enabled);
						}
						document.Record(std::move(updated));
					}
					break;
				}
				case EditKind::Preview: {
					const auto found = writers.find(edit.Target.Id());
					if (found == writers.end() || found->second.empty()) {
						document.Record(edit);
						break;
					}
					const auto &[nodeId, port] = found->second.front();
					const ang::NodeInstance *node = graph.FindNode(nodeId);
					if (node == nullptr) {
						break;
					}
					if (!ToggleOf(*node, AUTHORING_PREVIEW, false)) {
						break;
					}
					std::string resource = TextOf(*node, Key("write", port));
					if (resource.empty()) {
						break;
					}
					Edit updated = edit;
					updated.Target = Name(resource);
					document.Record(std::move(updated));
					originalPreviews.insert(nodeId);
					break;
				}
				default:
					break;
				}
			}

			for (const ang::Group &group : graph.Groups()) {
				for (const ang::NodeInstance &member : graph.Nodes()) {
					if (member.Group != group.Id) {
						continue;
					}
					auto recorded = recordedGroupMembers.find(groupKey(group.Id, member.Id));
					if (recorded != recordedGroupMembers.end() && recorded->second > 0) {
						recorded->second--;
						continue;
					}
					const auto name = names.find(member.Id);
					if (name != names.end()) {
						recordGroup(groupTitle(group), name->second);
					}
				}
			}

			for (const ang::NodeInstance &node : graph.Nodes()) {
				const std::string original = TextOf(node, ORIGINAL_NAME);
				const uint32_t oldName = original.empty() ? 0 : Name(original).Id();
				if (node.Params.contains(AUTHORING_COMMENT) && !originalComments.contains(oldName)) {
					Edit edit;
					edit.Kind = EditKind::Comment;
					edit.Name = names.at(node.Id);
					edit.Value = TextOf(node, AUTHORING_COMMENT);
					document.Record(std::move(edit));
				}
				if (node.Params.contains(AUTHORING_MUTED) && !originalMutes.contains(oldName)) {
					Edit edit;
					edit.Kind = EditKind::Mute;
					edit.Name = names.at(node.Id);
					edit.Enabled = ToggleOf(node, AUTHORING_MUTED, false);
					document.Record(std::move(edit));
				}
				if (ToggleOf(node, AUTHORING_PREVIEW, false) && !originalPreviews.contains(node.Id)) {
					const std::string port = TextOf(node, AUTHORING_PREVIEW_PORT);
					const std::string resource = TextOf(node, Key("write", port));
					if (!resource.empty()) {
						Edit edit;
						edit.Kind = EditKind::Preview;
						edit.Target = Name(resource);
						document.Record(std::move(edit));
					}
				}
			}
		}

		void AppendOrphanNodeEdits(const PipelineDocument &basis, PipelineDocument &document) {
			using namespace engine::graph;
			bool hasOpenNode = false;
			for (const Edit &edit : basis.Edits()) {
				if (edit.Kind == EditKind::AddNode) {
					hasOpenNode = true;
				} else if (edit.Kind == EditKind::AddResource || edit.Kind == EditKind::Enable) {
					hasOpenNode = false;
				} else if (!hasOpenNode && (edit.Kind == EditKind::Set || edit.Kind == EditKind::Reads ||
											edit.Kind == EditKind::Writes)) {
					document.Record(edit);
				}
			}
		}

		ang::Registry BuildRegistry() {
			using namespace engine::graph;
			RegisterRenderNodeKinds();
			ang::Registry types = ang::Registry::WithScalars();
			const auto unpack = [](uint32_t hex) {
				return engine::core::Color3{
					static_cast<float>((hex >> 16) & 0xFF) / 255.0f,
					static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
					static_cast<float>(hex & 0xFF) / 255.0f
				};
			};
			for (const auto &[id, label, tint] : std::array{
					 std::tuple{"render.image", "IMAGE", 0xD07852u},
					 std::tuple{"render.buffer", "Buffer", 0xB39A58u},
					 std::tuple{"render.camera", "Camera", 0x53A7A0u},
					 std::tuple{"render.entities", "Entities", 0x73A856u},
				 }) {
				types.DefineType({.Id = id, .Label = label, .Wire = unpack(tint)});
			}

			for (const NodeKindSpec &spec : NodeCatalogue::All()) {
				ang::NodeDef def;
				def.Id = std::string(TYPE_PREFIX) + spec.Declaration.Id;
				def.Label = spec.Label.empty() ? std::string(spec.Kind.Text()) : spec.Label;
				def.Category = Describe(spec.Category);
				for (const auto &port : spec.Declaration.Ports) {
					ResourceKind kind;
					if (!ResourceKindFromPortType(port.ValueType, kind)) continue;
					const bool input = port.Direction == engine::graphcore::PortDirection::Input;
					// Inputs are optional: an unlinked read names an external resource.
					(input ? def.Inputs : def.Outputs)
						.push_back(
							{.Name = port.Id,
							 .Type = ResourceType(kind),
							 .Dir = input ? ang::Direction::Input : ang::Direction::Output,
							 .Optional = input}
						);
				}
				if (!RenderPipelinePreviewPort(def.Id).empty()) {
					def.Params.push_back(Toggle(PREVIEW_ENABLED, "Preview", true));
					def.Params.push_back(Toggle(PREVIEW_REVERSE, "Reverse spectrum", false));
				}
				def.Params.push_back(Toggle("enabled", "Enabled", true));
				def.Params.push_back(Toggle("profile", "Profile", true));
				def.Params.push_back(Select(
					"scope",
					"Scope",
					{"world", "view", "frame"},
					spec.Scope == NodeScope::World	? 0
					: spec.Scope == NodeScope::View ? 1
													: 2
				));
				def.Params.push_back(
					Select("queue", "Queue", {"auto", "cpu", "graphics", "compute", "transfer"}, 0)
				);
				def.Params.push_back(Select("async", "Async", {"auto", "allow", "serial"}, 0));
				for (const ParameterSpec &parameter : spec.Params) {
					const std::string key(parameter.Name.Text());
					switch (parameter.Widget) {
					case ParameterWidget::Text:
						def.Params.push_back(Text(key, parameter.Label, parameter.Default));
						break;
					case ParameterWidget::Number:
						def.Params.push_back(
							Number(key, parameter.Label, ParameterNumber(parameter, parameter.Default))
						);
						if (parameter.HasRange) {
							def.Params.back().Minimum = parameter.Minimum;
							def.Params.back().Maximum = parameter.Maximum;
						}
						break;
					case ParameterWidget::Toggle:
						def.Params.push_back(Toggle(key, parameter.Label, parameter.Default == "true"));
						break;
					case ParameterWidget::Select: {
						const auto selected =
							std::find(parameter.Options.begin(), parameter.Options.end(), parameter.Default);
						def.Params.push_back(Select(
							key,
							parameter.Label,
							parameter.Options,
							selected == parameter.Options.end()
								? 0
								: static_cast<size_t>(selected - parameter.Options.begin())
						));
						break;
					}
					}
				}
				for (const PortSpec &port : spec.Outputs) {
					const std::string output(port.Name.Text());
					def.Params.push_back(Select(
						ResourceKey("lifetime", output), output + " lifetime", {"transient", "external"}, 0
					));
					def.Params.push_back(Select(
						ResourceKey("resolution", output),
						output + " resolution",
						{"full", "half", "quarter", "eighth", "fixed"},
						0
					));
					def.Params.push_back(Number(ResourceKey("width", output), output + " width", 1920));
					def.Params.push_back(Number(ResourceKey("height", output), output + " height", 1080));
				}
				types.DefineNode(std::move(def));
			}
			return types;
		}
	}

	const ang::Registry &RenderPipelineTypes() {
		static const ang::Registry types = BuildRegistry();
		return types;
	}

	ang::Value RenderPipelineParam(const ang::NodeInstance &node, std::string_view key) {
		if (const auto held = node.Params.find(std::string(key)); held != node.Params.end()) {
			return held->second;
		}
		if (const auto def = RenderPipelineTypes().FindNode(node.Def)) {
			if (const ang::ParamDef *param = def->FindParam(key)) {
				return param->Default;
			}
		}
		return {};
	}

	std::string RenderPipelineLabel(const ang::NodeInstance &node) {
		const std::string label = TextOf(node, RENDER_PIPELINE_LABEL);
		return label.empty() ? std::string(KindOf(node).Text()) : label;
	}

	std::string RenderPipelinePreviewPort(std::string_view def) {
		RegisterRenderNodeKinds();
		const NodeKindSpec *spec = NodeCatalogue::Find(KindOf(def));
		if (spec == nullptr) {
			return {};
		}
		for (const PortSpec &port : spec->Outputs) {
			if (IsImageResource(port.Kind)) {
				return std::string(port.Name.Text());
			}
		}
		return {};
	}

	bool LoadRenderPipelineGraph(const PipelineDocument &document, ang::Graph &graph, std::string &error) {
		using namespace engine::graph;
		const ang::Registry &types = RenderPipelineTypes();
		graph = {};
		error.clear();

		const std::vector<AuthoredNode> authored = render_pipeline::AuthoredNodes(document);
		std::unordered_map<uint32_t, Edit> resourceSettings;
		std::unordered_map<uint32_t, std::string> comments;
		std::unordered_map<uint32_t, bool> muteStates;
		// Each pass belongs to the last group that names it; groups keep first-mention order.
		std::unordered_map<uint32_t, std::string> groupOf;
		std::vector<std::string> groupOrder;
		for (const Edit &edit : document.Edits()) {
			if (edit.Kind == EditKind::AddResource) {
				resourceSettings[edit.Name.Id()] = edit;
			} else if (edit.Kind == EditKind::Comment) {
				comments[edit.Name.Id()] = edit.Value;
			} else if (edit.Kind == EditKind::Mute) {
				muteStates[edit.Name.Id()] = edit.Enabled;
			} else if (edit.Kind == EditKind::Group) {
				const std::string title(edit.Name.Text());
				if (std::find(groupOrder.begin(), groupOrder.end(), title) == groupOrder.end()) {
					groupOrder.push_back(title);
				}
				groupOf[edit.Target.Id()] = title;
			}
		}
		std::unordered_set<std::string> usedGroups;
		for (const AuthoredNode &source : authored) {
			if (const auto group = groupOf.find(source.Name.Id()); group != groupOf.end()) {
				usedGroups.insert(group->second);
			}
		}
		std::vector<ang::Edit> edits;
		for (const std::string &title : groupOrder) {
			if (usedGroups.contains(title)) {
				edits.push_back(ang::edit::AddGroup{{.Id = title, .Label = title}});
			}
		}

		// Writers by resource, with the authored slot that wrote it.
		struct Writer {
			std::string Node;
			std::string Port;
			size_t Index = 0;
		};
		std::unordered_map<uint32_t, std::vector<Writer>> writers;
		std::vector<std::string> ids;
		ids.reserve(authored.size());
		ang::Graph probe;
		for (size_t index = 0; index < authored.size(); index++) {
			const AuthoredNode &source = authored[index];
			const std::string def = TypeId(source.Kind);
			const auto definition = types.FindNode(def);
			if (definition == nullptr) {
				error = "no render node type is registered for " + std::string(source.Kind.Text());
				return false;
			}
			const float x = source.Moved ? source.X : static_cast<float>(index) * 230.0f;
			const float y = source.Moved					   ? source.Y
							: source.Scope == NodeScope::World ? 0.0f
							: source.Scope == NodeScope::View  ? 180.0f
															   : 360.0f;
			const std::string name(source.Name.Text());
			ang::NodeInstance node{
				.Id = probe.FindNode(name) == nullptr && !name.empty() ? name : probe.NewNodeId(name),
				.Def = def,
				.Version = definition->Version,
				.Position = {x, y},
			};
			Params &params = node.Params;
			params[RENDER_PIPELINE_LABEL] = ang::Value(name);
			params[ORIGINAL_NAME] = ang::Value(name);
			params["enabled"] = ang::Value(source.Enabled);
			params["scope"] = ang::Value(std::string(Describe(source.Scope)));
			PutBindings(params, false, source.Reads);
			PutBindings(params, true, source.Writes);
			if (const auto comment = comments.find(source.Name.Id()); comment != comments.end()) {
				params[AUTHORING_COMMENT] = ang::Value(comment->second);
			}
			if (const auto mute = muteStates.find(source.Name.Id()); mute != muteStates.end()) {
				params[AUTHORING_MUTED] = ang::Value(mute->second);
			}
			if (const auto group = groupOf.find(source.Name.Id()); group != groupOf.end()) {
				node.Group = group->second;
			}
			const NodeKindSpec *spec = NodeCatalogue::Find(source.Kind);
			for (const NodeParameter &parameter : source.Parameters) {
				const std::string key(parameter.Key.Text());
				params[Key("original-parameter", key)] = ang::Value(parameter.Value);
				if (key == PREVIEW_ENABLED || key == PREVIEW_REVERSE || key == "profile") {
					params[key] = ang::Value(parameter.Value == "true");
				} else if (key == "queue" || key == "async") {
					params[key] = ang::Value(parameter.Value);
				} else {
					const ParameterSpec *declared = spec == nullptr ? nullptr : ParameterNamed(*spec, key);
					if (declared == nullptr) {
						params[Key("parameter", key)] = ang::Value(parameter.Value);
						continue;
					}
					switch (declared->Widget) {
					case ParameterWidget::Text:
					case ParameterWidget::Select:
						params[key] = ang::Value(parameter.Value);
						break;
					case ParameterWidget::Number:
						params[key] = ang::Value(ParameterNumber(*declared, parameter.Value));
						break;
					case ParameterWidget::Toggle:
						params[key] = ang::Value(parameter.Value == "true");
						break;
					}
				}
			}

			for (size_t slot = 0; spec != nullptr && slot < source.Reads.size(); slot++) {
				const std::string port = BindingPort(source.Reads[slot], *spec, PortDirection::Input, slot);
				params[Key("read", port)] = ang::Value(std::string(source.Reads[slot].Resource.Text()));
			}
			for (size_t slot = 0; spec != nullptr && slot < source.Writes.size(); slot++) {
				const std::string port = BindingPort(source.Writes[slot], *spec, PortDirection::Output, slot);
				params[Key("write", port)] = ang::Value(std::string(source.Writes[slot].Resource.Text()));
				writers[source.Writes[slot].Resource.Id()].push_back({node.Id, port, index});
				const auto found = resourceSettings.find(source.Writes[slot].Resource.Id());
				if (found != resourceSettings.end()) {
					const Edit &resource = found->second;
					params[ResourceKey("lifetime", port)] = ang::Value(LifetimeOf(resource));
					params[ResourceKey("resolution", port)] = ang::Value(ResolutionOf(resource));
					params[ResourceKey("width", port)] =
						ang::Value(static_cast<double>(resource.Width > 0 ? resource.Width : 1920));
					params[ResourceKey("height", port)] =
						ang::Value(static_cast<double>(resource.Height > 0 ? resource.Height : 1080));
				}
			}
			ids.push_back(node.Id);
			probe.Apply(ang::edit::AddNode{node});
			edits.push_back(ang::edit::AddNode{std::move(node)});
		}
		const ang::EditResult placed = graph.Apply(std::span<const ang::Edit>(edits));
		if (!placed.Ok) {
			error = placed.Error;
			graph = {};
			return false;
		}

		const auto setText = [&](const std::string &node, std::string key, std::string value) {
			graph.Apply(ang::edit::SetParam{node, std::move(key), ang::Value(std::move(value))});
		};
		for (const Edit &edit : document.Edits()) {
			if (edit.Kind != EditKind::Preview) {
				continue;
			}
			const auto found = writers.find(edit.Target.Id());
			if (found == writers.end() || found->second.empty()) {
				continue;
			}
			const Writer &writer = found->second.front();
			graph.Apply(ang::edit::SetParam{writer.Node, AUTHORING_PREVIEW, ang::Value(true)});
			setText(writer.Node, AUTHORING_PREVIEW_PORT, writer.Port);
		}

		for (size_t index = 0; index < authored.size(); index++) {
			const NodeKindSpec *spec = NodeCatalogue::Find(authored[index].Kind);
			for (size_t slot = 0; spec != nullptr && slot < authored[index].Reads.size(); slot++) {
				const Binding &read = authored[index].Reads[slot];
				const std::string input = BindingPort(read, *spec, PortDirection::Input, slot);
				setText(ids[index], Key("read-baseline", input), std::string(read.Resource.Text()));
				const auto found = writers.find(read.Resource.Id());
				if (found == writers.end()) {
					setText(ids[index], Key("external", input), "yes");
					continue;
				}

				const Writer *producer = nullptr;
				for (const Writer &candidate : found->second) {
					if (candidate.Index < index) {
						producer = &candidate;
					}
				}
				if (producer == nullptr && found->second.size() == 1) {
					producer = &found->second.front();
				}
				if (producer != nullptr && !producer->Port.empty() && !input.empty()) {
					const ang::Link link{producer->Node, producer->Port, ids[index], input};
					const ang::LinkVerdict verdict = ang::CheckNewLink(graph, types, link);
					if (!verdict.Ok) {
						error =
							"could not restore " + std::string(read.Resource.Text()) + ": " + verdict.Reason;
						graph = {};
						return false;
					}
					graph.Apply(ang::edit::AddLink{link});
					setText(ids[index], Key("connected", input), "yes");
				}
			}
		}
		return true;
	}

	bool SaveRenderPipelineGraph(
		const ang::Graph &graph, const PipelineDocument &basis, PipelineDocument &document, std::string &error
	) {
		using namespace engine::graph;
		document.Clear();
		error.clear();
		RegisterRenderNodeKinds();

		std::unordered_map<uint32_t, Edit> resources;
		std::vector<Name> resourceOrder;
		for (const Edit &edit : basis.Edits()) {
			if (edit.Kind == EditKind::AddResource && !resources.contains(edit.Name.Id())) {
				resources.emplace(edit.Name.Id(), edit);
				resourceOrder.push_back(edit.Name);
			}
		}
		const std::unordered_map<uint32_t, Edit> originalResources = resources;

		std::unordered_map<std::string, Name> names;
		std::unordered_set<uint32_t> usedNames;
		for (const ang::NodeInstance &node : graph.Nodes()) {
			const Name kind = KindOf(node);
			if (!kind.IsValid()) {
				error = "the canvas contains a non-render node";
				return false;
			}
			const std::string originalName = TextOf(node, ORIGINAL_NAME);
			if (!originalName.empty() && TextOf(node, RENDER_PIPELINE_LABEL) == originalName) {
				names[node.Id] = Name(originalName);
				usedNames.insert(names[node.Id].Id());
			}
		}
		for (const ang::NodeInstance &node : graph.Nodes()) {
			if (!names.contains(node.Id)) {
				names[node.Id] = UniqueNodeName(node, KindOf(node), usedNames);
			}
		}

		for (const ang::Link &link : graph.Links()) {
			const ang::NodeInstance *from = graph.FindNode(link.FromNode);
			const ang::NodeInstance *to = graph.FindNode(link.ToNode);
			if (from == nullptr || to == nullptr) {
				error = "a render link names a node that no longer exists";
				return false;
			}
			const NodeKindSpec *fromKind = NodeCatalogue::Find(KindOf(*from));
			const NodeKindSpec *toKind = NodeCatalogue::Find(KindOf(*to));
			const PortSpec *fromPort =
				fromKind == nullptr ? nullptr : PortNamed(*fromKind, PortDirection::Output, link.FromPort);
			const PortSpec *toPort =
				toKind == nullptr ? nullptr : PortNamed(*toKind, PortDirection::Input, link.ToPort);
			if (fromPort == nullptr || toPort == nullptr) {
				error = "a render link names a socket the pass no longer declares";
				return false;
			}
			// Inputs are reads. Raster composites can sample a compute-written
			// colour image without changing its storage descriptor or attachment role.
			const bool computeColourRead =
				fromPort->Kind == ResourceKind::Storage && toPort->Kind == ResourceKind::Colour;
			if (!PortsCompatible(fromPort->Kind, toPort->Kind) && !computeColourRead) {
				error = std::string(WhyIncompatible(fromPort->Kind, toPort->Kind));
				return false;
			}
		}

		const auto ensureResource = [&](Name name, const PortSpec &port, const Edit *basisResource) {
			if (resources.contains(name.Id())) {
				return;
			}
			Edit resource = basisResource == nullptr ? Edit{} : *basisResource;
			resource.Kind = EditKind::AddResource;
			resource.Name = name;
			if (basisResource == nullptr) {
				resource.Resource = port.Kind;
				resource.Format = port.Format;
			}
			resources.emplace(name.Id(), resource);
			resourceOrder.push_back(name);
		};

		for (const ang::NodeInstance &node : graph.Nodes()) {
			const Name kind = KindOf(node);
			const NodeKindSpec *spec = NodeCatalogue::Find(kind);
			if (spec == nullptr) {
				error = "the render catalogue no longer contains " + std::string(kind.Text());
				return false;
			}
			for (const PortSpec &port : spec->Outputs) {
				const std::string output(port.Name.Text());
				const std::string originalResource = OriginalBindingResource(node, true, output);
				const auto original = originalResources.find(Name(originalResource).Id());
				const Edit *baseline = original == originalResources.end() ? nullptr : &original->second;
				const bool isExisting = node.Params.contains(ORIGINAL_NAME);
				const bool hadBinding = HasBinding(node, true, output);
				std::string resource = TextOf(node, Key("write", output));
				const bool outputLinked =
					std::any_of(graph.Links().begin(), graph.Links().end(), [&](const ang::Link &link) {
						return link.FromNode == node.Id && link.FromPort == output;
					});
				if (resource.empty() && (!isExisting || outputLinked)) {
					resource = std::string(names[node.Id].Text()) + "." + output;
				}
				if (resource.empty() || (isExisting && !hadBinding &&
										 TextOf(node, Key("write", output)).empty() && !outputLinked)) {
					continue;
				}
				const Name resourceName(resource);
				ensureResource(resourceName, port, baseline);
				Edit &settings = resources.at(resourceName.Id());
				if (kind == Name("blit")) {
					ResourceFormat selected = ResourceFormat::RGBA16F;
					if (!ParseResourceFormat(SelectOf(node, "format", "RGBA16F"), selected)) {
						error = "blit target format is not recognised";
						return false;
					}
					const ParameterSpec *format = ParameterNamed(*spec, "format");
					const std::string defaultFormat = format == nullptr ? "RGBA16F" : format->Default;
					const std::string originalKey = Key("original-parameter", "format");
					const std::string originalValue =
						node.Params.contains(originalKey) ? TextOf(node, originalKey) : defaultFormat;
					if (!isExisting || SelectOf(node, "format", defaultFormat) != originalValue) {
						settings.Format = selected;
					}
				}
				const std::string lifetime =
					SelectOf(node, ResourceKey("lifetime", output).c_str(), "transient");
				if (baseline == nullptr || lifetime != LifetimeOf(*baseline)) {
					settings.External = lifetime == "external";
					settings.Lifetime =
						settings.External ? ResourceLifetime::External : ResourceLifetime::Transient;
				}
				const std::string resolution =
					SelectOf(node, ResourceKey("resolution", output).c_str(), "full");
				if (baseline == nullptr || resolution != ResolutionOf(*baseline)) {
					if (resolution == "fixed") {
						settings.Width = NumberOf(node, ResourceKey("width", output), 1920);
						settings.Height = NumberOf(node, ResourceKey("height", output), 1080);
					} else {
						settings.Divisor = resolution == "eighth"	 ? 8
										   : resolution == "quarter" ? 4
										   : resolution == "half"	 ? 2
																	 : 1;
						settings.Width = 0;
						settings.Height = 0;
					}
				} else if (resolution == "fixed") {
					const uint32_t width = NumberOf(node, ResourceKey("width", output), baseline->Width);
					const uint32_t height = NumberOf(node, ResourceKey("height", output), baseline->Height);
					if (width != baseline->Width) settings.Width = width;
					if (height != baseline->Height) settings.Height = height;
				}
			}
		}

		for (const Name resource : resourceOrder) {
			document.Record(resources.at(resource.Id()));
		}

		std::vector<Edit> enables;
		std::vector<Edit> moves;
		for (const ang::NodeInstance &node : graph.Nodes()) {
			const Name kind = KindOf(node);
			const NodeKindSpec &spec = *NodeCatalogue::Find(kind);
			const Name name = names.at(node.Id);

			Edit add;
			add.Kind = EditKind::AddNode;
			add.Name = name;
			add.NodeKind = kind;
			const std::string scope = SelectOf(node, "scope", Describe(spec.Scope));
			add.Scope = scope == "world"   ? NodeScope::World
						: scope == "frame" ? NodeScope::Frame
										   : NodeScope::View;
			document.Record(add);

			const auto currentRead = [&](std::string_view port) {
				if (const ang::Link *link = LinkInto(graph, node.Id, port); link != nullptr) {
					const ang::NodeInstance *producer = graph.FindNode(link->FromNode);
					if (producer == nullptr) {
						return std::string();
					}
					std::string resource = TextOf(*producer, Key("write", link->FromPort));
					if (resource.empty()) {
						resource = std::string(names.at(producer->Id).Text()) + "." + link->FromPort;
					}
					return resource;
				}
				if (TextOf(node, Key("external", port)) == "yes") {
					return TextOf(node, Key("read", port));
				}
				if (TextOf(node, Key("connected", port)) != "yes") {
					return TextOf(node, Key("read", port));
				}
				return std::string();
			};
			const auto recordBinding =
				[&](EditKind bindingKind, std::string_view port, std::string_view resource) {
					Edit binding;
					binding.Kind = bindingKind;
					binding.Key = Name(port);
					binding.Target = Name(resource);
					document.Record(std::move(binding));
				};

			const size_t originalReads = BindingCount(node, false);
			for (size_t index = 0; index < originalReads; index++) {
				const std::string port = BindingMetadata(node, false, index, "port");
				const std::string original = BindingMetadata(node, false, index, "resource");
				if (PortNamed(spec, PortDirection::Input, port) == nullptr) {
					recordBinding(EditKind::Reads, port, original);
				} else {
					const std::string current = currentRead(port);
					const std::string baseline = TextOf(node, Key("read-baseline", port));
					recordBinding(EditKind::Reads, port, current == baseline ? original : current);
				}
			}
			for (const PortSpec &port : spec.Inputs) {
				const std::string key(port.Name.Text());
				if (HasBinding(node, false, key)) {
					continue;
				}
				const std::string resource = currentRead(key);
				if (!resource.empty()) {
					recordBinding(EditKind::Reads, key, resource);
				}
			}

			const size_t originalWrites = BindingCount(node, true);
			for (size_t index = 0; index < originalWrites; index++) {
				const std::string port = BindingMetadata(node, true, index, "port");
				const std::string original = BindingMetadata(node, true, index, "resource");
				if (PortNamed(spec, PortDirection::Output, port) == nullptr) {
					recordBinding(EditKind::Writes, port, original);
				} else {
					const std::string current = TextOf(node, Key("write", port));
					const std::string baseline = OriginalBindingResource(node, true, port);
					recordBinding(EditKind::Writes, port, current == baseline ? original : current);
				}
			}
			for (const PortSpec &port : spec.Outputs) {
				const std::string key(port.Name.Text());
				if (HasBinding(node, true, key)) {
					continue;
				}
				std::string resource = TextOf(node, Key("write", key));
				if (resource.empty() && !node.Params.contains(ORIGINAL_NAME)) {
					resource = std::string(name.Text()) + "." + key;
				}
				if (!resource.empty()) {
					recordBinding(EditKind::Writes, key, resource);
				}
			}

			const std::string unknownPrefix = std::string(INTERNAL_PREFIX) + "parameter.";
			for (const auto &[key, value] : node.Params) {
				if (!key.starts_with(unknownPrefix)) {
					continue;
				}
				Edit set;
				set.Kind = EditKind::Set;
				set.Key = Name(std::string_view(key).substr(unknownPrefix.size()));
				set.Value = value.Get<std::string>() != nullptr ? *value.Get<std::string>() : std::string();
				document.Record(set);
			}
			for (const char *key : {"queue", "async"}) {
				Edit set;
				set.Kind = EditKind::Set;
				set.Key = Name(key);
				set.Value = SelectOf(node, key, "auto");
				document.Record(set);
			}
			{
				Edit set;
				set.Kind = EditKind::Set;
				set.Key = Name("profile");
				set.Value = ToggleOf(node, "profile", true) ? "true" : "false";
				document.Record(set);
			}
			if (!RenderPipelinePreviewPort(node.Def).empty()) {
				for (const char *key : {PREVIEW_ENABLED, PREVIEW_REVERSE}) {
					Edit set;
					set.Kind = EditKind::Set;
					set.Key = Name(key);
					set.Value = ToggleOf(node, key, key == PREVIEW_ENABLED) ? "true" : "false";
					document.Record(set);
				}
			}
			for (const ParameterSpec &parameter : spec.Params) {
				const std::string key(parameter.Name.Text());
				Edit set;
				set.Kind = EditKind::Set;
				set.Key = parameter.Name;
				switch (parameter.Widget) {
				case ParameterWidget::Text:
					set.Value = TextOf(node, key);
					break;
				case ParameterWidget::Number:
					set.Value = ParameterNumberText(node, key.c_str(), parameter);
					break;
				case ParameterWidget::Toggle:
					set.Value = ToggleOf(node, key.c_str(), parameter.Default == "true") ? "true" : "false";
					break;
				case ParameterWidget::Select:
					set.Value = SelectOf(node, key.c_str(), parameter.Default);
					break;
				}
				document.Record(set);
			}

			Edit enable;
			enable.Kind = EditKind::Enable;
			enable.Name = name;
			enable.Enabled = ToggleOf(node, "enabled", true);
			enables.push_back(enable);

			Edit move;
			move.Kind = EditKind::Move;
			move.Name = name;
			move.X = node.Position.X;
			move.Y = node.Position.Y;
			moves.push_back(move);
		}

		for (Edit &edit : enables) {
			document.Record(std::move(edit));
		}
		for (Edit &edit : moves) {
			document.Record(std::move(edit));
		}
		AppendEditedAuthoringMetadata(basis, graph, names, document);
		AppendOrphanNodeEdits(basis, document);
		return true;
	}

	void RenderPipelineFeedback(
		const ang::Graph &graph,
		const RenderGraph &built,
		const PipelineProfile &profile,
		const engine::render::Renderer::PipelineRun &run,
		ang::Plan &plan,
		ang::RunReport &report,
		ang::RunTimeline &timeline
	) {
		plan = {};
		report = {};
		const auto nodes = graph.Nodes();
		for (size_t index = 0; index < nodes.size(); index++) {
			ang::PlannedNode planned;
			planned.Id = nodes[index].Id;
			plan.Nodes.push_back(std::move(planned));
			ang::NodeReport entry;
			const NodeId id{static_cast<uint32_t>(index + 1)};
			if (const Node *pass = built.Find(id)) {
				for (const ResourceId written : pass->Writes) {
					for (const ProfileResource &resource : profile.Resources) {
						if (resource.Id == written) {
							entry.Bytes += static_cast<size_t>(resource.Bytes);
						}
					}
				}
			}
			if (const Node *pass = built.Find(id)) {
				for (const auto &visit : run.Nodes) {
					if (visit.Node != pass->Name) continue;
					// Per-view nodes add up; a refusal outranks a pass that ran.
					if (entry.Status != ang::NodeStatus::Failed) entry.Status = visit.Status;
					entry.Milliseconds += visit.Milliseconds;
				}
			}
			for (const ProfilePass &pass : profile.Passes) {
				if (pass.Node == id) {
					const double microseconds = pass.Elapsed > 0.0 ? pass.Elapsed : pass.Wall;
					if (microseconds > 0.0) {
						if (entry.Status == ang::NodeStatus::Idle) entry.Status = ang::NodeStatus::Ran;
						entry.Milliseconds = microseconds / 1000.0;
					}
				}
			}
			report.Nodes.push_back(std::move(entry));
		}
		plan.Reindex();
		report.Milliseconds = run.Milliseconds;
		timeline = ang::RunTimeline(1);
		if (!run.Nodes.empty()) {
			ang::TimelineRun frame{.RunNumber = 1, .Milliseconds = run.Milliseconds};
			for (const auto &visit : run.Nodes) {
				const int64_t at = plan.Find(visit.Node.Text());
				const auto begin = static_cast<int64_t>(visit.BeginMilliseconds * 1.0e6);
				frame.Samples.push_back(
					{.Node = at < 0 ? 0u : static_cast<uint32_t>(at),
					 .BeginNanoseconds = begin,
					 .EndNanoseconds = begin + static_cast<int64_t>(visit.Milliseconds * 1.0e6),
					 .Outcome = visit.Status}
				);
			}
			timeline.Push(std::move(frame));
		}
	}
}
