#include "ImageGraphRegistration.hpp"

#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ImageGraph.hpp>
#include <engine/scene/Part.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace engine::scene {
	namespace {
		bool ImageGraphMutationAllowed(const ecs::Store &store, ecs::Entity instance) {
			return !store.AdoptOnly() ||
				   (ecs::Store::IsPredicted(instance) && ecs::IsClientLocalInstance(store, instance));
		}
		bool Token(std::string_view text) {
			return !text.empty() && text.size() <= 128 && text != "." && text != ".." &&
				   std::all_of(text.begin(), text.end(), [](unsigned char ch) {
					   return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
							  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
				   });
		}
		bool Asset(std::string_view text) {
			if (text.empty() || text.size() > 4096 || text.front() == '/' || !text.ends_with(".aimagegraph"))
				return false;
			for (size_t begin = 0; begin < text.size();) {
				const size_t end = text.find('/', begin);
				const auto segment =
					text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin);
				if (segment.empty() || segment == "." || segment == ".." ||
					std::any_of(segment.begin(), segment.end(), [](unsigned char ch) {
						return ch <= 32 || ch >= 127 || ch == '\\' || ch == ':' || ch == '#' || ch == '?' ||
							   ch == '%';
					}))
					return false;
				if (end == std::string_view::npos) return true;
				begin = end + 1;
			}
			return false;
		}
		bool Unique(const ecs::Store &store, ecs::Entity instance, core::Name key) {
			bool unique = true;
			const auto paired = [&](ecs::Entity other) {
				const auto *instanceProjection = store.Get<ecs::InstanceProjection>(instance);
				if (instanceProjection != nullptr && instanceProjection->Active &&
					instanceProjection->Source == other)
					return true;
				const auto *otherProjection = store.Get<ecs::InstanceProjection>(other);
				return otherProjection != nullptr && otherProjection->Active &&
					   otherProjection->Source == instance;
			};
			const auto check = [&](ecs::Entity other) {
				const auto *graph = store.Get<ImageGraph>(other);
				if (other != instance && !paired(other) && graph != nullptr && graph->InstanceKey == key)
					unique = false;
			};
			store.EachRoot([&](ecs::Entity root) {
				check(root);
				store.EachDescendant(root, check);
			});
			return unique;
		}
		bool InputValid(const ImageGraphInput &input) {
			if (!Token(input.Name.Text())) return false;
			switch (input.Kind) {
			case ImageGraphInputKind::Number:
				return std::isfinite(input.Number);
			case ImageGraphInputKind::Boolean:
			case ImageGraphInputKind::Colour:
				return true;
			case ImageGraphInputKind::String:
				return input.String.size() <= MAXIMUM_IMAGE_GRAPH_STRING_BYTES &&
					   std::none_of(input.String.begin(), input.String.end(), [](unsigned char ch) {
						   return ch < 32;
					   });
			}
			return false;
		}
		bool InputsValid(std::span<const ImageGraphInput> inputs) {
			if (inputs.size() > MAXIMUM_IMAGE_GRAPH_INPUTS) return false;
			size_t bytes = 4;
			std::unordered_set<uint32_t> names;
			for (const auto &input : inputs) {
				if (!InputValid(input) || !names.insert(input.Name.Id()).second) return false;
				bytes += input.Name.Text().size() + 15;
				switch (input.Kind) {
				case ImageGraphInputKind::Number:
					bytes += 8;
					break;
				case ImageGraphInputKind::Boolean:
					bytes += 1;
					break;
				case ImageGraphInputKind::Colour:
					bytes += 4;
					break;
				case ImageGraphInputKind::String:
					bytes += 4 + input.String.size();
					break;
				}
				if (bytes > MAXIMUM_IMAGE_GRAPH_INPUT_BYTES) return false;
			}
			return true;
		}
		ImageGraphInput Canonical(const ImageGraphInput &input) {
			ImageGraphInput value;
			value.Name = input.Name;
			value.Kind = input.Kind;
			switch (input.Kind) {
			case ImageGraphInputKind::Number:
				value.Number = input.Number;
				break;
			case ImageGraphInputKind::Boolean:
				value.Boolean = input.Boolean;
				break;
			case ImageGraphInputKind::Colour:
				value.Colour = input.Colour;
				break;
			case ImageGraphInputKind::String:
				value.String = input.String;
				break;
			}
			return value;
		}
		void Sort(std::vector<ImageGraphInput> &inputs) {
			std::sort(inputs.begin(), inputs.end(), [](const auto &a, const auto &b) {
				return a.Name.Text() < b.Name.Text();
			});
		}
		void WriteInputs(core::ByteWriter &writer, std::span<const ImageGraphInput> inputs) {
			writer.WriteUInt32(static_cast<uint32_t>(inputs.size()));
			for (const auto &input : inputs) {
				writer.WriteName(input.Name);
				constexpr std::array kinds{"number", "boolean", "colour", "string"};
				writer.WriteString(kinds[static_cast<size_t>(input.Kind)]);
				switch (input.Kind) {
				case ImageGraphInputKind::Number:
					writer.WriteDouble(input.Number);
					break;
				case ImageGraphInputKind::Boolean:
					writer.WriteUInt8(input.Boolean ? 1 : 0);
					break;
				case ImageGraphInputKind::Colour:
					for (uint8_t channel : input.Colour)
						writer.WriteUInt8(channel);
					break;
				case ImageGraphInputKind::String:
					writer.WriteString(input.String);
					break;
				}
			}
		}
		bool ReadInputs(core::ByteReader &reader, std::vector<ImageGraphInput> &out) {
			const size_t before = reader.Remaining();
			const uint32_t count = reader.ReadUInt32();
			if (reader.Failed() || count > MAXIMUM_IMAGE_GRAPH_INPUTS) return false;
			std::vector<ImageGraphInput> candidate;
			for (uint32_t index = 0; index < count; ++index) {
				ImageGraphInput input;
				const auto name = reader.ReadString();
				if (!Token(name)) return false;
				input.Name = core::Name(name);
				const auto kind = reader.ReadString();
				if (kind == "number")
					input.Kind = ImageGraphInputKind::Number;
				else if (kind == "boolean")
					input.Kind = ImageGraphInputKind::Boolean;
				else if (kind == "colour")
					input.Kind = ImageGraphInputKind::Colour;
				else if (kind == "string")
					input.Kind = ImageGraphInputKind::String;
				else
					return false;
				switch (input.Kind) {
				case ImageGraphInputKind::Number:
					input.Number = reader.ReadDouble();
					break;
				case ImageGraphInputKind::Boolean: {
					const uint8_t boolean = reader.ReadUInt8();
					if (boolean > 1) return false;
					input.Boolean = boolean != 0;
					break;
				}
				case ImageGraphInputKind::Colour:
					for (auto &channel : input.Colour)
						channel = reader.ReadUInt8();
					break;
				case ImageGraphInputKind::String: {
					const auto text = reader.ReadString();
					if (text.size() > MAXIMUM_IMAGE_GRAPH_STRING_BYTES ||
						before - reader.Remaining() > MAXIMUM_IMAGE_GRAPH_INPUT_BYTES)
						return false;
					input.String = text;
					break;
				}
				default:
					return false;
				}
				if (reader.Failed() || before - reader.Remaining() > MAXIMUM_IMAGE_GRAPH_INPUT_BYTES ||
					!InputValid(input))
					return false;
				candidate.push_back(std::move(input));
			}
			if (!InputsValid(candidate)) return false;
			Sort(candidate);
			out = std::move(candidate);
			return true;
		}
		std::string InputsText(std::span<const ImageGraphInput> inputs) {
			core::ByteWriter writer(0, MAXIMUM_IMAGE_GRAPH_INPUT_BYTES);
			WriteInputs(writer, inputs);
			constexpr char HEX[] = "0123456789abcdef";
			std::string text = "v1:";
			for (std::byte byte : writer.Bytes()) {
				const auto value = std::to_integer<uint8_t>(byte);
				text += HEX[value >> 4];
				text += HEX[value & 15];
			}
			return text;
		}
		bool ParseInputs(std::string_view text, std::vector<ImageGraphInput> &out) {
			if (text.empty()) {
				out.clear();
				return true;
			}
			if (!text.starts_with("v1:") || text.size() > 3 + 2 * MAXIMUM_IMAGE_GRAPH_INPUT_BYTES ||
				(text.size() - 3) % 2 != 0)
				return false;
			const auto digit = [](char ch) -> int {
				if (ch >= '0' && ch <= '9') return ch - '0';
				if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
				return -1;
			};
			std::vector<std::byte> bytes;
			bytes.reserve((text.size() - 3) / 2);
			for (size_t index = 3; index < text.size(); index += 2) {
				const int high = digit(text[index]), low = digit(text[index + 1]);
				if (high < 0 || low < 0) return false;
				bytes.push_back(static_cast<std::byte>(high * 16 + low));
			}
			core::ByteReader reader(bytes);
			return ReadInputs(reader, out) && reader.AtEnd();
		}
		template <core::Name ImageGraph::*Member, bool (*Set)(ecs::Store &, ecs::Entity, core::Name)>
		ecs::PropertyDescriptor NameProperty(const char *name) {
			ecs::PropertyDescriptor property;
			property.Name = core::Name(name);
			property.Type = ecs::PropertyType::Name;
			property.Size = sizeof(core::Name);
			property.Reads = property.Writes =
				&ecs::ComponentSet::Intern({ecs::Components::Of<ImageGraph>()});
			property.Get = [](const ecs::Store &store, ecs::Entity entity, void *out) {
				const auto *graph = store.Get<ImageGraph>(entity);
				if (graph == nullptr) return false;
				*static_cast<core::Name *>(out) = graph->*Member;
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity entity, const void *value) {
				if (!Set(store, entity, *static_cast<const core::Name *>(value))) return false;
				// Reflected assignments mark declared writes; native no-op setters stay quiet.
				(void)store.GetMutable<ImageGraph>(entity);
				return true;
			};
			return property;
		}
	}

	bool ImageGraphTokenValid(std::string_view text) {
		return Token(text);
	}
	bool SetImageGraphInstanceKey(ecs::Store &store, ecs::Entity instance, core::Name key) {
		if (!ImageGraphMutationAllowed(store, instance)) return false;
		const auto *held = store.Get<ImageGraph>(instance);
		if (held == nullptr) return false;
		if (!key.IsValid()) return !held->InstanceKey.IsValid();
		if (!Token(key.Text()) || !Unique(store, instance, key)) return false;
		if (held->InstanceKey == key) return true;
		auto *graph = store.GetMutable<ImageGraph>(instance);
		graph->InstanceKey = key;
		++graph->Revision;
		return true;
	}
	bool SetImageGraphAsset(ecs::Store &store, ecs::Entity instance, core::Name asset) {
		if (!ImageGraphMutationAllowed(store, instance)) return false;
		const auto *held = store.Get<ImageGraph>(instance);
		if (held == nullptr) return false;
		if (!asset.IsValid()) return !held->Graph.IsValid();
		if (!Asset(asset.Text())) return false;
		if (held->Graph == asset) return true;
		auto *graph = store.GetMutable<ImageGraph>(instance);
		graph->Graph = asset;
		++graph->Revision;
		return true;
	}
	bool SetImageGraphOutput(ecs::Store &store, ecs::Entity instance, core::Name output) {
		if (!ImageGraphMutationAllowed(store, instance) || !Token(output.Text())) return false;
		const auto *held = store.Get<ImageGraph>(instance);
		if (held == nullptr) return false;
		if (held->Output == output) return true;
		auto *graph = store.GetMutable<ImageGraph>(instance);
		graph->Output = output;
		++graph->Revision;
		return true;
	}
	bool
	SetImageGraphInputs(ecs::Store &store, ecs::Entity instance, std::span<const ImageGraphInput> inputs) {
		if (!ImageGraphMutationAllowed(store, instance) || !InputsValid(inputs)) return false;
		const auto *held = store.Get<ImageGraph>(instance);
		if (held == nullptr) return false;
		std::vector<ImageGraphInput> candidate;
		for (const auto &input : inputs)
			candidate.push_back(Canonical(input));
		Sort(candidate);
		if (held->Inputs == candidate) return true;
		auto *graph = store.GetMutable<ImageGraph>(instance);
		graph->Inputs = std::move(candidate);
		++graph->Revision;
		return true;
	}
	bool SetImageGraphInput(ecs::Store &store, ecs::Entity instance, const ImageGraphInput &input) {
		if (!ImageGraphMutationAllowed(store, instance) || !InputValid(input)) return false;
		const auto *held = store.Get<ImageGraph>(instance);
		if (held == nullptr || !InputsValid(held->Inputs)) return false;
		const auto normalized = Canonical(input);
		const auto previous = std::find_if(held->Inputs.begin(), held->Inputs.end(), [&](const auto &value) {
			return value.Name == input.Name;
		});
		if (previous != held->Inputs.end() && *previous == normalized) return true;
		auto candidate = held->Inputs;
		const auto found = std::find_if(candidate.begin(), candidate.end(), [&](const auto &value) {
			return value.Name == input.Name;
		});
		if (found == candidate.end())
			candidate.push_back(normalized);
		else
			*found = normalized;
		return SetImageGraphInputs(store, instance, candidate);
	}
	bool ResetImageGraphInput(ecs::Store &store, ecs::Entity instance, core::Name name) {
		if (!ImageGraphMutationAllowed(store, instance) || !Token(name.Text())) return false;
		const auto *held = store.Get<ImageGraph>(instance);
		if (held == nullptr || !InputsValid(held->Inputs)) return false;
		if (std::none_of(held->Inputs.begin(), held->Inputs.end(), [&](const auto &value) {
				return value.Name == name;
			}))
			return true;
		auto candidate = held->Inputs;
		std::erase_if(candidate, [&](const auto &value) { return value.Name == name; });
		return SetImageGraphInputs(store, instance, candidate);
	}
	bool
	GetImageGraphInput(const ecs::Store &store, ecs::Entity instance, core::Name name, ImageGraphInput &out) {
		const auto *graph = store.Get<ImageGraph>(instance);
		if (graph == nullptr) return false;
		const auto found = std::find_if(graph->Inputs.begin(), graph->Inputs.end(), [&](const auto &value) {
			return value.Name == name;
		});
		if (found == graph->Inputs.end() || !InputValid(*found)) return false;
		out = *found;
		return true;
	}
	core::Name ImageGraphContentName(const ecs::Store &store, ecs::Entity instance, core::Name output) {
		const auto *graph = store.Get<ImageGraph>(instance);
		if (graph == nullptr || !Asset(graph->Graph.Text()) || !Token(graph->InstanceKey.Text()) ||
			!Unique(store, instance, graph->InstanceKey))
			return {};
		if (!output.IsValid()) output = graph->Output;
		if (!Token(output.Text())) return {};
		return core::Name(
			"imagegraph-instance://" + std::string(graph->InstanceKey.Text()) + "#" +
			std::string(output.Text())
		);
	}
	ecs::ClassId ImageGraphClass() {
		static const auto klass = [] {
			EnsureClassTree();
			const std::array components{ecs::Components::Of<ImageGraph>()};
			const auto type =
				ecs::Classes::Register("ImageGraph", ecs::Classes::Find(core::Name("Instance")), components);
			ecs::Classes::Computed(
				type, NameProperty<&ImageGraph::InstanceKey, SetImageGraphInstanceKey>("InstanceKey")
			);
			ecs::Classes::Computed(type, NameProperty<&ImageGraph::Graph, SetImageGraphAsset>("Graph"));
			ecs::Classes::Computed(type, NameProperty<&ImageGraph::Output, SetImageGraphOutput>("Output"));
			ecs::PropertyDescriptor inputs;
			inputs.Name = core::Name("Inputs");
			inputs.Type = ecs::PropertyType::String;
			inputs.Size = sizeof(std::string);
			inputs.Scriptable = false;
			inputs.Reads = inputs.Writes = &ecs::ComponentSet::Intern({ecs::Components::Of<ImageGraph>()});
			inputs.Get = [](const ecs::Store &store, ecs::Entity entity, void *out) {
				const auto *graph = store.Get<ImageGraph>(entity);
				if (graph == nullptr || !InputsValid(graph->Inputs)) return false;
				*static_cast<std::string *>(out) = InputsText(graph->Inputs);
				return true;
			};
			inputs.Set = [](ecs::Store &store, ecs::Entity entity, const void *value) {
				std::vector<ImageGraphInput> decoded;
				if (!ParseInputs(*static_cast<const std::string *>(value), decoded) ||
					!SetImageGraphInputs(store, entity, decoded))
					return false;
				(void)store.GetMutable<ImageGraph>(entity);
				return true;
			};
			ecs::Classes::Computed(type, inputs);
			for (const ecs::PropertyDescriptor &property : ecs::Classes::Describe(type).Properties)
				(void)ecs::Classes::SetPropertiesTag(type, property.Spelling, core::Name("Data"));
			(void)ecs::Classes::SetPropertiesTag(type, "Graph", core::Name("Image"));
			(void)ecs::Classes::SetPropertiesTag(type, "Output", core::Name("Rendering"));
			return type;
		}();
		return klass;
	}
	namespace detail {
		void WriteImageGraphs(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *graphs = static_cast<const ImageGraph *>(source);
			for (size_t index = 0; index < count; ++index) {
				if (!InputsValid(graphs[index].Inputs))
					throw std::invalid_argument("invalid image graph inputs cannot be serialized");
			}
			for (size_t index = 0; index < count; ++index) {
				const auto &graph = graphs[index];
				writer.WriteName(graph.InstanceKey);
				writer.WriteName(graph.Graph);
				writer.WriteName(graph.Output);
				WriteInputs(writer, graph.Inputs);
			}
		}
		void ReadImageGraphs(core::ByteReader &reader, void *destination, size_t count) {
			auto *graphs = static_cast<ImageGraph *>(destination);
			for (size_t index = 0; index < count; ++index) {
				ImageGraph graph;
				const auto key = reader.ReadString();
				const auto asset = reader.ReadString();
				const auto output = reader.ReadString();
				if ((!key.empty() && !Token(key)) || (!asset.empty() && !Asset(asset)) || !Token(output) ||
					!ReadInputs(reader, graph.Inputs)) {
					reader.Fail();
					return;
				}
				graph.InstanceKey = core::Name(key);
				graph.Graph = core::Name(asset);
				graph.Output = core::Name(output);
				if (reader.Failed()) return;
				graphs[index] = std::move(graph);
			}
		}
	}
}
