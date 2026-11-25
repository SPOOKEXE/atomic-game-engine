#include <engine/ecs/Classes.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/content/MaterialLayers.hpp>
#include <engine/scene/content/MaterialLayout.hpp>
#include <engine/scene/content/Shaders.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>

namespace engine::scene {
	namespace {
		constexpr std::array<std::string_view, 5> BLENDS{"Over", "Multiply", "Add", "Screen", "Height"};
		constexpr std::array<std::pair<std::string_view, uint8_t>, 7> CHANNELS{{
			{"Albedo", LAYER_ALBEDO},
			{"Normal", LAYER_NORMAL},
			{"Roughness", LAYER_ROUGHNESS},
			{"Metalness", LAYER_METALNESS},
			{"Occlusion", LAYER_OCCLUSION},
			{"Emissive", LAYER_EMISSIVE},
			{"Custom", LAYER_CUSTOM},
		}};

		std::string Float(float value) {
			char text[48];
			const auto written =
				std::to_chars(text, text + sizeof(text), value, std::chars_format::scientific);
			return std::string(text, written.ptr);
		}

		// The `#pragma parameter` and `#pragma texture` lines of a source.
		std::vector<std::string> Declarations(std::string_view source) {
			std::vector<std::string> lines;
			for (size_t at = 0; at < source.size();) {
				size_t end = source.find('\n', at);
				if (end == std::string_view::npos) end = source.size();
				std::string_view line = source.substr(at, end - at);
				at = end + 1;
				if (line.find("#pragma") == std::string_view::npos) continue;
				if (line.find("parameter") == std::string_view::npos &&
					line.find("texture") == std::string_view::npos)
					continue;
				lines.emplace_back(line);
			}
			return lines;
		}

		// The names a source declares with `#pragma keyword`, at most eight.
		std::vector<std::string> Keywords(std::string_view source) {
			std::vector<std::string> names;
			for (size_t at = 0; at < source.size();) {
				size_t end = source.find('\n', at);
				if (end == std::string_view::npos) end = source.size();
				std::string_view line = source.substr(at, end - at);
				at = end + 1;
				constexpr std::string_view LEAD = "#pragma keyword ";
				if (!line.starts_with(LEAD)) continue;
				line.remove_prefix(LEAD.size());
				while (!line.empty() && (line.back() == ' ' || line.back() == '\r'))
					line.remove_suffix(1);
				const bool word = !line.empty() && std::all_of(line.begin(), line.end(), [](char letter) {
					return std::isalnum(static_cast<unsigned char>(letter)) || letter == '_';
				});
				if (word && names.size() < 8) names.emplace_back(line);
			}
			return names;
		}

		// `layer` over `o`, for a colour-like value.
		std::string Blended(LayerBlend blend, const std::string &from, const std::string &to) {
			switch (blend) {
			case LayerBlend::Multiply:
				return from + " * " + to;
			case LayerBlend::Add:
				return from + " + " + to;
			case LayerBlend::Screen:
				return "1.0 - (1.0 - " + from + ") * (1.0 - " + to + ")";
			case LayerBlend::Height:
			case LayerBlend::Over:
				break;
			}
			return to;
		}
	}

	LayeredSurface
	ComposeLayers(std::string_view base, std::span<const LayerSource> layers, std::string_view keywords) {
		LayeredSurface result;
		std::string defines;
		const std::vector<std::string> switches = Keywords(base);
		while (!keywords.empty()) {
			const size_t end = std::min(keywords.find(','), keywords.size());
			std::string word(keywords.substr(0, end));
			std::erase(word, ' ');
			keywords = end < keywords.size() ? keywords.substr(end + 1) : std::string_view{};
			if (word.empty()) continue;
			// Only what the shader declares, so the permutations are its author's.
			if (std::find(switches.begin(), switches.end(), word) == switches.end()) {
				result.Error = "the shader declares no keyword '" + word + "'";
				return result;
			}
			defines += "#define " + word + " 1\n";
		}
		// The base's declarations come first and in order, so its parameters keep
		// the floats they have without layers.
		std::string head;
		std::vector<std::string> declared = Declarations(base);
		for (const LayerSource &layer : layers) {
			for (std::string &line : Declarations(layer.Code))
				if (std::find(declared.begin(), declared.end(), line) == declared.end())
					declared.push_back(std::move(line));
		}
		for (const std::string &line : declared)
			head += line + "\n";
		MaterialLayout layout = ParseMaterialLayout(head);
		if (!layout.Error.empty()) {
			result.Error = "layers: " + layout.Error;
			return result;
		}
		for (size_t index = 0; index < layers.size(); ++index) {
			const std::string name = "engineLayerOpacity" + std::to_string(index);
			const std::string line = "#pragma parameter float " + name + " = 1\n";
			if (ParseMaterialLayout(head + line).Error.empty()) {
				head += line;
				result.Opacities.push_back(name);
			} else {
				result.Opacities.emplace_back();
			}
		}
		layout = ParseMaterialLayout(head);

		// The base without its declaration lines, which `head` holds.
		std::string body;
		if (base.empty()) {
			body =
				"void EngineBaseSurface(SurfaceInput i, inout SurfaceOutput o) { DefaultSurface(i, o); }\n";
		} else {
			body = "#define Surface EngineBaseSurface\n";
			for (size_t at = 0; at < base.size();) {
				size_t end = base.find('\n', at);
				if (end == std::string_view::npos) end = base.size();
				const std::string line(base.substr(at, end - at));
				at = end + 1;
				const bool moved = !Declarations(line).empty();
				body += (moved ? std::string{} : line) + "\n";
			}
			body += "#undef Surface\n";
		}

		std::string blend =
			"void Surface(SurfaceInput i, inout SurfaceOutput o) {\n\tEngineBaseSurface(i, o);\n";
		std::vector<std::string> modules;
		for (size_t index = 0; index < layers.size(); ++index) {
			const LayerSource &layer = layers[index];
			auto found = std::find(modules.begin(), modules.end(), layer.Module);
			const size_t module = static_cast<size_t>(found - modules.begin());
			const std::string function = "EngineLayer" + std::to_string(module);
			if (found == modules.end()) {
				modules.push_back(layer.Module);
				body += "#define Layer " + function + "\n#include \"" + layer.Module + "\"\n#undef Layer\n";
			}
			std::string weight =
				result.Opacities[index].empty() ? Float(layer.Opacity) : result.Opacities[index];
			if (!layer.Mask.empty()) {
				const size_t where = std::min(layer.Mask.find('@'), layer.Mask.size());
				const std::string named = layer.Mask.substr(0, where);
				const std::string source = where < layer.Mask.size() ? layer.Mask.substr(where + 1) : "uv";
				std::string at = "i.UV";
				if (source == "mesh") {
					at = "i.MeshUV";
				} else if (source.size() == 10 && source.starts_with("attribute") && source[9] >= '0' &&
						   source[9] <= '3') {
					at = std::string("i.Attributes[") + source[9] + "].xy";
				} else if (source != "uv") {
					result.Error = "layer '" + layer.Module + "': no coordinates called '" + source + "'";
					return result;
				}
				const size_t dot = named.find('.');
				const std::string texture = named.substr(0, dot);
				const std::string channel = dot == std::string::npos ? "r" : named.substr(dot + 1);
				if (layout.FindTexture(texture) < 0 || channel.size() != 1 ||
					std::string_view("rgba").find(channel) == std::string_view::npos) {
					result.Error = "layer '" + layer.Module + "': the mask '" + layer.Mask +
								   "' is not a declared texture and one of r, g, b or a";
					return result;
				}
				// An unbound mask hides its layer rather than covering everything.
				weight += " * (" + texture + "Bound() ? texture(" + texture + ", " + at + ")." + channel +
						  " : 0.0)";
			}
			blend += "\t{\n\t\tSurfaceOutput l = o;\n\t\t" + function + "(i, l);\n\t\tfloat w = clamp(" +
					 weight + ", 0.0, 1.0);\n";
			if (layer.Blend == LayerBlend::Height)
				blend += "\t\tw *= smoothstep(-0.1, 0.1, l.Height - o.Height + (w - 0.5) * 2.0);\n";
			const auto mix = [&](std::string_view field, uint8_t channel, bool colour) {
				if ((layer.Channels & channel) == 0) return;
				const std::string o = "o." + std::string(field), l = "l." + std::string(field);
				blend +=
					"\t\t" + o + " = mix(" + o + ", " + (colour ? Blended(layer.Blend, o, l) : l) + ", w);\n";
			};
			mix("Albedo", LAYER_ALBEDO, true);
			mix("Emissive", LAYER_EMISSIVE, true);
			mix("Roughness", LAYER_ROUGHNESS, false);
			mix("Metalness", LAYER_METALNESS, false);
			mix("Occlusion", LAYER_OCCLUSION, false);
			blend += "\t\to.Height = mix(o.Height, l.Height, w);\n";
			if ((layer.Channels & LAYER_NORMAL) != 0)
				blend += "\t\to.Normal = normalize(mix(o.Normal, l.Normal, w));\n";
			if ((layer.Channels & LAYER_CUSTOM) != 0)
				blend += "\t\tfor (int c = 0; c < 4; c++) o.Custom[c] = mix(o.Custom[c], l.Custom[c], w);\n";
			blend += "\t}\n";
		}
		blend += "}\n";
		// With no layers the base is the surface.
		if (layers.empty())
			result.Source = defines + std::string(base);
		else
			result.Source = head + defines + body + blend;
		return result;
	}

	namespace {
		ecs::PropertyDescriptor LayerBlendProperty() {
			ecs::PropertyDescriptor property;
			property.Name = core::Name("Blend");
			property.Type = ecs::PropertyType::Enum;
			property.EnumName = core::Name("LayerBlend");
			property.Size = sizeof(core::Name);
			property.Kind = ecs::PropertyKind::Computed;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<MaterialLayer>()});
			property.Writes = property.Reads;
			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const auto *layer = store.Get<MaterialLayer>(instance);
				if (layer == nullptr) return false;
				*static_cast<core::Name *>(out) = core::Name(BLENDS[static_cast<size_t>(layer->Blend)]);
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				auto *layer = store.GetMutable<MaterialLayer>(instance);
				const core::Name named = *static_cast<const core::Name *>(value);
				for (size_t index = 0; layer != nullptr && index < BLENDS.size(); ++index) {
					if (named != core::Name(BLENDS[index])) continue;
					layer->Blend = static_cast<LayerBlend>(index);
					return true;
				}
				return false;
			};
			return property;
		}

		// `MaterialLayer.Channels`: names joined by commas, such as
		// `Albedo, Roughness`; empty text is every channel.
		ecs::PropertyDescriptor LayerChannelsProperty() {
			ecs::PropertyDescriptor property;
			property.Name = core::Name("Channels");
			property.Type = ecs::PropertyType::String;
			property.Size = sizeof(std::string);
			property.Kind = ecs::PropertyKind::Computed;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<MaterialLayer>()});
			property.Writes = property.Reads;
			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const auto *layer = store.Get<MaterialLayer>(instance);
				if (layer == nullptr) return false;
				std::string text;
				if (layer->Channels != LAYER_ALL) {
					for (const auto &[name, bit] : CHANNELS) {
						if ((layer->Channels & bit) == 0) continue;
						if (!text.empty()) text += ", ";
						text += name;
					}
				}
				*static_cast<std::string *>(out) = std::move(text);
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				auto *layer = store.GetMutable<MaterialLayer>(instance);
				if (layer == nullptr) return false;
				std::string_view text = *static_cast<const std::string *>(value);
				uint8_t channels = 0;
				while (!text.empty()) {
					const size_t end = std::min(text.find(','), text.size());
					std::string_view word = text.substr(0, end);
					while (!word.empty() && word.front() == ' ')
						word.remove_prefix(1);
					while (!word.empty() && word.back() == ' ')
						word.remove_suffix(1);
					text = end < text.size() ? text.substr(end + 1) : std::string_view{};
					if (word.empty()) continue;
					const auto found = std::find_if(CHANNELS.begin(), CHANNELS.end(), [&](const auto &entry) {
						return entry.first == word;
					});
					if (found == CHANNELS.end()) return false;
					channels = static_cast<uint8_t>(channels | found->second);
				}
				layer->Channels = channels == 0 ? LAYER_ALL : channels;
				return true;
			};
			return property;
		}

		ecs::ClassId RegisterMaterialLayerClass() {
			EnsureClassTree();
			ecs::EnumTable::Register("LayerBlend", BLENDS);
			const ecs::ClassId instance = ecs::Classes::Find(core::Name("Instance"));
			const std::array held{ecs::Components::Of<MaterialLayer>()};
			const ecs::ClassId layer = ecs::Classes::Register("MaterialLayer", instance, held);
			ecs::Classes::Property<&MaterialLayer::Shader>(layer, "Shader");
			ecs::Classes::Property<&MaterialLayer::Mask>(layer, "Mask");
			ecs::Classes::ClampedProperty<&MaterialLayer::Opacity, 0.0f, 1.0f>(layer, "Opacity");
			ecs::Classes::Property<&MaterialLayer::Order>(layer, "Order");
			ecs::Classes::Property<&MaterialLayer::Enabled>(layer, "Enabled");
			ecs::Classes::Computed(layer, LayerBlendProperty());
			ecs::Classes::Computed(layer, LayerChannelsProperty());
			for (const std::string_view property :
				 {"Shader", "Mask", "Opacity", "Order", "Enabled", "Blend", "Channels"})
				(void)ecs::Classes::SetPropertiesTag(layer, property, core::Name("Material"));
			return layer;
		}

		// A 128-bit key of generated text.
		std::string TextKey(std::string_view text) {
			std::string key;
			for (const uint64_t seed : {14695981039346656037ull, 1099511628211ull * 31}) {
				uint64_t hash = seed;
				for (const char letter : text) {
					hash ^= static_cast<unsigned char>(letter);
					hash *= 1099511628211ull;
					hash ^= hash >> 29;
				}
				char digits[17];
				const auto written = std::to_chars(digits, digits + sizeof(digits), hash, 16);
				key.append(digits, written.ptr);
				key += '.';
			}
			return key;
		}

		uint64_t Fold(uint64_t signature, uint64_t value) {
			signature ^= value + 0x9E3779B97F4A7C15ull + (signature << 6) + (signature >> 2);
			return signature * 0xBF58476D1CE4E5B9ull;
		}
	}

	namespace {
		void ClearOpacities(
			const LayeredShaders &shaders, const LayeredShaders::Held &held, SurfaceAppearance &appearance
		) {
			const auto found = shaders.Surfaces.find(held.Shader.Id());
			if (!held.Shader.IsValid() || found == shaders.Surfaces.end()) return;
			const MaterialLayout layout = ParseMaterialLayout(found->second.Source);
			for (size_t index = 0; index < held.Opacities.size() && index < held.Written.size(); ++index) {
				const MaterialParameter *parameter = layout.Find(held.Opacities[index]);
				if (parameter != nullptr &&
					appearance.MaterialParameters[parameter->Offset] == held.Written[index])
					SetMaterialParameter(appearance, *parameter, nullptr, 0);
			}
		}
	}

	ecs::ClassId MaterialLayerClass() {
		static const ecs::ClassId layer = RegisterMaterialLayerClass();
		return layer;
	}

	void ResolveMaterialLayers(ecs::Store &store) {
		struct Row {
			ecs::Entity Entity;
			MaterialLayer Layer;
		};
		std::unordered_map<uint64_t, std::vector<Row>> parts;
		store.Each<const MaterialLayer>([&](ecs::Entity entity, const MaterialLayer &layer) {
			if (!layer.Enabled || !layer.Shader.IsValid()) return;
			const ecs::Entity part = store.ParentOf(entity);
			if (part == ecs::NULL_ENTITY || store.Get<SurfaceAppearance>(part) == nullptr) return;
			parts[part.Id].push_back({entity, layer});
		});
		// A material with keywords is a stack of none.
		store.Each<const MaterialRef>([&](ecs::Entity entity, const MaterialRef &material) {
			if (!material.Keywords.IsValid() || material.Keywords.Text().empty() ||
				!material.Shader.IsValid())
				return;
			const ecs::Entity part = store.ParentOf(entity);
			if (part != ecs::NULL_ENTITY && store.Get<SurfaceAppearance>(part) != nullptr)
				(void)parts[part.Id];
		});
		LayeredShaders *held = store.ResourceMutable<LayeredShaders>();
		if (held == nullptr) {
			if (parts.empty()) return;
			store.SetResource(LayeredShaders{});
			held = store.ResourceMutable<LayeredShaders>();
		}
		const auto release = [&](const core::Name &shader) {
			const auto found = held->Surfaces.find(shader.Id());
			if (found != held->Surfaces.end() && --found->second.Uses == 0) held->Surfaces.erase(found);
		};
		// A part whose last layer went draws its base again.
		for (auto entry = held->Parts.begin(); entry != held->Parts.end();) {
			if (parts.contains(entry->first)) {
				++entry;
				continue;
			}
			if (auto *appearance = store.GetMutable<SurfaceAppearance>(ecs::Entity{entry->first})) {
				ClearOpacities(*held, entry->second, *appearance);
				if (appearance->Shader == entry->second.Shader) appearance->Shader = core::Name{};
			}
			release(entry->second.Shader);
			entry = held->Parts.erase(entry);
		}
		for (auto &[id, rows] : parts) {
			const ecs::Entity part{id};
			// Ties go by entity, which is creation order, never by walk order.
			std::sort(rows.begin(), rows.end(), [](const Row &left, const Row &right) {
				return left.Layer.Order != right.Layer.Order ? left.Layer.Order < right.Layer.Order
															 : left.Entity.Id < right.Entity.Id;
			});
			if (rows.size() > MAX_MATERIAL_LAYERS) rows.resize(MAX_MATERIAL_LAYERS);
			core::Name base, keywords;
			store.EachChild(part, [&](ecs::Entity child) {
				const auto *material = store.Get<MaterialRef>(child);
				if (material == nullptr || !material->Shader.IsValid()) return;
				base = material->Shader;
				keywords = material->Keywords;
			});
			const ShaderText baseText = base.IsValid() ? ShaderTextOf(store, base) : ShaderText{};
			uint64_t signature = Fold(0, (uint64_t(base.Id()) << 32) | baseText.Revision);
			signature = Fold(signature, baseText.Source.Id);
			signature = Fold(signature, keywords.Id());
			std::vector<ShaderText> modules;
			for (const Row &row : rows) {
				modules.push_back(ShaderModuleTextOf(store, row.Layer.Shader));
				signature =
					Fold(signature, (uint64_t(row.Layer.Shader.Id()) << 32) | modules.back().Revision);
				signature = Fold(signature, modules.back().Source.Id);
				signature = Fold(
					signature,
					(uint64_t(row.Layer.Mask.Id()) << 32) | uint64_t(row.Layer.Blend) << 8 |
						row.Layer.Channels
				);
			}
			auto &state = held->Parts[id];
			// An opacity with no float left for it is part of the text.
			for (size_t index = 0; index < rows.size() && index < state.Opacities.size(); ++index)
				if (state.Opacities[index].empty())
					signature = Fold(signature, static_cast<uint64_t>(rows[index].Layer.Opacity * 65535.0f));
			const bool known = state.Signature == signature && state.Shader.IsValid();
			if (!known) {
				std::vector<LayerSource> sources;
				for (size_t index = 0; index < rows.size(); ++index) {
					const MaterialLayer &layer = rows[index].Layer;
					sources.push_back(
						{std::string(layer.Shader.Text()),
						 modules[index].Code,
						 std::string(layer.Mask.Text()),
						 layer.Opacity,
						 layer.Blend,
						 layer.Channels}
					);
				}
				LayeredSurface composed = ComposeLayers(
					baseText.Code,
					sources,
					keywords.IsValid() ? std::string_view(keywords.Text()) : std::string_view{}
				);
				if (!composed.Error.empty()) {
					// The compiler reports it in the author's terms, on the script.
					composed.Source = "#error " + composed.Error +
									  "\nvoid Surface(SurfaceInput i, inout SurfaceOutput o) {}\n";
					composed.Opacities.clear();
				}
				const core::Name shader(
					"layers/" + std::string(base.Text()) + "/" + TextKey(composed.Source)
				);
				auto &surface = held->Surfaces[shader.Id()];
				++surface.Uses;
				surface.Source = std::move(composed.Source);
				surface.Reporter = baseText.Found || rows.empty() ? baseText.Source : rows.front().Entity;
				// The old stack's opacities may sit where the new one's values go.
				if (auto *stale = store.GetMutable<SurfaceAppearance>(part))
					ClearOpacities(*held, state, *stale);
				if (state.Shader.IsValid()) release(state.Shader);
				state = {signature, shader, std::move(composed.Opacities), {}};
			}
			SurfaceAppearance *appearance = store.GetMutable<SurfaceAppearance>(part);
			appearance->Shader = state.Shader;
			const auto found = held->Surfaces.find(state.Shader.Id());
			if (found == held->Surfaces.end()) continue;
			const MaterialLayout layout = ParseMaterialLayout(found->second.Source);
			state.Written.assign(state.Opacities.size(), 0.0f);
			for (size_t index = 0; index < rows.size() && index < state.Opacities.size(); ++index) {
				state.Written[index] = rows[index].Layer.Opacity;
				const MaterialParameter *parameter = layout.Find(state.Opacities[index]);
				if (parameter != nullptr)
					SetMaterialParameter(*appearance, *parameter, &rows[index].Layer.Opacity, 1);
			}
		}
	}
}
