#include "ValuePayload.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/PcxExpression.hpp>

#include <charconv>
#include <numbers>

namespace engine::imagegraph {
	namespace {
		struct Parser {
			std::string Source;
			size_t Position = 0, Tokens = 0;
			PcxExpressionValue Output;
			Diagnostic Error;
			struct Token {
				std::string Text;
				bool Number = false, String = false;
			} Current;
			bool Fail(std::string text, Status status = Status::InvalidValue) {
				if (Error.Code == Status::Ok) Error = {status, {}, "equation", std::move(text)};
				return false;
			}
			void Next() {
				Current = {};
				if (++Tokens > 16384) {
					Fail("PCX token budget exceeded", Status::LimitExceeded);
					return;
				}
				while (Position < Source.size() && (Source[Position] == ' ' || Source[Position] == '\n'))
					++Position;
				if (Position == Source.size()) return;
				const size_t start = Position;
				if (Source.compare(start, 3, "∸") == 0) {
					Position += 3;
					Current.Text = "∸";
					return;
				}
				const char c = Source[Position++];
				if (c == '"') {
					Current.String = true;
					while (Position < Source.size() && Source[Position] != '"')
						++Position;
					if (Position == Source.size()) {
						Fail("unterminated PCX string");
						return;
					}
					Current.Text = Source.substr(start + 1, Position - start - 1);
					++Position;
					return;
				}
				for (const auto &op :
					 {"**",
					  "<<",
					  ">>",
					  "==",
					  "!=",
					  "<>",
					  ">=",
					  "<=",
					  "+=",
					  "-=",
					  "*=",
					  "/=",
					  "++",
					  "--",
					  ".."})
					if (Source.compare(start, 2, op) == 0) {
						++Position;
						Current.Text = op;
						return;
					}
				if (std::string_view("()+-*/%$&|^<>~=,[]{};:").find(c) != std::string_view::npos) {
					Current.Text = c;
					return;
				}
				// Numbers follow the source real converter, including exponent notation.
				if ((c >= '0' && c <= '9') || (c == '.' && Position < Source.size() &&
											   Source[Position] >= '0' && Source[Position] <= '9')) {
					const char *begin = Source.data() + start, *end = Source.data() + Source.size();
					double number = 0;
					const auto parsed = std::from_chars(begin, end, number, std::chars_format::general);
					if (parsed.ec == std::errc{}) {
						Position = size_t(parsed.ptr - Source.data());
						if (Position > start && Source[Position - 1] == '.' && Position < Source.size() &&
							Source[Position] == '.')
							--Position;
						Current.Text = Source.substr(start, Position - start);
						Current.Number = true;
						return;
					}
				}
				while (Position < Source.size() && Source.compare(Position, 2, "..") != 0 &&
					   Source.compare(Position, 3, "∸") != 0 &&
					   std::string_view(" ()+-*/%$&|^<>~=,[]{};:\n").find(Source[Position]) ==
						   std::string_view::npos)
					++Position;
				Current.Text = Source.substr(start, Position - start);
			}
			uint32_t
			Add(std::string operation, std::vector<uint32_t> arguments = {}, Value literal = double{0}) {
				auto &nodes = Output.Data->Instructions;
				if (nodes.size() >= 4096) {
					Fail("PCX instruction budget exceeded", Status::LimitExceeded);
					return 0;
				}
				nodes.push_back({std::move(operation), std::move(literal), std::move(arguments)});
				return uint32_t(nodes.size() - 1);
			}
			static int Precedence(std::string_view op) {
				if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=" || op == "++" ||
					op == "--")
					return -99;
				if (op == "==" || op == "!=" || op == "<>") return -1;
				if (op == "<" || op == ">" || op == "<=" || op == ">=") return 0;
				if (op == "+" || op == "-") return 1;
				if (op == "*" || op == "/" || op == "%") return 2;
				if (op == "**" || op == "$" || op == "^") return 3;
				if (op == "|") return 4;
				if (op == "&") return 5;
				if (op == "<<" || op == ">>") return 6;
				if (op == ".." || op == "∸") return 9;
				return -1000;
			}
			uint32_t Expression(int minimum = -99, size_t depth = 1) {
				if (depth > 64) {
					Fail("PCX expression nesting exceeds budget", Status::LimitExceeded);
					return 0;
				}
				uint32_t left = 0;
				if (Current.Text.empty() && !Current.String) {
					Fail("PCX expression is incomplete");
					return 0;
				}
				if (Current.Text == "-" || Current.Text == "~" || Current.Text == "+") {
					const auto op = Current.Text;
					Next();
					left = Expression(9, depth + 1);
					if (op != "+") left = Add(op == "-" ? "neg" : "~", {left});
				} else if (Current.Text == "(") {
					Next();
					left = Expression(-99, depth + 1);
					if (Current.Text != ")")
						Fail("PCX closing parenthesis is missing");
					else
						Next();
				} else if (Current.Text == "[") {
					Next();
					std::vector<uint32_t> members;
					if (Current.Text != "]")
						while (Error.Code == Status::Ok) {
							members.push_back(Expression(-99, depth + 1));
							if (Current.Text != ",") break;
							Next();
							if (Current.Text == "]") break;
						}
					if (Current.Text != "]")
						Fail("PCX closing bracket is missing");
					else
						Next();
					left = Add("array", std::move(members));
				} else if (Current.Number) {
					double value = 0;
					auto parsed = std::from_chars(
						Current.Text.data(), Current.Text.data() + Current.Text.size(), value
					);
					if (parsed.ec != std::errc{} || !std::isfinite(value))
						Fail("PCX numeric literal is invalid");
					left = Add("literal", {}, value);
					Next();
				} else if (Current.String) {
					left = Add("literal", {}, Current.Text);
					Next();
				} else {
					if (Current.Text == ")" || Current.Text == "]" || Current.Text == "," ||
						Precedence(Current.Text) != -1000) {
						Fail("unexpected PCX operator");
						return 0;
					}
					auto name = Current.Text;
					Next();
					if (Current.Text == "(") {
						Next();
						std::vector<uint32_t> arguments;
						if (Current.Text != ")")
							while (Error.Code == Status::Ok) {
								arguments.push_back(Expression(-99, depth + 1));
								if (Current.Text != ",") break;
								Next();
							}
						if (Current.Text != ")")
							Fail("PCX function closing parenthesis is missing");
						else
							Next();
						left = Add("call:" + name, std::move(arguments));
					} else if (name == "pi")
						left = Add("literal", {}, std::numbers::pi);
					else if (name == "e")
						left = Add("literal", {}, 2.7182818284);
					else
						left = Add("name", {}, std::move(name));
				}
				while (Error.Code == Status::Ok) {
					if (Current.Text == "[") {
						if (minimum > 5) break;
						Next();
						const auto index = Expression(-99, depth + 1);
						if (Current.Text != "]")
							Fail("PCX index closing bracket is missing");
						else
							Next();
						left = Add("index", {left, index});
						continue;
					}
					const auto precedence = Precedence(Current.Text);
					if (precedence < minimum) break;
					const auto op = Current.Text;
					Next();
					const auto right = op == "++" || op == "--" ? Add("literal", {}, double{1})
																: Expression(precedence + 1, depth + 1);
					left =
						Add(op == "++"	 ? "+="
							: op == "--" ? "-="
							: op == "∸"	 ? "negsub"
										 : op,
							{left, right});
				}
				return left;
			}
			void Separators() {
				while (Current.Text == ";")
					Next();
			}
			bool Expect(std::string_view token) {
				if (Current.Text != token) return Fail("PCX statement requires " + std::string(token));
				Next();
				return true;
			}
			uint32_t Block(size_t depth) {
				Separators();
				if (!Expect("{")) return 0;
				const auto body = Program(depth + 1);
				Expect("}");
				Separators();
				return body;
			}
			uint32_t Conditional(size_t depth) {
				Next();
				Expect("(");
				const auto condition = Expression(-99, depth + 1);
				Expect(")");
				const auto yes = Block(depth + 1);
				uint32_t no = 0;
				if (Current.Text == "elseif")
					no = Conditional(depth + 1);
				else if (Current.Text == "else") {
					Next();
					no = Block(depth + 1);
				} else
					no = Add("sequence");
				return Add("if", {condition, yes, no});
			}
			uint32_t Statement(size_t depth) {
				if (depth > 64) {
					Fail("PCX statement nesting exceeds budget", Status::LimitExceeded);
					return 0;
				}
				if (Current.Text == "if") return Conditional(depth + 1);
				if (Current.Text != "for") return Expression(-99, depth + 1);
				Next();
				Expect("(");
				auto first = Expression(-99, depth + 1);
				std::string index;
				if (Current.Text == ",") {
					if (Output.Data->Instructions[first].Operation != "name") {
						Fail("PCX foreach index must be a name");
						return 0;
					}
					index = std::get<std::string>(Output.Data->Instructions[first].Literal);
					Next();
					first = Expression(-99, depth + 1);
				}
				Expect(":");
				const auto second = Expression(-99, depth + 1);
				if (Current.Text == ":") {
					if (!index.empty()) {
						Fail("PCX numeric for cannot have an index binding");
						return 0;
					}
					Next();
					const auto test = Expression(-99, depth + 1);
					Expect(")");
					const auto body = Block(depth + 1);
					return Add("for", {first, test, second, body});
				}
				const auto &binding = Output.Data->Instructions[first];
				if (binding.Operation != "name") {
					Fail("PCX foreach value must be a name");
					return 0;
				}
				const auto name = std::get<std::string>(binding.Literal);
				Expect(")");
				const auto body = Block(depth + 1);
				return Add("for_each", {second, body}, index + "\n" + name);
			}
			uint32_t Program(size_t depth = 1) {
				std::vector<uint32_t> statements;
				Separators();
				while (!Current.Text.empty() && Current.Text != "}" && Error.Code == Status::Ok) {
					const bool structured = Current.Text == "if" || Current.Text == "for";
					statements.push_back(Statement(depth + 1));
					if (structured) continue;
					if (Current.Text == ";")
						Separators();
					else if (Current.Text != "}" && !Current.Text.empty() && Current.Text != "if" &&
							 Current.Text != "for") {
						Fail("PCX statements require newline boundaries");
						break;
					}
				}
				return Add("sequence", std::move(statements));
			}
		};
	}
	static Status
	CompilePcx(std::string_view source, PcxExpressionValue &output, Diagnostic &diagnostic, bool program) {
		ENGINE_PROFILE("imagegraph.pcx.compile");
		if (source.size() > Limits::MaximumTextBytes) {
			diagnostic = {Status::LimitExceeded, {}, "equation", "PCX source byte budget exceeded"};
			return diagnostic.Code;
		}
		Parser parser;
		bool quoted = false;
		for (char character : source) {
			if (character == '"') quoted = !quoted;
			if (character == '\n' && program && !quoted)
				parser.Source += ';';
			else if (character != '\n' && (quoted || character != ' '))
				parser.Source += character;
		}
		// These are the source cleaner's internal operator tokens.
		for (const auto &[from, to] :
			 {std::pair<std::string_view, std::string_view>{"$", "**"},
			  {"»", ">>"},
			  {"«", "<<"},
			  {"⩵", "=="},
			  {"≠", "!="},
			  {"≤", "<="},
			  {"≥", ">="},
			  {"⊕", "+="},
			  {"⊖", "-="},
			  {"⊗", "*="},
			  {"⊘", "/="},
			  {"◘", ".."}}) {
			size_t offset = 0;
			while ((offset = parser.Source.find(from, offset)) != std::string::npos) {
				parser.Source.replace(offset, from.size(), to);
				offset += to.size();
			}
		}
		parser.Output.Data.emplace();
		parser.Next();
		if (parser.Current.Text.empty()) {
			parser.Output.Data->Root = parser.Add("literal", {}, UndefinedValue{});
		} else
			parser.Output.Data->Root = program ? parser.Program() : parser.Expression();
		if (!parser.Current.Text.empty() && parser.Error.Code == Status::Ok)
			parser.Fail("PCX expression has trailing tokens");
		if (parser.Error.Code != Status::Ok) {
			diagnostic = std::move(parser.Error);
			return diagnostic.Code;
		}
		if (!detail::ValidPcxPayload(parser.Output)) {
			diagnostic = {Status::LimitExceeded, {}, "equation", "PCX tree storage exceeds budget"};
			return diagnostic.Code;
		}
		output = std::move(parser.Output);
		diagnostic = {};
		return Status::Ok;
	}
	Status CompilePcxExpression(std::string_view source, PcxExpressionValue &output, Diagnostic &diagnostic) {
		return CompilePcx(source, output, diagnostic, false);
	}
	Status CompilePcxProgram(std::string_view source, PcxExpressionValue &output, Diagnostic &diagnostic) {
		return CompilePcx(source, output, diagnostic, true);
	}

}
