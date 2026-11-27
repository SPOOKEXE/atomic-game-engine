#include "resources/EngineShaderSources.hpp"

#include <engine/render/SceneLight.hpp>
#include <engine/render/resources/ShaderAssembly.hpp>
#include <engine/scene/content/MaterialLayout.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <unordered_set>
#include <vector>

namespace engine::render {
	namespace {
		// Text with comments blanked, newlines kept, so declarations are found in code only.
		std::string WithoutComments(std::string_view source) {
			std::string out(source);
			for (size_t at = 0; at + 1 < out.size();) {
				if (out[at] == '/' && out[at + 1] == '/') {
					while (at < out.size() && out[at] != '\n')
						out[at++] = ' ';
				} else if (out[at] == '/' && out[at + 1] == '*') {
					const size_t close = out.find("*/", at + 2);
					const size_t end = close == std::string::npos ? out.size() : close + 2;
					for (; at < end; ++at)
						if (out[at] != '\n') out[at] = ' ';
				} else {
					++at;
				}
			}
			return out;
		}

		bool IsWord(char letter) {
			return std::isalnum(static_cast<unsigned char>(letter)) || letter == '_';
		}

		// Whether `type name (` is declared in comment-free code.
		bool Declares(std::string_view code, std::string_view type, std::string_view name) {
			for (size_t at = code.find(type); at != std::string_view::npos; at = code.find(type, at + 1)) {
				if (at > 0 && IsWord(code[at - 1])) continue;
				size_t next = at + type.size();
				if (next >= code.size() || !std::isspace(static_cast<unsigned char>(code[next]))) continue;
				while (next < code.size() && std::isspace(static_cast<unsigned char>(code[next])))
					++next;
				if (code.substr(next, name.size()) != name) continue;
				next += name.size();
				while (next < code.size() && std::isspace(static_cast<unsigned char>(code[next])))
					++next;
				if (next < code.size() && code[next] == '(') return true;
			}
			return false;
		}

		std::string_view Trim(std::string_view text) {
			while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
				text.remove_prefix(1);
			while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
				text.remove_suffix(1);
			return text;
		}

		// The whole `type name(...) { ... }` definition in `text`, with a `#line`
		// naming where it came from, or empty when it is not defined there.
		std::string FunctionText(std::string_view text, std::string_view type, std::string_view name) {
			const std::string code = WithoutComments(text);
			for (size_t at = code.find(type); at != std::string::npos; at = code.find(type, at + 1)) {
				if (at > 0 && IsWord(code[at - 1])) continue;
				if (!Declares(std::string_view(code).substr(at), type, name)) break;
				size_t next = at + type.size();
				while (next < code.size() && std::isspace(static_cast<unsigned char>(code[next])))
					++next;
				if (code.compare(next, name.size(), name) != 0 ||
					(next + name.size() < code.size() && IsWord(code[next + name.size()])))
					continue;
				const size_t open = code.find_first_of("{;", next);
				if (open == std::string::npos || code[open] == ';') continue;
				size_t depth = 0, close = open;
				for (; close < code.size(); ++close) {
					if (code[close] == '{') ++depth;
					if (code[close] == '}' && --depth == 0) break;
				}
				if (close >= code.size()) return {};
				// The nearest `#line` before it says which file and line it is.
				std::string where = "#line 1 \"shader\"\n";
				size_t line = 0;
				for (size_t mark = code.rfind("#line", at); mark != std::string::npos;) {
					const size_t end = code.find('\n', mark);
					std::string_view directive = std::string_view(code).substr(mark + 5, end - mark - 5);
					directive = Trim(directive);
					const size_t space = directive.find(' ');
					uint32_t number = 1;
					std::from_chars(
						directive.data(), directive.data() + std::min(space, directive.size()), number
					);
					const std::string file(
						space == std::string_view::npos ? "" : Trim(directive.substr(space))
					);
					line = number;
					for (size_t walk = end + 1; walk < at; ++walk)
						if (code[walk] == '\n') ++line;
					where =
						"#line " + std::to_string(line) + " " + (file.empty() ? "\"shader\"" : file) + "\n";
					break;
				}
				if (line == 0) {
					line = 1;
					for (size_t walk = 0; walk < at; ++walk)
						if (code[walk] == '\n') ++line;
					where = "#line " + std::to_string(line) + " \"shader\"\n";
				}
				return where + std::string(text.substr(at, close + 1 - at)) + "\n";
			}
			return {};
		}

		// An engine vertex stage without its `#version` and `#extension` lines,
		// which the wrapper states first.
		std::string VertexBody(std::string_view stage) {
			std::string out;
			for (size_t at = 0; at < stage.size();) {
				size_t end = stage.find('\n', at);
				if (end == std::string_view::npos) end = stage.size();
				const std::string_view line = Trim(stage.substr(at, end - at));
				if (!line.starts_with("#version") && !line.starts_with("#extension")) {
					out += stage.substr(at, end - at);
					out += '\n';
				}
				at = end + 1;
			}
			return out;
		}

		struct Expansion {
			const ShaderModuleResolver &Modules;
			bool Surface = false;
			std::string Out{};
			std::string Error{};
			// Each module is expanded once, as its own include guard.
			std::unordered_set<std::string> Seen{};
			std::vector<std::string> Stack{};

			bool Fail(std::string message) {
				if (Error.empty()) Error = std::move(message);
				return false;
			}

			// Copies `source` with its includes expanded. `#version` is dropped in a
			// surface, whose wrapper states it.
			bool Append(std::string_view file, std::string_view source) {
				if (Stack.size() >= MAX_SHADER_INCLUDE_DEPTH)
					return Fail(std::string(file) + ": includes are nested too deeply");
				Stack.emplace_back(file);
				const std::string code = WithoutComments(source);
				if (!Surface && Stack.size() == 1) {
					// `#version` must stay the first line of a whole program.
				} else {
					Out += "#line 1 \"" + std::string(file) + "\"\n";
				}
				size_t line = 1;
				for (size_t at = 0; at < source.size(); ++line) {
					size_t end = source.find('\n', at);
					if (end == std::string_view::npos) end = source.size();
					const std::string_view bare = Trim(std::string_view(code).substr(at, end - at));
					std::string_view directive = bare;
					bool include = false, version = false;
					if (!directive.empty() && directive.front() == '#') {
						directive = Trim(directive.substr(1));
						include = directive.starts_with("include");
						version = directive.starts_with("version");
					}
					if (include) {
						const std::string_view target = Trim(directive.substr(7));
						const std::string where = std::string(file) + ":" + std::to_string(line) + ": ";
						if (target.size() < 3 || target.front() != '"' || target.back() != '"')
							return Fail(where + "#include takes a quoted module name");
						const std::string module(target.substr(1, target.size() - 2));
						if (std::find(Stack.begin(), Stack.end(), module) != Stack.end())
							return Fail(where + "'" + module + "' includes itself");
						if (Seen.insert(module).second) {
							if (Seen.size() > MAX_SHADER_INCLUDES) return Fail(where + "too many includes");
							if (const auto engine = EngineShaderModule(module)) {
								// A surface already holds every engine module.
								if (!Surface) {
									return Fail(
										where + "'" + module +
										"' needs a Surface function, not a whole program"
									);
								}
							} else if (const auto text = Modules ? Modules(module) : std::nullopt) {
								if (!Append(module, *text)) return false;
							} else {
								return Fail(where + "no ShaderModule named '" + module + "'");
							}
						}
						Out += "#line " + std::to_string(line + 1) + " \"" + std::string(file) + "\"\n";
					} else if (version && (Surface || Stack.size() > 1)) {
						Out += '\n';
					} else {
						Out += source.substr(at, end - at);
						Out += '\n';
					}
					if (Out.size() > MAX_SHADER_AUTHOR_BYTES)
						return Fail(std::string(file) + ": the shader is too large");
					at = end + 1;
				}
				Stack.pop_back();
				return true;
			}
		};
	}

	std::optional<std::string_view> EngineShaderModule(std::string_view name) {
		if (name == "engine/surface") return ENGINE_SURFACE_GLSL;
		if (name == "engine/brdf") return ENGINE_BRDF_GLSL;
		if (name == "engine/pass") return ENGINE_PASS_GLSL;
		return std::nullopt;
	}

	ShaderAssembly
	AssembleShader(std::string_view name, std::string_view source, const ShaderModuleResolver &modules) {
		ShaderAssembly result;
		const std::string code = WithoutComments(source);
		const bool main = Declares(code, "void", "main");
		result.Surface = !main && Declares(code, "void", "Surface");
		result.Compute = !main && !result.Surface && Declares(code, "void", "Compute");
		result.Pass = !main && !result.Surface && (result.Compute || Declares(code, "vec4", "Pass"));
		result.Lit = result.Surface && Declares(code, "vec3", "Light");
		Expansion expansion{.Modules = modules, .Surface = result.Surface || result.Pass};
		const std::string label = name.empty() ? std::string("shader") : std::string(name);
		if (!expansion.Append(label, source)) {
			result.Error = std::move(expansion.Error);
			return result;
		}
		// A Light can live in a module the script includes.
		if (result.Surface && !result.Lit)
			result.Lit = Declares(WithoutComments(expansion.Out), "vec3", "Light");
		if (!result.Surface && !result.Pass) {
			if (expansion.Seen.empty()) {
				result.Forward = std::string(source);
			} else {
				// Named #line directives need the extension, after `#version`.
				const std::string &text = expansion.Out;
				const size_t version = text.find("#version");
				const size_t after = version == std::string::npos ? 0 : text.find('\n', version) + 1;
				result.Forward = text.substr(0, after) +
								 "#extension GL_GOOGLE_cpp_style_line_directive : require\n#line 2 \"" +
								 label + "\"\n" + text.substr(after);
			}
			return result;
		}
		// Declared names become globals read from the part's values. Only the
		// script's own declarations count, which is what `SetMaterialParameter` reads.
		const scene::MaterialLayout layout = scene::ParseMaterialLayout(source);
		if (!layout.Error.empty()) {
			result.Error = label + ":" + layout.Error;
			return result;
		}
		if (result.Pass && !layout.Textures.empty()) {
			result.Error = label + ": a pass has no textures of its own";
			return result;
		}
		if (!result.Pass && !layout.Images.empty()) {
			result.Error = label + ": only a pass reads named images";
			return result;
		}
		result.Images = layout.Images;
		std::string declared;
		for (size_t slot = 0; slot < layout.Textures.size(); ++slot) {
			const std::string &texture = layout.Textures[slot];
			const uint32_t layers = slot < layout.TextureLayers.size() ? layout.TextureLayers[slot] : 0;
			declared += "layout(set = 2, binding = " + std::to_string(11 + slot) + ") uniform " +
						(layers > 0 ? "sampler2DArray " : "sampler2D ") + texture + ";\n";
			// An array is read as `texture(name, vec3(uv, layer))`.
			if (layers > 0)
				declared += "int " + texture + "Layers() { return " + std::to_string(layers) + "; }\n";
			declared += "bool " + texture + "Bound() { return lighting.ParameterState." +
						(slot == 0 ? "y" : "z") + " > 0.5; }\n";
		}
		for (size_t slot = 0; slot < layout.Images.size(); ++slot) {
			const std::string scale =
				std::string("lighting.ImageScale") + (slot < 2 ? "0." : "1.") + (slot % 2 == 0 ? "xy" : "zw");
			// An image no pass drew this frame reads as zero.
			declared += "vec4 " + layout.Images[slot] + "(vec2 uv) { return " + scale +
						".x > 0.0 ? texture(passImage" + std::to_string(slot) + ", uv * " + scale +
						") : vec4(0.0); }\n";
		}
		for (const scene::MaterialParameter &parameter : layout.Parameters) {
			static constexpr std::array<std::string_view, 4> TYPES{"float", "vec2", "vec3", "vec4"};
			const std::string type(TYPES[parameter.Components - 1]);
			std::string fallback = type + "(";
			for (uint32_t index = 0; index < parameter.Components; ++index) {
				char text[48];
				const auto written = std::to_chars(
					text, text + sizeof(text), parameter.Default[index], std::chars_format::scientific
				);
				fallback += (index == 0 ? "" : ", ") + std::string(text, written.ptr);
			}
			const std::string from = "lighting.Parameter" + std::to_string(parameter.Offset / 4) + "." +
									 std::string("xyzw").substr(parameter.Offset % 4, parameter.Components);
			declared += type + " " + parameter.Name + " = (int(lighting.ParameterState.x + 0.5) >> " +
						std::to_string(parameter.Offset) + " & 1) != 0 ? " + from + " : " + fallback + ");\n";
		}
		if (result.Surface && Declares(WithoutComments(expansion.Out), "void", "Vertex")) {
			const std::string vertex = FunctionText(expansion.Out, "void", "Vertex");
			if (vertex.empty()) {
				result.Error = label + ": Vertex is declared but not defined";
				return result;
			}
			// The fragment stages never see it; blanked so their lines still match.
			const std::string body =
				vertex.substr(vertex.find('\n') + 1, vertex.size() - vertex.find('\n') - 2);
			const size_t at = expansion.Out.find(body);
			if (at != std::string::npos)
				for (size_t index = at; index < at + body.size(); ++index)
					if (expansion.Out[index] != '\n') expansion.Out[index] = ' ';
			// Parameters only: a vertex stage samples no material texture.
			std::string values;
			for (const scene::MaterialParameter &parameter : layout.Parameters) {
				const size_t at = declared.find(" " + parameter.Name + " = (int(lighting.ParameterState");
				const size_t start = declared.rfind('\n', at);
				const size_t end = declared.find('\n', at);
				values += declared.substr(
					start == std::string::npos ? 0 : start + 1,
					end - (start == std::string::npos ? 0 : start + 1) + 1
				);
			}
			const std::array<std::pair<std::string_view, int>, 4> stages{{
				{OPAQUE_VERTEX_GLSL, 1},
				{PACKED_OPAQUE_VERTEX_GLSL, 2},
				{SHADOW_VERTEX_GLSL, 1},
				{PACKED_SHADOW_VERTEX_GLSL, 2},
			}};
			for (size_t index = 0; index < stages.size(); ++index) {
				std::string text = "#version 450\n#extension GL_GOOGLE_cpp_style_line_directive : require\n";
				text += "#define AUTHORED_VERTEX\n#define VERTEX_VALUES_BINDING " +
						std::to_string(stages[index].second) + "\n#line 1 \"engine/vertex\"\n";
				text += ENGINE_VERTEX_GLSL;
				text += '\n';
				text += values;
				text += vertex;
				text += "#line 1 \"engine/vertex-main\"\n";
				text += VertexBody(stages[index].first);
				result.Vertex[index] = std::move(text);
			}
		}
		if (result.Pass) {
			result.Forward = "#version 450\n#extension GL_GOOGLE_cpp_style_line_directive : require\n";
			if (result.Compute) result.Forward += "#define PASS_COMPUTE\n";
			result.Forward += ENGINE_PASS_GLSL;
			result.Forward += declared;
			result.Forward += expansion.Out;
			if (result.Compute) {
				result.Forward += "\n#line 1 \"engine/pass\"\nvoid main() {\n"
								  "	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);\n"
								  "	if (any(greaterThanEqual(pixel, OutputSize()))) return;\n"
								  "	PassInput i;\n	i.UV = (vec2(pixel) + 0.5) / lighting.Output.xy;\n"
								  "	i.Texel = lighting.Target.zw;\n	i.Depth = Depth(i.UV);\n"
								  "	i.PixelsPerUnit = lighting.View.y / max(i.Depth, 1e-4);\n"
								  "	i.Time = lighting.View.x;\n	Compute(i, pixel);\n}\n";
			} else {
				result.Forward += "\n#line 1 \"engine/pass\"\nvoid main() {\n"
								  "	PassInput i;\n	i.UV = inUv;\n	i.Texel = lighting.Target.zw;\n"
								  "	i.Depth = Depth(inUv);\n"
								  "	i.PixelsPerUnit = lighting.View.y / max(i.Depth, 1e-4);\n"
								  "	i.Time = lighting.View.x;\n	outColour = Pass(i);\n}\n";
			}
			return result;
		}
		const auto wrap = [&](bool gbuffer, bool bake = false) {
			std::string text = "#version 450\n#extension GL_GOOGLE_cpp_style_line_directive : require\n";
			text += "#define AUTHORED_SURFACE\n#define MAX_LIGHTS " + std::to_string(MAX_SCENE_LIGHTS) + "\n";
			if (result.Lit) text += "#define SURFACE_LIT\n";
			if (gbuffer) text += "#define SURFACE_GBUFFER\n";
			if (bake) text += "#define SURFACE_BAKE\n";
			text += SURFACE_SHADING_GLSL;
			text += "\n#line 1 \"engine/brdf\"\n";
			text += ENGINE_BRDF_GLSL;
			text += "\n#line 1 \"engine/surface\"\n";
			text += ENGINE_SURFACE_GLSL;
			text += '\n';
			text += declared;
			text += expansion.Out;
			text += "\n#line 1 \"engine/surface-main\"\n";
			text += ENGINE_SURFACE_MAIN_GLSL;
			return text;
		};
		result.Forward = wrap(false);
		result.GBuffer = wrap(true);
		result.Bake = wrap(false, true);
		return result;
	}
}
