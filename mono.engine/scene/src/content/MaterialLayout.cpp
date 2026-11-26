#include <engine/scene/Components.hpp>
#include <engine/scene/content/MaterialLayout.hpp>
#include <engine/scene/content/TileSets.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>

namespace engine::scene {
	namespace {
		std::string_view Trim(std::string_view text) {
			while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
				text.remove_prefix(1);
			while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
				text.remove_suffix(1);
			return text;
		}

		std::string_view Word(std::string_view &text) {
			text = Trim(text);
			size_t end = 0;
			while (end < text.size() &&
				   (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_'))
				++end;
			const std::string_view word = text.substr(0, end);
			text.remove_prefix(end);
			return word;
		}

		bool Number(std::string_view text, float &value) {
			text = Trim(text);
			const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
			return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
				   std::isfinite(value);
		}

		bool Identifier(std::string_view name) {
			return !name.empty() && !std::isdigit(static_cast<unsigned char>(name.front())) &&
				   !name.starts_with("gl_") && name.size() <= 64;
		}
	}

	const MaterialParameter *MaterialLayout::Find(std::string_view name) const {
		const auto found = std::find_if(Parameters.begin(), Parameters.end(), [&](const auto &parameter) {
			return parameter.Name == name;
		});
		return found == Parameters.end() ? nullptr : &*found;
	}

	int MaterialLayout::FindTexture(std::string_view name) const {
		const auto found = std::find(Textures.begin(), Textures.end(), name);
		return found == Textures.end() ? -1 : static_cast<int>(found - Textures.begin());
	}

	MaterialLayout ParseMaterialLayout(std::string_view source) {
		MaterialLayout layout;
		uint32_t next = 0;
		size_t line = 0;
		for (size_t at = 0; at < source.size();) {
			++line;
			size_t end = source.find('\n', at);
			if (end == std::string_view::npos) end = source.size();
			std::string_view text = Trim(source.substr(at, end - at));
			at = end + 1;
			if (text.empty() || text.front() != '#') continue;
			text.remove_prefix(1);
			if (Word(text) != "pragma") continue;
			const std::string_view kind = Word(text);
			if (kind != "parameter" && kind != "texture" && kind != "image") continue;
			const auto fail = [&](std::string_view reason) {
				if (layout.Error.empty()) layout.Error = std::to_string(line) + ": " + std::string(reason);
			};
			const size_t comment = text.find("//");
			if (comment != std::string_view::npos) text = text.substr(0, comment);
			if (kind == "image") {
				const std::string_view name = Word(text);
				if (!Identifier(name) || !Trim(text).empty()) {
					fail("an image is `#pragma image name`");
				} else if (layout.Find(name) != nullptr || layout.FindTexture(name) >= 0 ||
						   std::find(layout.Images.begin(), layout.Images.end(), name) != layout.Images.end()) {
					fail("'" + std::string(name) + "' is declared twice");
				} else if (layout.Images.size() >= MAX_PASS_IMAGES) {
					fail("a pass reads at most four named images");
				} else {
					layout.Images.emplace_back(name);
				}
				continue;
			}
			if (kind == "texture") {
				const std::string_view name = Word(text);
				// `name[N]` is an array of N layers.
				uint32_t layers = 0;
				if (!text.empty() && text.front() == '[') {
					const size_t close = text.find(']');
					const std::string_view count =
						close == std::string_view::npos ? std::string_view{} : text.substr(1, close - 1);
					const auto parsed = std::from_chars(count.data(), count.data() + count.size(), layers);
					if (count.empty() || parsed.ec != std::errc{} || parsed.ptr != count.data() + count.size() ||
						layers < 1 || layers > MAX_TEXTURE_LAYERS) {
						fail("a texture array is `#pragma texture name[N]` with N from 1 to 16");
						continue;
					}
					text.remove_prefix(close + 1);
				}
				if (!Identifier(name) || !Trim(text).empty()) {
					fail("a texture is `#pragma texture name`");
				} else if (layout.Find(name) != nullptr || layout.FindTexture(name) >= 0) {
					fail("'" + std::string(name) + "' is declared twice");
				} else if (layout.Textures.size() >= MAX_MATERIAL_TEXTURES) {
					fail("a material has at most two textures of its own");
				} else {
					layout.Textures.emplace_back(name);
					layout.TextureLayers.push_back(layers);
				}
				continue;
			}
			MaterialParameter parameter;
			const std::string_view type = Word(text);
			parameter.Components = type == "float"	? 1u
								   : type == "vec2" ? 2u
								   : type == "vec3" ? 3u
								   : type == "vec4" ? 4u
													: 0u;
			const std::string_view name = Word(text);
			if (parameter.Components == 0 || !Identifier(name)) {
				fail("a parameter is `#pragma parameter float|vec2|vec3|vec4 name = value`");
				continue;
			}
			if (layout.Find(name) != nullptr || layout.FindTexture(name) >= 0) {
				fail("'" + std::string(name) + "' is declared twice");
				continue;
			}
			parameter.Name = std::string(name);
			text = Trim(text);
			if (!text.empty()) {
				bool good = text.front() == '=';
				if (good) text.remove_prefix(1);
				for (uint32_t index = 0; good && index < parameter.Components; ++index) {
					const size_t comma = text.find(',');
					const bool last = index + 1 == parameter.Components;
					good = last == (comma == std::string_view::npos) &&
						   Number(text.substr(0, comma), parameter.Default[index]);
					if (good && !last) text.remove_prefix(comma + 1);
				}
				if (!good) {
					fail("'" + parameter.Name + "' needs one number per component");
					continue;
				}
			}
			if (next % 4 + parameter.Components > 4) next = (next / 4 + 1) * 4;
			if (next + parameter.Components > MAX_MATERIAL_FLOATS) {
				fail("a material has at most sixteen floats of parameters");
				continue;
			}
			parameter.Offset = next;
			next += parameter.Components;
			layout.Parameters.push_back(std::move(parameter));
		}
		return layout;
	}

	std::string FormatMaterialParameters(const SurfaceAppearance &appearance) {
		std::string out;
		for (size_t index = 0; index < MAX_MATERIAL_FLOATS; ++index) {
			if ((appearance.MaterialParameterMask >> index & 1u) == 0) continue;
			char text[48];
			const auto written =
				std::to_chars(text, text + sizeof(text), appearance.MaterialParameters[index]);
			if (!out.empty()) out += ';';
			out += std::to_string(index) + '=';
			out.append(text, written.ptr);
		}
		return out;
	}

	bool ParseMaterialParameters(std::string_view text, SurfaceAppearance &appearance) {
		float values[MAX_MATERIAL_FLOATS] = {};
		uint16_t mask = 0;
		while (!Trim(text).empty()) {
			const size_t end = std::min(text.find(';'), text.size());
			const std::string_view pair = text.substr(0, end);
			const size_t equals = pair.find('=');
			uint32_t index = 0;
			if (equals == std::string_view::npos) return false;
			const std::string_view number = Trim(pair.substr(0, equals));
			const auto parsed = std::from_chars(number.data(), number.data() + number.size(), index);
			if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() ||
				index >= MAX_MATERIAL_FLOATS || !Number(pair.substr(equals + 1), values[index]))
				return false;
			mask = static_cast<uint16_t>(mask | 1u << index);
			text = end < text.size() ? text.substr(end + 1) : std::string_view{};
		}
		std::copy(std::begin(values), std::end(values), std::begin(appearance.MaterialParameters));
		appearance.MaterialParameterMask = mask;
		return true;
	}

	bool SetMaterialParameter(
		SurfaceAppearance &appearance, const MaterialParameter &parameter, const float *values, size_t count
	) {
		if (count != 0 && count != parameter.Components) return false;
		for (uint32_t index = 0; index < parameter.Components; ++index) {
			const uint32_t slot = parameter.Offset + index;
			appearance.MaterialParameters[slot] = count == 0 ? 0.0f : values[index];
			const uint16_t bit = static_cast<uint16_t>(1u << slot);
			appearance.MaterialParameterMask = static_cast<uint16_t>(
				count == 0 ? appearance.MaterialParameterMask & ~bit : appearance.MaterialParameterMask | bit
			);
		}
		return true;
	}
}
