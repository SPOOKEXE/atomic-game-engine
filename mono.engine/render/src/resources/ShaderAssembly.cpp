#include "resources/EngineShaderSources.hpp"

#include <engine/render/SceneLight.hpp>
#include <engine/render/resources/ShaderAssembly.hpp>

#include <algorithm>
#include <cctype>
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

		struct Expansion {
			const ShaderModuleResolver &Modules;
			bool Surface = false;
			std::string Out;
			std::string Error;
			// Each module is expanded once, as its own include guard.
			std::unordered_set<std::string> Seen;
			std::vector<std::string> Stack;

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
		return std::nullopt;
	}

	ShaderAssembly
	AssembleShader(std::string_view name, std::string_view source, const ShaderModuleResolver &modules) {
		ShaderAssembly result;
		const std::string code = WithoutComments(source);
		const bool main = Declares(code, "void", "main");
		result.Surface = !main && Declares(code, "void", "Surface");
		result.Lit = result.Surface && Declares(code, "vec3", "Light");
		Expansion expansion{.Modules = modules, .Surface = result.Surface};
		const std::string label = name.empty() ? std::string("shader") : std::string(name);
		if (!expansion.Append(label, source)) {
			result.Error = std::move(expansion.Error);
			return result;
		}
		// A Light can live in a module the script includes.
		if (result.Surface && !result.Lit)
			result.Lit = Declares(WithoutComments(expansion.Out), "vec3", "Light");
		if (!result.Surface) {
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
		const auto wrap = [&](bool gbuffer) {
			std::string text = "#version 450\n#extension GL_GOOGLE_cpp_style_line_directive : require\n";
			text += "#define AUTHORED_SURFACE\n#define MAX_LIGHTS " + std::to_string(MAX_SCENE_LIGHTS) + "\n";
			if (result.Lit) text += "#define SURFACE_LIT\n";
			if (gbuffer) text += "#define SURFACE_GBUFFER\n";
			text += SURFACE_SHADING_GLSL;
			text += "\n#line 1 \"engine/brdf\"\n";
			text += ENGINE_BRDF_GLSL;
			text += "\n#line 1 \"engine/surface\"\n";
			text += ENGINE_SURFACE_GLSL;
			text += '\n';
			text += expansion.Out;
			text += "\n#line 1 \"engine/surface-main\"\n";
			text += ENGINE_SURFACE_MAIN_GLSL;
			return text;
		};
		result.Forward = wrap(false);
		result.GBuffer = wrap(true);
		return result;
	}
}
