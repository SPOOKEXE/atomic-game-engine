#include "CompleteResolve.hpp"

#include <algorithm>
#include <cctype>

namespace studio {

	namespace {

		// How many assignments an alias or a clone is followed through. A chain
		// longer than this belongs to a generated file rather than to somebody
		// typing, and following one has to stop somewhere.
		constexpr int DEPTH = 8;

		// The effects pass reads at most this much text above the caret, so a
		// huge generated file costs a bounded amount per keystroke.
		constexpr size_t EFFECTS_WINDOW = 200000;

		// The byte that closes the bracket at `open`, skipping strings, or npos.
		size_t Matching(const std::string_view text, const size_t open) {
			int depth = 0;
			char quote = '\0';
			for (size_t at = open; at < text.size(); at++) {
				const char character = text[at];
				if (quote != '\0') {
					if (character == '\\') {
						at++;
					} else if (character == quote) {
						quote = '\0';
					}
					continue;
				}
				if (character == '"' || character == '\'' || character == '`') {
					quote = character;
				} else if (character == '(' || character == '[' || character == '{') {
					depth++;
				} else if (character == ')' || character == ']' || character == '}') {
					if (--depth == 0) {
						return at;
					}
				}
			}
			return std::string_view::npos;
		}

		// The first top-level occurrence of `token`, outside brackets and
		// strings.
		size_t TopLevel(const std::string_view text, const std::string_view token) {
			int depth = 0;
			char quote = '\0';
			for (size_t at = 0; at + token.size() <= text.size(); at++) {
				const char character = text[at];
				if (quote != '\0') {
					if (character == '\\') {
						at++;
					} else if (character == quote) {
						quote = '\0';
					}
					continue;
				}
				if (character == '"' || character == '\'' || character == '`') {
					quote = character;
				} else if (character == '(' || character == '[' || character == '{') {
					depth++;
				} else if (character == ')' || character == ']' || character == '}') {
					depth--;
				} else if (depth == 0 && text.substr(at, token.size()) == token) {
					return at;
				}
			}
			return std::string_view::npos;
		}

		// The single `=` of an assignment, not `==`, `~=`, `<=`, `>=` or `=>`.
		size_t AssignmentSign(const std::string_view text) {
			int depth = 0;
			char quote = '\0';
			for (size_t at = 0; at < text.size(); at++) {
				const char character = text[at];
				if (quote != '\0') {
					if (character == '\\') {
						at++;
					} else if (character == quote) {
						quote = '\0';
					}
					continue;
				}
				if (character == '"' || character == '\'' || character == '`') {
					quote = character;
				} else if (character == '(' || character == '[' || character == '{') {
					depth++;
				} else if (character == ')' || character == ']' || character == '}') {
					depth--;
				} else if (character == '=' && depth == 0) {
					const char before = at > 0 ? text[at - 1] : '\0';
					const char after = at + 1 < text.size() ? text[at + 1] : '\0';
					if (after == '=' || after == '>' || before == '=' || before == '~' || before == '<' ||
						before == '>' || before == '!' || before == '+' || before == '-' || before == '*' ||
						before == '/') {
						if (after == '=') {
							at++;
						}
						continue;
					}
					return at;
				}
			}
			return std::string_view::npos;
		}

		// A type annotation's text is a type and nothing that only looks like
		// one: `part:Destroy()` must not read as `part: Destroy()`.
		bool LooksLikeType(const std::string_view value) {
			if (value.empty()) {
				return false;
			}
			return std::all_of(value.begin(), value.end(), [](const char character) {
				return IsCompletionWordCharacter(character) || character == '.' || character == '?' ||
					   character == '|' || character == '{' || character == '}' || character == ' ' ||
					   character == '<' || character == '>' || character == ',';
			});
		}

		// The last `name = expression` (or `name: Type = expression`) before
		// `before`, on one line.
		struct Assignment {
			size_t At = std::string_view::npos;
			std::string_view Initializer;
			std::string_view Annotation;
		};

		Assignment
		LastAssignment(const std::string_view text, const size_t before, const std::string_view name) {
			Assignment found;
			if (!IsCompletionIdentifier(name)) {
				return found;
			}

			size_t at = 0;
			while (at < before) {
				const size_t hit = text.find(name, at);
				if (hit == std::string_view::npos || hit >= before) {
					break;
				}
				at = hit + name.size();

				// A whole word, and not a member: `model.part = x` does not
				// assign a local called `part`.
				const bool wordStart = hit == 0 || (!IsCompletionWordCharacter(text[hit - 1]) &&
													text[hit - 1] != '.' && text[hit - 1] != ':');
				const bool wordEnd = at >= text.size() || !IsCompletionWordCharacter(text[at]);
				if (!wordStart || !wordEnd) {
					continue;
				}

				const size_t newline = text.find('\n', at);
				const size_t lineEnd =
					std::min(newline == std::string_view::npos ? text.size() : newline, before);

				size_t cursor = at;
				while (cursor < lineEnd && (text[cursor] == ' ' || text[cursor] == '\t')) {
					cursor++;
				}

				std::string_view annotation;
				if (cursor < lineEnd && text[cursor] == ':' && cursor + 1 < lineEnd &&
					text[cursor + 1] != ':') {
					const size_t equals = text.find('=', cursor);
					if (equals == std::string_view::npos || equals >= lineEnd) {
						continue;
					}
					annotation = TrimCompletion(text.substr(cursor + 1, equals - cursor - 1));
					if (!LooksLikeType(annotation)) {
						continue;
					}
					cursor = equals;
				}

				// `p == q` asks about the local rather than answering for it.
				if (cursor >= lineEnd || text[cursor] != '=' ||
					(cursor + 1 < text.size() && (text[cursor + 1] == '=' || text[cursor + 1] == '>'))) {
					continue;
				}

				found.At = hit;
				found.Initializer = text.substr(cursor + 1, lineEnd - cursor - 1);
				found.Annotation = annotation;
			}
			return found;
		}

		// A typed function parameter: `(hit: BasePart)`.
		std::string_view
		ParameterType(const std::string_view text, const size_t before, const std::string_view name) {
			std::string_view type;
			size_t at = 0;
			while (at < before) {
				const size_t hit = text.find(name, at);
				if (hit == std::string_view::npos || hit >= before) {
					break;
				}
				at = hit + name.size();
				if ((hit > 0 && IsCompletionWordCharacter(text[hit - 1])) ||
					(at < text.size() && IsCompletionWordCharacter(text[at]))) {
					continue;
				}

				size_t left = hit;
				while (left > 0 && (text[left - 1] == ' ' || text[left - 1] == '\t')) {
					left--;
				}
				if (left == 0 || (text[left - 1] != '(' && text[left - 1] != ',')) {
					continue;
				}

				size_t cursor = at;
				while (cursor < text.size() && text[cursor] == ' ') {
					cursor++;
				}
				if (cursor + 1 >= text.size() || text[cursor] != ':' || text[cursor + 1] == ':') {
					continue;
				}
				cursor++;

				size_t end = cursor;
				int depth = 0;
				while (end < text.size() && text[end] != '\n') {
					const char character = text[end];
					if (character == '(' || character == '{' || character == '<') {
						depth++;
					} else if ((character == ')' || character == ',') && depth == 0) {
						break;
					} else if (character == ')' || character == '}' || character == '>') {
						depth--;
					}
					end++;
				}
				const std::string_view candidate = TrimCompletion(text.substr(cursor, end - cursor));
				if (LooksLikeType(candidate)) {
					type = candidate;
				}
			}
			return type;
		}

		// The collection a generic `for` hands `name` the values of:
		// `for _, child in workspace:GetChildren() do` answers
		// `workspace:GetChildren()`.
		std::string_view
		LoopSource(const std::string_view text, const size_t before, const std::string_view name) {
			std::string_view source;
			size_t at = 0;
			while (true) {
				const size_t hit = text.find("for ", at);
				if (hit == std::string_view::npos || hit >= before) {
					break;
				}
				at = hit + 4;
				if (hit > 0 && IsCompletionWordCharacter(text[hit - 1])) {
					continue;
				}
				const size_t newline = text.find('\n', hit);
				const std::string_view line = text.substr(
					hit + 4, (newline == std::string_view::npos ? text.size() : newline) - hit - 4
				);
				const size_t in = line.find(" in ");
				if (in == std::string_view::npos) {
					continue;
				}

				// The last name in the list is the value.
				std::string_view names = line.substr(0, in);
				const size_t comma = names.rfind(',');
				const std::string_view value =
					TrimCompletion(comma == std::string_view::npos ? names : names.substr(comma + 1));
				if (value != name) {
					continue;
				}

				std::string_view collection = line.substr(in + 4);
				if (const size_t done = collection.rfind(" do"); done != std::string_view::npos) {
					collection = collection.substr(0, done);
				}
				collection = TrimCompletion(collection);
				for (const std::string_view wrapper : {"ipairs(", "pairs("}) {
					if (collection.starts_with(wrapper) && collection.ends_with(")")) {
						collection =
							collection.substr(wrapper.size(), collection.size() - wrapper.size() - 1);
					}
				}
				source = collection;
			}
			return source;
		}

		bool IsFindChildOfClass(const std::string_view name) {
			return name == "FindFirstChildOfClass" || name == "FindFirstChildWhichIsA";
		}

		bool IsFindAncestorOfClass(const std::string_view name) {
			return name == "FindFirstAncestorOfClass" || name == "FindFirstAncestorWhichIsA";
		}

	}

	bool IsCompletionWordCharacter(const char character) {
		const auto value = static_cast<unsigned char>(character);
		return std::isalnum(value) != 0 || character == '_' || character == '$';
	}

	bool IsCompletionIdentifier(const std::string_view value) {
		if (value.empty() || std::isdigit(static_cast<unsigned char>(value.front())) != 0) {
			return false;
		}
		return std::all_of(value.begin(), value.end(), IsCompletionWordCharacter);
	}

	std::string_view TrimCompletion(const std::string_view value) {
		const size_t first = value.find_first_not_of(" \t\r");
		if (first == std::string_view::npos) {
			return {};
		}
		return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
	}

	std::string_view WithoutComment(const std::string_view value) {
		char quote = '\0';

		for (size_t at = 0; at + 1 < value.size(); at++) {
			const char character = value[at];

			if (quote != '\0') {
				if (character == '\\') {
					at++;
				} else if (character == quote) {
					quote = '\0';
				}
				continue;
			}

			if (character == '"' || character == '\'') {
				quote = character;
				continue;
			}

			if ((character == '-' || character == '/') && value[at + 1] == character) {
				return value.substr(0, at);
			}
		}

		return value;
	}

	std::string_view FirstStringArgument(const std::string_view arguments) {
		const std::string_view value = TrimCompletion(arguments);
		if (value.size() < 2 || (value.front() != '"' && value.front() != '\'')) {
			return {};
		}
		const size_t close = value.find(value.front(), 1);
		return close == std::string_view::npos ? std::string_view{} : value.substr(1, close - 1);
	}

	size_t ExpressionStart(const std::string_view text, size_t at) {
		while (at > 0) {
			const char character = text[at - 1];
			if (IsCompletionWordCharacter(character) || character == '.' || character == ':') {
				at--;
				continue;
			}
			if (character == ')' || character == ']') {
				// Back to the bracket this one closes. A quote inside is skipped
				// with its pair so `GetService(")")` does not end the group.
				const char open = character == ')' ? '(' : '[';
				int depth = 0;
				char quote = '\0';
				size_t cursor = at;
				bool matched = false;
				while (cursor > 0) {
					const char inside = text[cursor - 1];
					cursor--;
					if (quote != '\0') {
						if (inside == quote) {
							quote = '\0';
						}
						continue;
					}
					if (inside == '"' || inside == '\'') {
						quote = inside;
					} else if (inside == character) {
						depth++;
					} else if (inside == open) {
						if (--depth == 0) {
							matched = true;
							break;
						}
					} else if (inside == '\n') {
						break;
					}
				}
				if (!matched) {
					break;
				}
				at = cursor;
				continue;
			}
			break;
		}
		return at;
	}

	ScriptResolver::ScriptResolver(
		const std::string_view text, const size_t caret, const CompletionSources &sources
	)
		: Text(text), Dump(sources.Api != nullptr ? *sources.Api : ApiDump::Engine()) {
		// Node 0 is `game`, whose children are the world's roots.
		Nodes.push_back({"game", "DataModel", -1, false});
		for (const CompletionNode &node : sources.Tree) {
			Nodes.push_back({node.Name, node.ClassName, node.Parent < 0 ? 0 : node.Parent + 1, false});
		}
		ScriptNode = sources.ScriptNode >= 0 && sources.ScriptNode + 1 < static_cast<int>(Nodes.size())
						 ? sources.ScriptNode + 1
						 : -1;

		Children.resize(Nodes.size());
		for (size_t index = 1; index < Nodes.size(); index++) {
			const int parent = Nodes[index].Parent;
			if (parent >= 0 && parent < static_cast<int>(Nodes.size())) {
				Children[static_cast<size_t>(parent)].push_back(static_cast<int>(index));
			}
		}

		ReadEffects(std::min(caret, text.size()));
	}

	int ScriptResolver::AddNode(const std::string &name, const std::string &className, const int parent) {
		const int index = static_cast<int>(Nodes.size());
		Nodes.push_back({name, className, parent, false});
		Children.emplace_back();
		if (parent >= 0) {
			Children[static_cast<size_t>(parent)].push_back(index);
		}
		return index;
	}

	void ScriptResolver::Reparent(const int node, const int parent) {
		if (node <= 0 || parent < 0) {
			return;
		}
		const int old = Nodes[static_cast<size_t>(node)].Parent;
		if (old >= 0) {
			std::vector<int> &siblings = Children[static_cast<size_t>(old)];
			siblings.erase(std::remove(siblings.begin(), siblings.end(), node), siblings.end());
		}
		Nodes[static_cast<size_t>(node)].Parent = parent;
		Children[static_cast<size_t>(parent)].push_back(node);
	}

	bool ScriptResolver::Live(int node) const {
		for (int depth = 0; node >= 0 && depth < 4096; depth++) {
			if (Nodes[static_cast<size_t>(node)].Removed) {
				return false;
			}
			node = Nodes[static_cast<size_t>(node)].Parent;
		}
		return true;
	}

	int ScriptResolver::ChildNamed(const int node, const std::string_view name) const {
		if (node < 0 || node >= static_cast<int>(Children.size())) {
			return -1;
		}
		for (const int child : Children[static_cast<size_t>(node)]) {
			if (Nodes[static_cast<size_t>(child)].Name == name &&
				!Nodes[static_cast<size_t>(child)].Removed) {
				return child;
			}
		}
		return -1;
	}

	std::vector<int> ScriptResolver::ChildrenOf(const int node) const {
		std::vector<int> live;
		if (node < 0 || node >= static_cast<int>(Children.size()) || !Live(node)) {
			return live;
		}
		for (const int child : Children[static_cast<size_t>(node)]) {
			if (!Nodes[static_cast<size_t>(child)].Removed) {
				live.push_back(child);
			}
		}
		return live;
	}

	std::string ScriptResolver::KeyOf(const ResolvedValue &value) const {
		if (value.Node >= 0) {
			const std::string &className = Nodes[static_cast<size_t>(value.Node)].ClassName;
			if (Dump.Type(className) != nullptr) {
				return className;
			}
		}
		return ApiTypeKey(value.Type);
	}

	ScriptResolver::Binding *ScriptResolver::BindingFor(const std::string_view name, const size_t before) {
		const Assignment assignment = LastAssignment(Text, before, name);
		if (assignment.At == std::string_view::npos) {
			return nullptr;
		}
		for (Binding &binding : Bindings) {
			if (binding.Name == name && binding.At == assignment.At) {
				return &binding;
			}
		}
		return nullptr;
	}

	void ScriptResolver::ReadEffects(const size_t caret) {
		const size_t first = caret > EFFECTS_WINDOW ? caret - EFFECTS_WINDOW : 0;
		size_t lineStart = first == 0 ? 0 : Text.find('\n', first);
		if (lineStart == std::string_view::npos) {
			return;
		}
		lineStart = first == 0 ? 0 : lineStart + 1;

		while (lineStart < caret) {
			const size_t newline = Text.find('\n', lineStart);
			const size_t lineEnd = newline == std::string_view::npos ? Text.size() : newline;
			if (lineEnd > caret) {
				// The line being typed is not a statement yet.
				break;
			}
			const std::string_view line = WithoutComment(Text.substr(lineStart, lineEnd - lineStart));

			// `;` separates statements in both languages.
			size_t start = 0;
			while (start <= line.size()) {
				const size_t separator = TopLevel(line.substr(start), ";");
				const size_t end = separator == std::string_view::npos ? line.size() : start + separator;
				ReadStatement(line.substr(start, end - start), lineStart + start);
				if (separator == std::string_view::npos) {
					break;
				}
				start = end + 1;
			}

			if (newline == std::string_view::npos) {
				break;
			}
			lineStart = newline + 1;
		}
	}

	void ScriptResolver::ReadStatement(const std::string_view statement, const size_t at) {
		std::string_view text = TrimCompletion(statement);
		if (text.empty()) {
			return;
		}
		size_t offset = at + static_cast<size_t>(text.data() - statement.data());
		for (const std::string_view keyword : {"local ", "const ", "let ", "var "}) {
			if (text.starts_with(keyword)) {
				text = TrimCompletion(text.substr(keyword.size()));
				offset = at + static_cast<size_t>(text.data() - statement.data());
				break;
			}
		}

		// `x:Destroy()`, or `x.Destroy()` in JavaScript.
		for (const std::string_view destroy : {":Destroy()", ".Destroy()"}) {
			if (text.ends_with(destroy)) {
				const std::string_view chain = text.substr(0, text.size() - destroy.size());
				const ResolvedValue target = Evaluate(chain, offset);
				if (target.Node > 0) {
					Nodes[static_cast<size_t>(target.Node)].Removed = true;
				}
				if (IsCompletionIdentifier(chain)) {
					Destructions.push_back({std::string(chain), offset});
				}
				return;
			}
		}

		const size_t equals = AssignmentSign(text);
		if (equals == std::string_view::npos) {
			return;
		}
		std::string_view target = TrimCompletion(text.substr(0, equals));
		const std::string_view value = TrimCompletion(text.substr(equals + 1));

		if (const size_t colon = target.find(':'); colon != std::string_view::npos) {
			target = TrimCompletion(target.substr(0, colon));
		}

		if (IsCompletionIdentifier(target)) {
			// `local part = Instance.new("Part", workspace)`.
			constexpr std::string_view CONSTRUCT = "Instance.new(";
			if (!value.starts_with(CONSTRUCT) || !value.ends_with(")")) {
				return;
			}
			const std::string_view arguments =
				value.substr(CONSTRUCT.size(), value.size() - CONSTRUCT.size() - 1);
			const std::string_view className = FirstStringArgument(arguments);
			if (Dump.Type(className) == nullptr) {
				return;
			}

			Binding binding;
			binding.Name = std::string(target);
			binding.At = offset + static_cast<size_t>(target.data() - text.data());
			binding.ClassName = std::string(className);

			if (const size_t comma = TopLevel(arguments, ","); comma != std::string_view::npos) {
				const ResolvedValue parent = Evaluate(arguments.substr(comma + 1), offset);
				if (parent.Node >= 0 && Live(parent.Node)) {
					binding.Node = AddNode(binding.ClassName, binding.ClassName, parent.Node);
				}
			}
			Bindings.push_back(std::move(binding));
			return;
		}

		const size_t dot = target.rfind('.');
		if (dot == std::string_view::npos) {
			return;
		}
		const std::string_view object = target.substr(0, dot);
		const std::string_view field = target.substr(dot + 1);
		Binding *binding = IsCompletionIdentifier(object) ? BindingFor(object, offset) : nullptr;

		if (field == "Name") {
			const std::string_view name = FirstStringArgument(value);
			if (name.empty()) {
				return;
			}
			if (binding != nullptr && binding->Node < 0) {
				binding->PendingName = std::string(name);
				return;
			}
			const int node = binding != nullptr ? binding->Node : Evaluate(object, offset).Node;
			if (node > 0) {
				Nodes[static_cast<size_t>(node)].Name = std::string(name);
			}
			return;
		}

		if (field != "Parent") {
			return;
		}

		if (value == "nil" || value == "null" || value == "undefined") {
			const int node = binding != nullptr ? binding->Node : Evaluate(object, offset).Node;
			if (node > 0) {
				Nodes[static_cast<size_t>(node)].Removed = true;
			}
			return;
		}

		const ResolvedValue parent = Evaluate(value, offset);
		if (parent.Node < 0 || !Live(parent.Node)) {
			return;
		}
		if (binding != nullptr && binding->Node < 0) {
			const std::string &name =
				binding->PendingName.empty() ? binding->ClassName : binding->PendingName;
			binding->Node = AddNode(name, binding->ClassName, parent.Node);
			return;
		}
		const int node = binding != nullptr ? binding->Node : Evaluate(object, offset).Node;
		Reparent(node, parent.Node);
	}

	ResolvedValue ScriptResolver::Evaluate(const std::string_view expression, const size_t before) {
		return Evaluate(expression, before, DEPTH);
	}

	ResolvedValue
	ScriptResolver::Evaluate(const std::string_view expression, const size_t before, const int depth) {
		if (depth <= 0) {
			return {};
		}

		std::string_view value = TrimCompletion(WithoutComment(expression));
		while (!value.empty() && value.back() == ';') {
			value = TrimCompletion(value.substr(0, value.size() - 1));
		}
		if (value.empty()) {
			return {};
		}

		// `x :: Part` is the author's word for the type, and the node stays.
		if (const size_t cast = TopLevel(value, "::"); cast != std::string_view::npos) {
			ResolvedValue left = Evaluate(value.substr(0, cast), before, depth - 1);
			left.Type = std::string(TrimCompletion(value.substr(cast + 2)));
			return left;
		}

		ResolvedValue current;
		size_t at = 0;

		if (value.front() == '(') {
			const size_t close = Matching(value, 0);
			if (close == std::string_view::npos) {
				return {};
			}
			current = Evaluate(value.substr(1, close - 1), before, depth - 1);
			at = close + 1;
		} else if (IsCompletionWordCharacter(value.front()) &&
				   std::isdigit(static_cast<unsigned char>(value.front())) == 0) {
			while (at < value.size() && IsCompletionWordCharacter(value[at])) {
				at++;
			}
			const std::string_view name = value.substr(0, at);

			if (at < value.size() && value[at] == '(') {
				// A global function's declared return: `wait(1)`.
				const size_t close = Matching(value, at);
				if (close == std::string_view::npos) {
					return {};
				}
				const ApiGlobal *global = Dump.Global(name);
				if (global == nullptr || global->Overloads.empty()) {
					return {};
				}
				current.Type = global->Overloads.front().Type;
				at = close + 1;
			} else {
				current = Root(name, before, depth);
			}
		} else {
			return {};
		}

		while (at < value.size()) {
			const char character = value[at];

			if (character == '.' || character == ':') {
				size_t end = at + 1;
				while (end < value.size() && IsCompletionWordCharacter(value[end])) {
					end++;
				}
				const std::string_view name = value.substr(at + 1, end - at - 1);
				if (name.empty()) {
					return {};
				}
				if (end < value.size() && value[end] == '(') {
					const size_t close = Matching(value, end);
					if (close == std::string_view::npos) {
						return {};
					}
					current = Call(current, name, value.substr(end + 1, close - end - 1));
					at = close + 1;
				} else if (character == ':') {
					return {};
				} else {
					current = Index(current, name);
					at = end;
				}
				continue;
			}

			if (character == '[') {
				const size_t close = Matching(value, at);
				if (close == std::string_view::npos) {
					return {};
				}
				current = ResolvedValue{ApiElementType(current.Type)};
				at = close + 1;
				continue;
			}

			// An operator, a comparison, anything else: not a chain.
			return {};
		}

		return current;
	}

	ResolvedValue ScriptResolver::Root(const std::string_view name, const size_t before, const int depth) {
		bool found = false;
		ResolvedValue local = Local(name, before, depth, found);
		if (found) {
			return local;
		}

		if (name == "game") {
			return {"DataModel", 0};
		}
		if (name == "workspace") {
			return {"Workspace", ChildNamed(0, "Workspace")};
		}
		if (name == "script") {
			const ApiGlobal *global = Dump.Global("script");
			ResolvedValue value{global != nullptr ? global->Type : std::string(), ScriptNode};
			if (ScriptNode >= 0) {
				value.Type = Nodes[static_cast<size_t>(ScriptNode)].ClassName;
			}
			return value;
		}
		if (const ApiGlobal *global = Dump.Global(name); global != nullptr && !global->Type.empty()) {
			return {global->Type};
		}

		if (const std::string_view type = ParameterType(Text, before, name); !type.empty()) {
			return {std::string(type)};
		}
		if (const std::string_view source = LoopSource(Text, before, name); !source.empty()) {
			return {ApiElementType(Evaluate(source, before, depth - 1).Type)};
		}
		return {};
	}

	ResolvedValue
	ScriptResolver::Local(const std::string_view name, const size_t before, const int depth, bool &found) {
		const Assignment assignment = LastAssignment(Text, before, name);
		found = assignment.At != std::string_view::npos;
		if (!found) {
			return {};
		}

		// Resolved as of the assignment rather than as of the caret, which is
		// both the scope an author means and what makes `a = b` and `b = a`
		// terminate: each hop moves strictly earlier.
		ResolvedValue value = Evaluate(assignment.Initializer, assignment.At, depth - 1);
		if (!assignment.Annotation.empty()) {
			value.Type = std::string(assignment.Annotation);
		}

		for (const Binding &binding : Bindings) {
			if (binding.Name == name && binding.At == assignment.At && binding.Node >= 0) {
				value.Node = binding.Node;
			}
		}
		for (const Destruction &destruction : Destructions) {
			if (destruction.Name == name && destruction.At > assignment.At && destruction.At < before) {
				value.Destroyed = true;
			}
		}
		return value;
	}

	ResolvedValue ScriptResolver::Index(const ResolvedValue &value, const std::string_view name) {
		const std::string key = KeyOf(value);
		const ApiMember *member = key.empty() ? nullptr : Dump.Member(key, name);

		if (value.Node >= 0) {
			if (name == "Parent") {
				const int parent = Nodes[static_cast<size_t>(value.Node)].Parent;
				if (parent >= 0) {
					return {Nodes[static_cast<size_t>(parent)].ClassName, parent};
				}
				return {};
			}

			// A property wins over a child of the same name, as it does at run
			// time; `game.Workspace` is both and keeps the node.
			const bool property = member != nullptr && member->Kind != ApiMemberKind::Method &&
								  member->Kind != ApiMemberKind::Function;
			if (!property || key == "DataModel") {
				if (const int child = ChildNamed(value.Node, name); child >= 0) {
					return {Nodes[static_cast<size_t>(child)].ClassName, child};
				}
			}
		}

		if (member == nullptr || member->Kind == ApiMemberKind::Method ||
			member->Kind == ApiMemberKind::Function) {
			return {};
		}
		return {member->Type};
	}

	ResolvedValue ScriptResolver::Call(
		const ResolvedValue &value, const std::string_view name, const std::string_view arguments
	) {
		const std::string_view literal = FirstStringArgument(arguments);
		const std::string key = KeyOf(value);

		if (name == "GetService" && !literal.empty()) {
			ResolvedValue service;
			if (const ApiService *row = Dump.Service(literal); row != nullptr) {
				service.Type = row->Type;
			} else if (Dump.Type(literal) != nullptr) {
				service.Type = std::string(literal);
			}
			service.Node = ChildNamed(0, literal);
			return service;
		}

		if ((name == "FindFirstChild" || name == "WaitForChild") && !literal.empty()) {
			if (const int child = ChildNamed(value.Node, literal); child >= 0) {
				return {Nodes[static_cast<size_t>(child)].ClassName, child};
			}
			return {"Instance"};
		}

		if ((IsFindChildOfClass(name) || IsFindAncestorOfClass(name)) && Dump.Type(literal) != nullptr) {
			ResolvedValue found{std::string(literal)};
			if (value.Node >= 0 && IsFindChildOfClass(name)) {
				for (const int child : ChildrenOf(value.Node)) {
					if (Dump.IsA(Nodes[static_cast<size_t>(child)].ClassName, literal)) {
						found.Node = child;
						break;
					}
				}
			} else if (value.Node >= 0) {
				for (int up = Nodes[static_cast<size_t>(value.Node)].Parent; up >= 0;
					 up = Nodes[static_cast<size_t>(up)].Parent) {
					if (Dump.IsA(Nodes[static_cast<size_t>(up)].ClassName, literal)) {
						found.Node = up;
						break;
					}
				}
			}
			return found;
		}

		if (name == "FindFirstAncestor" && !literal.empty() && value.Node >= 0) {
			for (int up = Nodes[static_cast<size_t>(value.Node)].Parent; up >= 0;
				 up = Nodes[static_cast<size_t>(up)].Parent) {
				if (Nodes[static_cast<size_t>(up)].Name == literal) {
					return {Nodes[static_cast<size_t>(up)].ClassName, up};
				}
			}
			return {"Instance"};
		}

		// A clone of a `Part` is a `Part`, and a new instance in no tree.
		if (name == "Clone" && TrimCompletion(arguments).empty()) {
			return key.empty() ? ResolvedValue{} : ResolvedValue{key};
		}

		if (key == "@Instance" && name == "new") {
			return Dump.Type(literal) != nullptr ? ResolvedValue{std::string(literal)} : ResolvedValue{};
		}

		if (const ApiMember *member = key.empty() ? nullptr : Dump.Member(key, name); member != nullptr) {
			if (member->Kind == ApiMemberKind::Method || member->Kind == ApiMemberKind::Function ||
				member->Kind == ApiMemberKind::Callback) {
				return {member->Type};
			}
		}
		return {};
	}

}
