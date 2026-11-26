#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/content/MaterialGraphs.hpp>
#include <engine/scene/content/Shaders.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <optional>
#include <unordered_map>
#include <vector>

namespace engine::scene {
	namespace {
		// float is 1, vec2 to vec4 are 2 to 4.
		using Width = uint32_t;

		std::string TypeName(Width width) {
			return width == 1 ? "float" : "vec" + std::to_string(width);
		}

		std::optional<Width> TypeOf(std::string_view word) {
			if (word == "float") return 1;
			if (word == "vec2") return 2;
			if (word == "vec3") return 3;
			if (word == "vec4") return 4;
			return std::nullopt;
		}

		bool Identifier(std::string_view word) {
			if (word.empty() || !(std::isalpha(static_cast<unsigned char>(word[0])) || word[0] == '_')) return false;
			return std::all_of(word.begin(), word.end(), [](char c) {
				return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
			});
		}

		// A value an expression produced: its GLSL and its width.
		struct Value {
			std::string Code;
			Width Type = 1;
		};

		// The engine's inputs to a surface, by graph name.
		const std::unordered_map<std::string_view, Value> &Inputs() {
			static const std::unordered_map<std::string_view, Value> inputs{
				{"uv", {"i.UV", 2}},
				{"meshuv", {"i.MeshUV", 2}},
				{"normal", {"i.Normal", 3}},
				{"position", {"i.WorldPosition", 3}},
				{"view", {"i.View", 3}},
				{"time", {"i.Time", 1}},
				{"tint", {"i.Tint", 4}},
				{"attribute0", {"i.Attributes[0]", 4}},
				{"attribute1", {"i.Attributes[1]", 4}},
				{"attribute2", {"i.Attributes[2]", 4}},
				{"attribute3", {"i.Attributes[3]", 4}},
			};
			return inputs;
		}

		// SurfaceOutput's fields and their widths.
		const std::unordered_map<std::string_view, Width> &Fields() {
			static const std::unordered_map<std::string_view, Width> fields{
				{"Albedo", 3},	 {"Normal", 3},	  {"Roughness", 1}, {"Metalness", 1}, {"Occlusion", 1},
				{"Specular", 1}, {"Emissive", 3}, {"Alpha", 1},	 {"Height", 1},	   {"User", 1},
				{"Custom0", 4},	 {"Custom1", 4},  {"Custom2", 4},  {"Custom3", 4},
			};
			return fields;
		}

		std::string FieldCode(std::string_view field) {
			if (field.starts_with("Custom")) return "Custom[" + std::string(field.substr(6)) + "]";
			return std::string(field);
		}

		struct Compiler {
			std::string Name;
			std::string Declarations;
			std::string Includes;
			std::string Body;
			std::string Error;
			size_t Line = 0;
			std::unordered_map<std::string, Value> Symbols;
			std::vector<std::string> Textures;
			std::vector<std::string> Keywords;
			bool NoiseUsed = false;
			bool BaseSet = false;
			bool Started = false;
			size_t Nodes = 0;

			bool Fail(std::string reason) {
				if (Error.empty()) Error = std::to_string(Line) + ": " + std::move(reason);
				return false;
			}

			std::string LineMark() const {
				return "#line " + std::to_string(Line) + " \"" + Name + "\"\n";
			}

			// Tokens of one statement: words, numbers, parentheses and `=`.
			static std::vector<std::string> Tokens(std::string_view text) {
				std::vector<std::string> out;
				for (size_t at = 0; at < text.size();) {
					const char c = text[at];
					if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
						++at;
					} else if (c == '(' || c == ')' || c == '=') {
						out.emplace_back(1, c);
						++at;
					} else {
						size_t end = at;
						while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) &&
							   text[end] != '(' && text[end] != ')' && text[end] != '=' && text[end] != ',')
							++end;
						out.emplace_back(text.substr(at, end - at));
						at = end;
					}
				}
				return out;
			}

			// Widens a float to `width`; a vector must already be that width.
			std::optional<std::string> To(const Value &value, Width width, std::string_view what) {
				if (value.Type == width) return value.Code;
				if (value.Type == 1) return TypeName(width) + "(" + value.Code + ")";
				Fail(std::string(what) + " is " + TypeName(value.Type) + " where " + TypeName(width) + " is needed");
				return std::nullopt;
			}

			std::optional<Value> Swizzle(Value value, std::string_view swizzle) {
				if (swizzle.empty()) return value;
				if (swizzle.size() > 4) {
					Fail("a swizzle takes at most four of x, y, z and w");
					return std::nullopt;
				}
				for (const char c : swizzle) {
					const size_t index = std::string_view("xyzw").find(c);
					const size_t colour = std::string_view("rgba").find(c);
					const size_t at = index != std::string_view::npos ? index : colour;
					if (at == std::string_view::npos || at >= value.Type) {
						Fail("'" + std::string(swizzle) + "' reaches past a " + TypeName(value.Type));
						return std::nullopt;
					}
				}
				return Value{"(" + value.Code + ")." + std::string(swizzle), static_cast<Width>(swizzle.size())};
			}

			// One operand: a number, a name (optionally swizzled) or a bracket.
			std::optional<Value> Operand(const std::vector<std::string> &tokens, size_t &at) {
				if (at >= tokens.size()) {
					Fail("an operand is missing");
					return std::nullopt;
				}
				const std::string token = tokens[at++];
				if (token == "(") {
					auto value = Expression(tokens, at, ")");
					if (!value) return std::nullopt;
					if (at >= tokens.size() || tokens[at] != ")") {
						Fail("a bracket is not closed");
						return std::nullopt;
					}
					++at;
					return value;
				}
				double number = 0;
				const auto parsed = std::from_chars(token.data(), token.data() + token.size(), number);
				if (parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size()) {
					std::string code = token;
					if (code.find_first_of(".eE") == std::string::npos) code += ".0";
					return Value{"(" + code + ")", 1};
				}
				const size_t dot = token.find('.');
				const std::string head = token.substr(0, dot);
				std::string swizzle = dot == std::string::npos ? std::string() : token.substr(dot + 1);
				if (head == "base") {
					const size_t next = swizzle.find('.');
					const std::string field = swizzle.substr(0, next);
					const auto found = Fields().find(field);
					if (found == Fields().end()) {
						Fail("the surface has no field '" + field + "'");
						return std::nullopt;
					}
					return Swizzle(
						{"base." + FieldCode(field), found->second},
						next == std::string::npos ? std::string_view{} : std::string_view(swizzle).substr(next + 1)
					);
				}
				if (const auto input = Inputs().find(head); input != Inputs().end())
					return Swizzle(input->second, swizzle);
				if (const auto symbol = Symbols.find(head); symbol != Symbols.end())
					return Swizzle(symbol->second, swizzle);
				Fail("nothing called '" + head + "'");
				return std::nullopt;
			}

			// `op operand...` until `end` (a closing bracket or the line's end).
			std::optional<Value> Expression(const std::vector<std::string> &tokens, size_t &at, std::string_view end) {
				if (at >= tokens.size()) {
					Fail("an expression is missing");
					return std::nullopt;
				}
				const std::string op = tokens[at];
				const auto operands = [&](size_t count) -> std::optional<std::vector<Value>> {
					std::vector<Value> out;
					for (size_t index = 0; index < count; ++index) {
						auto value = Operand(tokens, at);
						if (!value) return std::nullopt;
						out.push_back(std::move(*value));
					}
					return out;
				};
				const auto widest = [](const std::vector<Value> &values) {
					Width width = 1;
					for (const Value &value : values)
						width = std::max(width, value.Type);
					return width;
				};
				// Element-wise functions: every operand widened to the widest.
				static const std::unordered_map<std::string_view, std::pair<std::string_view, size_t>> ELEMENTWISE{
					{"add", {"+", 2}},		 {"sub", {"-", 2}},			   {"mul", {"*", 2}},
					{"div", {"/", 2}},		 {"min", {"min", 2}},		   {"max", {"max", 2}},
					{"pow", {"pow", 2}},	 {"mix", {"mix", 3}},		   {"clamp", {"clamp", 3}},
					{"step", {"step", 2}},	 {"smoothstep", {"smoothstep", 3}}, {"abs", {"abs", 1}},
					{"fract", {"fract", 1}}, {"floor", {"floor", 1}},	   {"sin", {"sin", 1}},
					{"cos", {"cos", 1}},	 {"sqrt", {"sqrt", 1}},		   {"saturate", {"saturate", 1}},
					{"oneminus", {"oneminus", 1}}, {"normalize", {"normalize", 1}},
				};
				if (const auto found = ELEMENTWISE.find(op); found != ELEMENTWISE.end()) {
					++at;
					auto values = operands(found->second.second);
					if (!values) return std::nullopt;
					const Width width = widest(*values);
					std::vector<std::string> codes;
					for (const Value &value : *values) {
						auto code = To(value, width, op + "'s operand");
						if (!code) return std::nullopt;
						codes.push_back(*code);
					}
					const std::string_view name = found->second.first;
					std::string code;
					if (name.size() == 1)
						code = "(" + codes[0] + " " + std::string(name) + " " + codes[1] + ")";
					else if (name == "saturate")
						code = "clamp(" + codes[0] + ", 0.0, 1.0)";
					else if (name == "oneminus")
						code = "(1.0 - " + codes[0] + ")";
					else {
						code = std::string(name) + "(";
						for (size_t index = 0; index < codes.size(); ++index)
							code += (index ? ", " : "") + codes[index];
						code += ")";
					}
					return Value{code, width};
				}
				if (op == "dot" || op == "distance" || op == "length") {
					++at;
					auto values = operands(op == "length" ? 1 : 2);
					if (!values) return std::nullopt;
					if (values->size() == 2 && (*values)[0].Type != (*values)[1].Type)
						return Fail(op + " takes two of one width"), std::nullopt;
					std::string code = op + "(" + (*values)[0].Code;
					if (values->size() == 2) code += ", " + (*values)[1].Code;
					return Value{code + ")", 1};
				}
				if (op == "cross") {
					++at;
					auto values = operands(2);
					if (!values) return std::nullopt;
					if ((*values)[0].Type != 3 || (*values)[1].Type != 3) return Fail("cross takes two vec3"), std::nullopt;
					return Value{"cross(" + (*values)[0].Code + ", " + (*values)[1].Code + ")", 3};
				}
				if (const auto width = TypeOf(op); width && *width > 1) {
					// vecN from parts whose widths add to N, or one float.
					++at;
					std::vector<std::string> codes;
					Width total = 0;
					while (at < tokens.size() && tokens[at] != end && total < *width) {
						auto value = Operand(tokens, at);
						if (!value) return std::nullopt;
						total += value->Type;
						codes.push_back(value->Code);
					}
					if (!(total == *width || (codes.size() == 1 && total == 1)))
						return Fail(op + " takes parts adding up to " + std::to_string(*width)), std::nullopt;
					std::string code = op + "(";
					for (size_t index = 0; index < codes.size(); ++index)
						code += (index ? ", " : "") + codes[index];
					return Value{code + ")", *width};
				}
				if (op == "sample") {
					++at;
					if (at >= tokens.size() || std::find(Textures.begin(), Textures.end(), tokens[at]) == Textures.end())
						return Fail("sample takes a declared texture"), std::nullopt;
					const std::string texture = tokens[at++];
					auto uv = Operand(tokens, at);
					if (!uv) return std::nullopt;
					if (uv->Type != 2) return Fail("sample takes vec2 coordinates"), std::nullopt;
					return Value{"(" + texture + "Bound() ? texture(" + texture + ", " + uv->Code + ") : vec4(0.0))", 4};
				}
				if (op == "noise") {
					++at;
					auto values = operands(2);
					if (!values) return std::nullopt;
					auto uv = To((*values)[0], 2, "noise's coordinates");
					if (!uv || (*values)[1].Type != 1) return Fail("noise takes vec2 coordinates and a float scale"), std::nullopt;
					NoiseUsed = true;
					return Value{"EngineGraphNoise(" + *uv + " * " + (*values)[1].Code + ")", 1};
				}
				if (op == "normalmap") {
					++at;
					auto value = Operand(tokens, at);
					if (!value) return std::nullopt;
					if (value->Type != 3) return Fail("normalmap takes a vec3 in 0 to 1"), std::nullopt;
					return Value{"ApplyNormalMap(i.Normal, i.UV, " + value->Code + ")", 3};
				}
				if (op == "call") {
					// `call type Module.Function args...`
					++at;
					if (at + 1 >= tokens.size()) return Fail("call takes a type and Module.Function"), std::nullopt;
					const auto width = TypeOf(tokens[at++]);
					const std::string target = tokens[at++];
					const size_t dot = target.rfind('.');
					if (!width || dot == std::string::npos || !Identifier(target.substr(dot + 1)))
						return Fail("call takes a type and Module.Function"), std::nullopt;
					const std::string module = target.substr(0, dot);
					const std::string include = "#include \"" + module + "\"\n";
					if (Includes.find(include) == std::string::npos) Includes += include;
					std::string code = target.substr(dot + 1) + "(";
					bool first = true;
					while (at < tokens.size() && tokens[at] != end) {
						auto value = Operand(tokens, at);
						if (!value) return std::nullopt;
						code += (first ? "" : ", ") + value->Code;
						first = false;
					}
					return Value{code + ")", *width};
				}
				// A bare operand.
				return Operand(tokens, at);
			}

			void Start() {
				if (Started) return;
				Started = true;
				Body += "void Surface(SurfaceInput i, inout SurfaceOutput o) {\n";
				if (!BaseSet) Body += "\tDefaultSurface(i, o);\n";
				Body += "\tSurfaceOutput base = o;\n";
			}

			bool Statement(std::string_view text) {
				const std::vector<std::string> tokens = Tokens(text);
				if (tokens.empty()) return true;
				const std::string &kind = tokens[0];
				if (kind == "parameter" || kind == "texture" || kind == "keyword" || kind == "base") {
					if (Started) return Fail("declarations come before the first node or output");
					if (kind == "parameter") {
						if (tokens.size() < 3 || !TypeOf(tokens[1]) || !Identifier(tokens[2]) || Symbols.contains(tokens[2]))
							return Fail("a parameter is `parameter float|vec2|vec3|vec4 name = value`");
						Declarations += "#pragma " + std::string(Trim(text)) + "\n";
						Symbols[tokens[2]] = {tokens[2], *TypeOf(tokens[1])};
					} else if (kind == "texture") {
						if (tokens.size() != 2 || !Identifier(tokens[1]) || Symbols.contains(tokens[1]))
							return Fail("a texture is `texture name`");
						Declarations += "#pragma texture " + tokens[1] + "\n";
						Textures.push_back(tokens[1]);
					} else if (kind == "keyword") {
						if (tokens.size() != 2 || !Identifier(tokens[1])) return Fail("a keyword is `keyword NAME`");
						if (Keywords.size() >= MAX_GRAPH_KEYWORDS)
							return Fail("a graph declares at most " + std::to_string(MAX_GRAPH_KEYWORDS) + " keywords");
						Declarations += "#pragma keyword " + tokens[1] + "\n";
						Keywords.push_back(tokens[1]);
					} else {
						if (tokens.size() != 2 || (tokens[1] != "default" && tokens[1] != "none"))
							return Fail("base is `base default` or `base none`");
						BaseSet = tokens[1] == "none";
					}
					return true;
				}
				Start();
				if (kind == "node") {
					if (tokens.size() < 4 || !Identifier(tokens[1]) || tokens[2] != "=")
						return Fail("a node is `node name = op operands...`");
					if (Symbols.contains(tokens[1]) || Inputs().contains(tokens[1]) || tokens[1] == "base")
						return Fail("'" + tokens[1] + "' is already a name");
					if (++Nodes > MAX_GRAPH_NODES) return Fail("a graph holds at most 512 nodes");
					size_t at = 3;
					auto value = Expression(tokens, at, "");
					if (!value) return false;
					if (at != tokens.size()) return Fail("'" + tokens[at] + "' is left over");
					const std::string symbol = "graph_" + tokens[1];
					Body += LineMark() + "\t" + TypeName(value->Type) + " " + symbol + " = " + value->Code + ";\n";
					Symbols[tokens[1]] = {symbol, value->Type};
					return true;
				}
				if (kind.starts_with("output")) {
					std::string keyword;
					if (kind.size() > 6) {
						if (kind[6] != '@' || std::find(Keywords.begin(), Keywords.end(), kind.substr(7)) == Keywords.end())
							return Fail("an output is `output Field = ...` or `output@KEYWORD Field = ...`");
						keyword = kind.substr(7);
					}
					if (tokens.size() < 4 || tokens[2] != "=") return Fail("an output is `output Field = expression`");
					const auto field = Fields().find(tokens[1]);
					if (field == Fields().end()) return Fail("the surface has no field '" + tokens[1] + "'");
					size_t at = 3;
					auto value = Expression(tokens, at, "");
					if (!value) return false;
					if (at != tokens.size()) return Fail("'" + tokens[at] + "' is left over");
					auto code = To(*value, field->second, tokens[1]);
					if (!code) return false;
					if (!keyword.empty()) Body += "#if " + keyword + "\n";
					Body += LineMark() + "\to." + FieldCode(tokens[1]) + " = " + *code + ";\n";
					if (!keyword.empty()) Body += "#endif\n";
					return true;
				}
				if (kind == "discard") {
					size_t at = 1;
					auto value = Expression(tokens, at, "");
					if (!value) return false;
					if (value->Type != 1) return Fail("discard takes a float: below one half is cut away");
					Body += LineMark() + "\tif (" + value->Code + " < 0.5) discard;\n";
					return true;
				}
				return Fail("unknown statement '" + kind + "'");
			}

			static std::string_view Trim(std::string_view text) {
				while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
				while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
				return text;
			}
		};

		// Value noise in 0..1, smooth between lattice points.
		constexpr std::string_view NOISE = R"(float EngineGraphHash(vec2 p) {
	return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}
float EngineGraphNoise(vec2 p) {
	vec2 cell = floor(p), f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(EngineGraphHash(cell), EngineGraphHash(cell + vec2(1, 0)), u.x),
			   mix(EngineGraphHash(cell + vec2(0, 1)), EngineGraphHash(cell + vec2(1, 1)), u.x), u.y);
}
)";
	}

	CompiledGraph CompileMaterialGraph(std::string_view document, std::string_view name) {
		Compiler compiler;
		compiler.Name = name.empty() ? "graph" : std::string(name);
		for (size_t at = 0; at <= document.size() && compiler.Error.empty();) {
			size_t end = document.find('\n', at);
			if (end == std::string_view::npos) end = document.size();
			++compiler.Line;
			std::string_view line = document.substr(at, end - at);
			if (const size_t comment = line.find('#'); comment != std::string_view::npos) line = line.substr(0, comment);
			(void)compiler.Statement(line);
			at = end + 1;
		}
		CompiledGraph result;
		if (!compiler.Error.empty()) {
			result.Error = compiler.Error;
			return result;
		}
		compiler.Start();
		result.Source = compiler.Declarations + compiler.Includes;
		if (compiler.NoiseUsed) result.Source += NOISE;
		result.Source += compiler.Body + "}\n";
		return result;
	}

}
