#pragma once

// ECMAScript byte regex matching with explicit parser, state and instruction bounds.
// The source DLL uses std::regex_search, including for its node named Match.

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace engine::imagegraph::detail::bounded_regex {
	inline constexpr size_t MAXIMUM_NODES = 2048, MAXIMUM_CAPTURES = 32, MAXIMUM_STATES = 16384,
							MAXIMUM_STEPS = 1000000;
	enum class Kind {
		Empty,
		Literal,
		Any,
		Class,
		Sequence,
		Alternative,
		Capture,
		Backreference,
		Repeat,
		Begin,
		End,
		Boundary,
		Lookahead
	};
	struct Node {
		Kind Type = Kind::Empty;
		std::vector<size_t> Children;
		std::array<uint64_t, 4> Characters{};
		size_t Minimum = 0, Maximum = 0, Capture = 0;
		uint8_t Character = 0;
		bool Negate = false, Greedy = true;
	};
	struct Pattern {
		std::string_view Source;
		std::vector<Node> Nodes;
		size_t Offset = 0, Captures = 0, Root = 0;
		bool Error = false, Limited = false;
		size_t Add(Node node) {
			if (Nodes.size() == MAXIMUM_NODES) {
				Limited = true;
				return 0;
			}
			Nodes.push_back(std::move(node));
			return Nodes.size() - 1;
		}
		bool Take(char c) {
			if (Offset < Source.size() && Source[Offset] == c) {
				++Offset;
				return true;
			}
			return false;
		}
		static bool Word(uint8_t c) {
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
		}
		static bool Space(uint8_t c) {
			return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
		}
		static void Set(Node &node, uint8_t c) {
			node.Characters[c / 64] |= uint64_t{1} << (c % 64);
		}
		bool Hex(size_t amount, uint8_t &value) {
			uint32_t number = 0;
			for (size_t i = 0; i < amount; ++i) {
				if (Offset == Source.size()) return false;
				const char c = Source[Offset++];
				const int digit = c >= '0' && c <= '9'	 ? c - '0'
								  : c >= 'a' && c <= 'f' ? c - 'a' + 10
								  : c >= 'A' && c <= 'F' ? c - 'A' + 10
														 : -1;
				if (digit < 0) return false;
				number = number * 16 + uint32_t(digit);
			}
			value = uint8_t(number);
			return true;
		}
		Node Escape(bool inClass) {
			Node node;
			if (Offset == Source.size()) {
				Error = true;
				return node;
			}
			const char c = Source[Offset++];
			if (c == 'd' || c == 'D' || c == 's' || c == 'S' || c == 'w' || c == 'W') {
				node.Type = Kind::Class;
				node.Negate = c == 'D' || c == 'S' || c == 'W';
				for (size_t i = 0; i < 256; ++i) {
					const uint8_t byte = uint8_t(i);
					const bool set = c == 'd' || c == 'D'	? (byte >= '0' && byte <= '9')
									 : c == 's' || c == 'S' ? Space(byte)
															: Word(byte);
					if (set) Set(node, byte);
				}
				return node;
			}
			if (!inClass && (c == 'b' || c == 'B')) {
				node.Type = Kind::Boundary;
				node.Negate = c == 'B';
				return node;
			}
			if (!inClass && c >= '1' && c <= '9') {
				node.Type = Kind::Backreference;
				node.Capture = size_t(c - '0');
				while (Offset < Source.size() && Source[Offset] >= '0' && Source[Offset] <= '9') {
					node.Capture = node.Capture * 10 + size_t(Source[Offset++] - '0');
					if (node.Capture > MAXIMUM_CAPTURES) {
						Error = true;
						break;
					}
				}
				return node;
			}
			node.Type = Kind::Literal;
			node.Character = uint8_t(c);
			switch (c) {
			case 'b':
				node.Character = 8;
				break;
			case 'f':
				node.Character = 12;
				break;
			case 'n':
				node.Character = 10;
				break;
			case 'r':
				node.Character = 13;
				break;
			case 't':
				node.Character = 9;
				break;
			case 'v':
				node.Character = 11;
				break;
			case '0':
				node.Character = 0;
				break;
			case 'x':
				Error = !Hex(2, node.Character);
				break;
			case 'u':
				Error = !Hex(4, node.Character);
				break;
			case 'c':
				if (Offset == Source.size())
					Error = true;
				else {
					const auto control = uint8_t(Source[Offset++]);
					node.Character = control;
				}
				break;
			default:
				break;
			}
			return node;
		}
		Node CharacterClass() {
			Node node;
			node.Type = Kind::Class;
			node.Negate = Take('^');
			while (Offset < Source.size() && Source[Offset] != ']') {
				Node start;
				if (Take('\\'))
					start = Escape(true);
				else {
					start.Type = Kind::Literal;
					start.Character = uint8_t(Source[Offset++]);
				}
				if (start.Type == Kind::Literal && Offset + 1 < Source.size() && Source[Offset] == '-' &&
					Source[Offset + 1] != ']') {
					++Offset;
					Node last;
					if (Take('\\'))
						last = Escape(true);
					else {
						last.Type = Kind::Literal;
						last.Character = uint8_t(Source[Offset++]);
					}
					if (last.Type != Kind::Literal || last.Character < start.Character) {
						Error = true;
						return node;
					}
					for (size_t c = start.Character; c <= last.Character; ++c)
						Set(node, uint8_t(c));
				} else if (start.Type == Kind::Class) {
					for (size_t c = 0; c < 256; ++c) {
						bool present = (start.Characters[c / 64] >> (c % 64)) & 1;
						if (present != start.Negate) Set(node, uint8_t(c));
					}
				} else
					Set(node, start.Character);
			}
			if (!Take(']')) Error = true;
			return node;
		}
		bool Number(size_t &number) {
			const size_t start = Offset;
			number = 0;
			while (Offset < Source.size() && Source[Offset] >= '0' && Source[Offset] <= '9') {
				const size_t digit = size_t(Source[Offset++] - '0');
				if (number > (std::numeric_limits<size_t>::max() - digit) / 10) {
					Error = true;
					return false;
				}
				number = number * 10 + digit;
			}
			return start != Offset;
		}
		size_t Atom(size_t depth) {
			if (depth > 64) {
				Limited = true;
				return 0;
			}
			if (Offset == Source.size()) {
				Error = true;
				return 0;
			}
			Node node;
			const char c = Source[Offset++];
			switch (c) {
			case '(': {
				bool capture = true, lookahead = false, negative = false;
				if (Take('?')) {
					if (Take(':'))
						capture = false;
					else if (Take('=')) {
						capture = false;
						lookahead = true;
					} else if (Take('!')) {
						capture = false;
						lookahead = true;
						negative = true;
					} else {
						Error = true;
						return 0;
					}
				}
				size_t group = 0;
				if (capture) {
					if (Captures == MAXIMUM_CAPTURES) {
						Limited = true;
						return 0;
					}
					group = ++Captures;
				}
				const size_t child = Alternative(depth + 1);
				if (!Take(')')) Error = true;
				node.Type = lookahead ? Kind::Lookahead : capture ? Kind::Capture : Kind::Sequence;
				node.Capture = group;
				node.Negate = negative;
				node.Children.push_back(child);
				break;
			}
			case '[':
				node = CharacterClass();
				break;
			case '\\':
				node = Escape(false);
				break;
			case '.':
				node.Type = Kind::Any;
				break;
			case '^':
				node.Type = Kind::Begin;
				break;
			case '$':
				node.Type = Kind::End;
				break;
			case '*':
			case '+':
			case '?':
			case ')':
				Error = true;
				break;
			default:
				node.Type = Kind::Literal;
				node.Character = uint8_t(c);
				break;
			}
			const size_t child = Add(std::move(node));
			if (Error || Limited || Offset == Source.size()) return child;
			Node repeat;
			repeat.Type = Kind::Repeat;
			repeat.Children.push_back(child);
			repeat.Maximum = std::numeric_limits<size_t>::max();
			if (Take('*'))
				repeat.Minimum = 0;
			else if (Take('+'))
				repeat.Minimum = 1;
			else if (Take('?')) {
				repeat.Minimum = 0;
				repeat.Maximum = 1;
			} else if (Take('{')) {
				if (!Number(repeat.Minimum)) {
					Error = true;
					return child;
				}
				if (Take(',')) {
					if (!Number(repeat.Maximum)) repeat.Maximum = std::numeric_limits<size_t>::max();
				} else
					repeat.Maximum = repeat.Minimum;
				if (!Take('}') || repeat.Maximum < repeat.Minimum) {
					Error = true;
					return child;
				}
			} else
				return child;
			repeat.Greedy = !Take('?');
			return Add(std::move(repeat));
		}
		size_t Sequence(size_t depth) {
			Node node;
			node.Type = Kind::Sequence;
			while (Offset < Source.size() && Source[Offset] != '|' && Source[Offset] != ')' && !Error &&
				   !Limited)
				node.Children.push_back(Atom(depth));
			return Add(std::move(node));
		}
		size_t Alternative(size_t depth) {
			Node node;
			node.Type = Kind::Alternative;
			node.Children.push_back(Sequence(depth));
			while (Take('|') && !Error && !Limited)
				node.Children.push_back(Sequence(depth));
			return Add(std::move(node));
		}
		bool Compile() {
			Root = Alternative(0);
			if (Offset != Source.size()) Error = true;
			for (const auto &node : Nodes)
				if (node.Type == Kind::Backreference && node.Capture > Captures) Error = true;
			return !Error && !Limited;
		}
	};
	struct Capture {
		size_t Start = 0, End = 0;
		bool Matched = false;
	};
	struct State {
		size_t Offset = 0;
		std::array<Capture, MAXIMUM_CAPTURES + 1> Captures{};
	};
	using States = std::vector<State>;
	struct Machine {
		const Pattern &Compiled;
		std::string_view Text;
		size_t Steps = 0, Produced = 0;
		bool Limited = false;
		bool Step() {
			if (++Steps > MAXIMUM_STEPS) {
				Limited = true;
				return false;
			}
			return true;
		}
		bool Push(States &states, const State &state) {
			if (++Produced > MAXIMUM_STATES) {
				Limited = true;
				return false;
			}
			states.push_back(state);
			return true;
		}
		States Apply(size_t index, const State &input, size_t depth = 0) {
			States output;
			if (!Step() || depth > 128) {
				Limited = true;
				return output;
			}
			const auto &node = Compiled.Nodes[index];
			switch (node.Type) {
			case Kind::Empty:
				Push(output, input);
				break;
			case Kind::Literal:
				if (input.Offset < Text.size() && uint8_t(Text[input.Offset]) == node.Character) {
					auto next = input;
					++next.Offset;
					Push(output, next);
				}
				break;
			case Kind::Any:
				if (input.Offset < Text.size() && Text[input.Offset] != '\n' && Text[input.Offset] != '\r') {
					auto next = input;
					++next.Offset;
					Push(output, next);
				}
				break;
			case Kind::Class:
				if (input.Offset < Text.size()) {
					const uint8_t c = uint8_t(Text[input.Offset]);
					const bool present = (node.Characters[c / 64] >> (c % 64)) & 1;
					if (present != node.Negate) {
						auto next = input;
						++next.Offset;
						Push(output, next);
					}
				}
				break;
			case Kind::Begin:
				if (input.Offset == 0) Push(output, input);
				break;
			case Kind::End:
				if (input.Offset == Text.size()) Push(output, input);
				break;
			case Kind::Boundary: {
				const bool left = input.Offset > 0 && Pattern::Word(uint8_t(Text[input.Offset - 1])),
						   right = input.Offset < Text.size() && Pattern::Word(uint8_t(Text[input.Offset]));
				if ((left != right) != node.Negate) Push(output, input);
				break;
			}
			case Kind::Backreference: {
				const auto capture = input.Captures[node.Capture];
				const auto value = capture.Matched ? Text.substr(capture.Start, capture.End - capture.Start)
												   : std::string_view{};
				if (Text.substr(input.Offset).starts_with(value)) {
					auto next = input;
					next.Offset += value.size();
					Push(output, next);
				}
				break;
			}
			case Kind::Sequence: {
				States current;
				Push(current, input);
				for (size_t child : node.Children) {
					States next;
					for (const auto &state : current) {
						auto matched = Apply(child, state, depth + 1);
						for (const auto &item : matched)
							if (!Push(next, item)) break;
						if (Limited) break;
					}
					current = std::move(next);
					if (current.empty() || Limited) break;
				}
				return current;
			}
			case Kind::Alternative:
				for (size_t child : node.Children) {
					auto matched = Apply(child, input, depth + 1);
					for (const auto &state : matched)
						if (!Push(output, state)) break;
					if (Limited) break;
				}
				break;
			case Kind::Capture: {
				auto matched = Apply(node.Children.front(), input, depth + 1);
				for (auto &state : matched) {
					state.Captures[node.Capture] = {input.Offset, state.Offset, true};
					if (!Push(output, state)) break;
				}
				break;
			}
			case Kind::Lookahead: {
				auto matched = Apply(node.Children.front(), input, depth + 1);
				if (node.Negate) {
					if (matched.empty() && !Limited) Push(output, input);
				} else if (!matched.empty()) {
					auto state = matched.front();
					state.Offset = input.Offset;
					Push(output, state);
				}
				break;
			}
			case Kind::Repeat: {
				struct Frame {
					State Current;
					size_t Count = 0, Next = 0;
					States Children;
					bool Expanded = false;
				};
				std::vector<Frame> stack;
				stack.push_back({input, 0, 0, {}, false});
				while (!stack.empty() && !Limited) {
					if (!Step()) break;
					auto &frame = stack.back();
					if (!frame.Expanded) {
						frame.Expanded = true;
						if (!node.Greedy && frame.Count >= node.Minimum) Push(output, frame.Current);
						if (frame.Count < node.Maximum && frame.Count <= Text.size() + 1) {
							auto seed = frame.Current;
							frame.Children = Apply(node.Children.front(), seed, depth + 1);
						}
					}
					if (frame.Next < frame.Children.size()) {
						auto next = frame.Children[frame.Next++];
						if (next.Offset == frame.Current.Offset) {
							if (frame.Count + 1 >= node.Minimum) Push(output, next);
							continue;
						}
						if (stack.size() == MAXIMUM_STATES) {
							Limited = true;
							break;
						}
						stack.push_back({next, frame.Count + 1, 0, {}, false});
						continue;
					}
					if (node.Greedy && frame.Count >= node.Minimum) Push(output, frame.Current);
					stack.pop_back();
				}
				break;
			}
			}
			return output;
		}
		bool At(size_t start, State &match, bool nonempty = false) {
			State input;
			input.Offset = start;
			auto states = Apply(Compiled.Root, input);
			for (auto &state : states) {
				if (nonempty && state.Offset == start) continue;
				state.Captures[0] = {start, state.Offset, true};
				match = state;
				return true;
			}
			return false;
		}

		bool Search(size_t offset, State &match, bool nonempty = false) {
			for (size_t start = offset; start <= Text.size() && !Limited; ++start) {
				State input;
				input.Offset = start;
				auto states = Apply(Compiled.Root, input);
				for (auto &state : states) {
					if (nonempty && state.Offset == start) continue;
					state.Captures[0] = {start, state.Offset, true};
					match = state;
					return true;
				}
			}
			return false;
		}
	};
}
