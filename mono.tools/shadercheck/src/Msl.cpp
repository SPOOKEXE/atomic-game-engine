#include <cctype>
#include <cstddef>
#include <cstdint>
#include <shadercheck/Msl.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shadercheck {

	namespace {

		// The MSL function qualifier for a stage, or empty for one Metal has no
		// word for.
		std::string_view QualifierFor(Stage stage) {
			switch (stage) {
			case Stage::Vertex:
				return "vertex";
			case Stage::Fragment:
				return "fragment";
			case Stage::Compute:
				return "kernel";
			default:
				return {};
			}
		}

		bool IsIdentifierCharacter(char character) {
			return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
		}

		// The text with comments and string literals blanked out, offsets kept.
		//
		// **Blanked rather than removed**, so every offset found afterwards is
		// still an offset into the file somebody can open, and a line number is a
		// count of newlines rather than a bookkeeping exercise. Everything below
		// scans this copy, which is what stops a `fragment` inside a comment from
		// reading as a second entry point.
		std::string Blanked(std::string_view text) {
			std::string out(text);
			for (size_t index = 0; index < out.size();) {
				const char character = out[index];

				if (character == '/' && index + 1 < out.size() && out[index + 1] == '/') {
					while (index < out.size() && out[index] != '\n') {
						out[index++] = ' ';
					}
					continue;
				}
				if (character == '/' && index + 1 < out.size() && out[index + 1] == '*') {
					out[index++] = ' ';
					out[index++] = ' ';
					while (index < out.size() &&
						   !(out[index] == '*' && index + 1 < out.size() && out[index + 1] == '/')) {
						if (out[index] != '\n') {
							out[index] = ' ';
						}
						++index;
					}
					if (index < out.size()) {
						out[index++] = ' ';
						out[index++] = ' ';
					}
					continue;
				}
				if (character == '"' || character == '\'') {
					const char quote = character;
					out[index++] = ' ';
					while (index < out.size() && out[index] != quote) {
						if (out[index] == '\\' && index + 1 < out.size()) {
							out[index++] = ' ';
						}
						if (index < out.size() && out[index] != '\n') {
							out[index] = ' ';
						}
						++index;
					}
					if (index < out.size()) {
						out[index++] = ' ';
					}
					continue;
				}
				++index;
			}
			return out;
		}

		size_t LineOf(std::string_view text, size_t offset) {
			size_t line = 1;
			for (size_t index = 0; index < offset && index < text.size(); ++index) {
				if (text[index] == '\n') {
					++line;
				}
			}
			return line;
		}

		char Closing(char open) {
			return open == '(' ? ')' : (open == '[' ? ']' : '}');
		}

		// Balanced braces, brackets and parentheses over the whole file.
		//
		// This is the whole of what "syntactically valid" can mean here. A Metal
		// compiler is the thing that would say more and there is none on this
		// platform, so the check is the one that catches a truncated write, a
		// translator that gave up half way, and nothing else — stated rather than
		// dressed up.
		bool Balanced(std::string_view code, std::string_view original, std::string &error) {
			std::vector<std::pair<char, size_t>> open;
			for (size_t index = 0; index < code.size(); ++index) {
				const char character = code[index];
				if (character == '(' || character == '[' || character == '{') {
					open.emplace_back(character, index);
					continue;
				}
				if (character != ')' && character != ']' && character != '}') {
					continue;
				}
				if (open.empty()) {
					error = "line " + std::to_string(LineOf(original, index)) + " closes a '" +
							std::string(1, character) + "' that was never opened";
					return false;
				}
				if (Closing(open.back().first) != character) {
					error = "line " + std::to_string(LineOf(original, index)) + " closes a '" +
							std::string(1, character) + "' where a '" +
							std::string(1, Closing(open.back().first)) + "' was owed";
					return false;
				}
				open.pop_back();
			}
			if (!open.empty()) {
				error = "line " + std::to_string(LineOf(original, open.back().second)) + " opens a '" +
						std::string(1, open.back().first) + "' that is never closed";
				return false;
			}
			return true;
		}

		// Where an entry point qualifier sits at the start of a line.
		std::vector<size_t> EntryPointStarts(std::string_view code) {
			static const std::string_view QUALIFIERS[] = {"vertex", "fragment", "kernel"};

			std::vector<size_t> found;
			size_t lineStart = 0;
			while (lineStart <= code.size()) {
				size_t cursor = lineStart;
				while (cursor < code.size() && (code[cursor] == ' ' || code[cursor] == '\t')) {
					++cursor;
				}
				for (const std::string_view qualifier : QUALIFIERS) {
					if (code.compare(cursor, qualifier.size(), qualifier) != 0) {
						continue;
					}
					const size_t after = cursor + qualifier.size();
					if (after < code.size() && !IsIdentifierCharacter(code[after])) {
						found.push_back(cursor);
					}
					break;
				}

				const size_t newline = code.find('\n', lineStart);
				if (newline == std::string_view::npos) {
					break;
				}
				lineStart = newline + 1;
			}
			return found;
		}

		// One `[[...]]` on a parameter, reduced to what has to be checked.
		struct Attribute {
			// The parameter's name, which SPIRV-Cross takes from the SPIR-V's own
			// `OpName` and is therefore comparable with a `Resource::Name`.
			std::string Parameter;
			// `texture`, `buffer` or `sampler`. Nothing else is inspected.
			std::string Kind;
			uint32_t Index = 0;
		};

		// Every indexed binding attribute in a parameter list.
		//
		// `[[stage_in]]`, `[[position]]` and the rest are stepped over: they are
		// not resource bindings and SDL has nothing to say about them.
		std::vector<Attribute> AttributesIn(std::string_view parameters) {
			std::vector<Attribute> attributes;
			size_t cursor = 0;
			while ((cursor = parameters.find("[[", cursor)) != std::string_view::npos) {
				const size_t nameStart = cursor + 2;
				size_t nameEnd = nameStart;
				while (nameEnd < parameters.size() && IsIdentifierCharacter(parameters[nameEnd])) {
					++nameEnd;
				}
				const std::string_view kind = parameters.substr(nameStart, nameEnd - nameStart);

				if ((kind == "texture" || kind == "buffer" || kind == "sampler") &&
					nameEnd < parameters.size() && parameters[nameEnd] == '(') {
					uint32_t index = 0;
					size_t digits = nameEnd + 1;
					bool any = false;
					while (digits < parameters.size() &&
						   std::isdigit(static_cast<unsigned char>(parameters[digits])) != 0) {
						index = index * 10 + static_cast<uint32_t>(parameters[digits] - '0');
						++digits;
						any = true;
					}

					// The parameter's name is the identifier immediately to the
					// left of the attribute, which is where MSL puts it.
					size_t back = cursor;
					while (back > 0 && (parameters[back - 1] == ' ' || parameters[back - 1] == '\t')) {
						--back;
					}
					const size_t end = back;
					while (back > 0 && IsIdentifierCharacter(parameters[back - 1])) {
						--back;
					}

					if (any) {
						attributes.push_back(
							Attribute{
								std::string(parameters.substr(back, end - back)), std::string(kind), index
							}
						);
					}
				}

				cursor = nameEnd;
			}
			return attributes;
		}

		bool IsTexture(ResourceKind kind) {
			return kind == ResourceKind::SampledTexture || kind == ResourceKind::StorageTexture;
		}
	}

	std::vector<Finding> CheckMsl(const Module &module, std::string_view msl) {
		std::vector<Finding> findings;
		const auto report = [&findings](std::string message) {
			findings.push_back(Finding{std::move(message)});
		};

		if (!module.Parsed()) {
			return findings;
		}
		if (msl.empty()) {
			report("the translated MSL is empty; shadercross wrote a file and no shader");
			return findings;
		}

		if (msl.find("#include <metal_stdlib>") == std::string_view::npos ||
			msl.find("using namespace metal;") == std::string_view::npos) {
			report(
				"the translated MSL has no `#include <metal_stdlib>` and `using namespace metal;` "
				"preamble; whatever it is, Metal will not compile it"
			);
		}

		const std::string code = Blanked(msl);

		std::string imbalance;
		if (!Balanced(code, msl, imbalance)) {
			report("the translated MSL does not parse — " + imbalance);
			return findings;
		}

		const std::vector<size_t> starts = EntryPointStarts(code);
		if (starts.size() != 1) {
			report(
				"the translated MSL declares " + std::to_string(starts.size()) +
				" entry points; SDL_GPUShaderCreateInfo names exactly one"
			);
			return findings;
		}

		const size_t open = code.find('(', starts[0]);
		if (open == std::string::npos) {
			report("the translated MSL's entry point has no parameter list");
			return findings;
		}

		// The declaration reads `<qualifier> <return type> <name>(`, so the name
		// is the identifier immediately before the opening parenthesis.
		size_t nameEnd = open;
		while (nameEnd > starts[0] && (code[nameEnd - 1] == ' ' || code[nameEnd - 1] == '\t')) {
			--nameEnd;
		}
		size_t nameStart = nameEnd;
		while (nameStart > starts[0] && IsIdentifierCharacter(code[nameStart - 1])) {
			--nameStart;
		}
		const std::string_view entryPoint(code.data() + nameStart, nameEnd - nameStart);
		if (entryPoint != MSL_ENTRY_POINT) {
			report(
				"the translated MSL's entry point is '" + std::string(entryPoint) +
				"'; the renderer asks for '" + std::string(MSL_ENTRY_POINT) + "' on this format"
			);
		}

		const std::string_view qualifier = QualifierFor(module.EntryStage);
		if (!qualifier.empty() && code.compare(starts[0], qualifier.size(), qualifier) != 0) {
			report(
				"the translated MSL's entry point is not qualified `" + std::string(qualifier) +
				"`; the module runs at the " + std::string(StageName(module.EntryStage)) + " stage"
			);
		}

		size_t depth = 0;
		size_t close = open;
		for (; close < code.size(); ++close) {
			if (code[close] == '(') {
				++depth;
			} else if (code[close] == ')' && --depth == 0) {
				break;
			}
		}
		if (close >= code.size()) {
			report("the translated MSL's entry point parameter list is never closed");
			return findings;
		}

		const std::string_view parameters(code.data() + open + 1, close - open - 1);
		const std::vector<uint32_t> metal = MetalIndices(module.Resources);

		// **Checked from the emitted side, which is the direction that catches a
		// rename.** Asking "does every resource appear" would pass a translation
		// that dropped one, because SPIRV-Cross legitimately drops a resource the
		// shader declares and never reads — `unlit.frag` declares four textures
		// and samples one. Asking "is every attribute in the file claimed by a
		// resource at that index" has no such hole.
		for (const Attribute &attribute : AttributesIn(parameters)) {
			bool matched = false;

			for (size_t index = 0; index < module.Resources.size(); ++index) {
				const Resource &resource = module.Resources[index];
				const bool texture = attribute.Kind == "texture" && IsTexture(resource.Kind);
				const bool buffer = attribute.Kind == "buffer" && !IsTexture(resource.Kind);
				// A sampler carries the index of the texture it belongs to and a
				// name derived from it, which is how the two are tied together
				// without hard-coding the suffix SPIRV-Cross happens to append.
				const bool sampler = attribute.Kind == "sampler" &&
									 resource.Kind == ResourceKind::SampledTexture &&
									 attribute.Parameter.rfind(resource.Name, 0) == 0;

				if (!texture && !buffer && !sampler) {
					continue;
				}
				if (!sampler && attribute.Parameter != resource.Name) {
					continue;
				}

				matched = true;
				if (metal[index] != attribute.Index) {
					report(
						"'" + attribute.Parameter + "' is at [[" + attribute.Kind + "(" +
						std::to_string(attribute.Index) + ")]] and belongs at [[" + attribute.Kind + "(" +
						std::to_string(metal[index]) +
						")]]; SDL_CreateGPUShader numbers sampled textures then storage textures, and "
						"uniform buffers then storage buffers, in descriptor order"
					);
				}
				break;
			}

			if (!matched) {
				report(
					"'" + attribute.Parameter + "' is bound at [[" + attribute.Kind + "(" +
					std::to_string(attribute.Index) +
					")]] and is not a resource the SPIR-V declares; the translation invented a binding "
					"or renamed one"
				);
			}
		}

		return findings;
	}
}
