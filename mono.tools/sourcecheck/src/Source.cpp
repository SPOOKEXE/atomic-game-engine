#include <algorithm>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <sourcecheck/Source.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace sourcecheck {

	namespace {

		namespace fs = std::filesystem;

		bool IsSourceIdentifierChar(char c) {
			return (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '_';
		}

		// A word boundary check, so `namespace` matches and `mynamespace` does not.
		bool WordAt(std::string_view text, size_t index, std::string_view word) {
			if (index + word.size() > text.size() || text.compare(index, word.size(), word) != 0) {
				return false;
			}
			if (index > 0 && IsSourceIdentifierChar(text[index - 1])) {
				return false;
			}
			const size_t after = index + word.size();
			return after >= text.size() || !IsSourceIdentifierChar(text[after]);
		}

		size_t SkipSpace(std::string_view text, size_t index) {
			while (index < text.size() && (std::isspace(static_cast<unsigned char>(text[index])) != 0)) {
				index++;
			}
			return index;
		}

		// Reads an identifier, allowing `::` so a qualified name comes back whole.
		std::string ReadName(std::string_view text, size_t &index) {
			const size_t start = index;
			while (index < text.size()) {
				if (IsSourceIdentifierChar(text[index])) {
					index++;
				} else if (text[index] == ':' && index + 1 < text.size() && text[index + 1] == ':') {
					index += 2;
				} else {
					break;
				}
			}
			return std::string(text.substr(start, index - start));
		}

		size_t LineOf(std::string_view text, size_t offset) {
			return static_cast<size_t>(
					   std::count(text.begin(), text.begin() + static_cast<long>(offset), '\n')
				   ) +
				   1;
		}

		// The keywords that appear where a member's type would, and mean the
		// declaration is not a data member.
		bool IsDeclarationKeyword(std::string_view word) {
			static constexpr std::string_view WORDS[] = {
				"using",	 "typedef",	  "static",	   "friend",  "return",	  "enum",	"template", "public",
				"private",	 "protected", "explicit",  "virtual", "operator", "struct", "class",	"union",
				"namespace", "constexpr", "consteval", "inline",  "extern",	  "auto",	"decltype",
			};
			for (const std::string_view candidate : WORDS) {
				if (word == candidate) {
					return true;
				}
			}
			return false;
		}

		// The identifiers that can sit in front of a `(` without the record having
		// behaviour.
		bool IsControlWord(std::string_view word) {
			static constexpr std::string_view WORDS[] = {
				"if",
				"for",
				"while",
				"switch",
				"return",
				"sizeof",
				"alignof",
				"static_assert",
				"noexcept",
				"decltype",
			};
			for (const std::string_view candidate : WORDS) {
				if (word == candidate) {
					return true;
				}
			}
			return false;
		}

		// Splits a record body into `;`-terminated declarations at the body's own
		// depth, so a nested record contributes one entry rather than its members.
		std::vector<std::pair<size_t, std::string>> Declarations(std::string_view body, size_t bodyStart) {
			std::vector<std::pair<size_t, std::string>> parts;
			int depth = 0;
			size_t start = 0;
			for (size_t index = 0; index < body.size(); index++) {
				const char c = body[index];
				if (c == '{' || c == '(' || c == '[') {
					depth++;
				} else if (c == '}' || c == ')' || c == ']') {
					depth--;
				} else if (c == ';' && depth == 0) {
					parts.emplace_back(bodyStart + start, std::string(body.substr(start, index - start)));
					start = index + 1;
				}
			}
			if (start < body.size()) {
				parts.emplace_back(bodyStart + start, std::string(body.substr(start)));
			}
			return parts;
		}

		std::string Collapse(std::string_view text) {
			std::string out;
			bool space = false;
			for (const char c : text) {
				if (std::isspace(static_cast<unsigned char>(c)) != 0) {
					space = !out.empty();
					continue;
				}
				if (space) {
					out.push_back(' ');
					space = false;
				}
				out.push_back(c);
			}
			return out;
		}

		// Whether a `;`-terminated declaration declares a function.
		//
		// The text is cut at the first `=` first, so `int Count = Size();` is data
		// with an initialiser rather than behaviour.
		bool DeclaresFunction(std::string_view declaration) {
			int depth = 0;
			size_t end = declaration.size();
			for (size_t index = 0; index < declaration.size(); index++) {
				const char c = declaration[index];
				if (c == '{' || c == '(' || c == '[' || c == '<') {
					depth++;
				} else if (c == '}' || c == ')' || c == ']' || c == '>') {
					depth--;
				} else if (c == '=' && depth == 0) {
					end = index;
					break;
				}
			}

			const std::string_view head = declaration.substr(0, end);
			for (size_t index = 0; index < head.size(); index++) {
				if (head[index] != '(') {
					continue;
				}
				size_t back = index;
				while (back > 0 && (std::isspace(static_cast<unsigned char>(head[back - 1])) != 0)) {
					back--;
				}
				const size_t nameEnd = back;
				while (back > 0 && IsSourceIdentifierChar(head[back - 1])) {
					back--;
				}
				if (back == nameEnd) {
					continue;
				}
				if (!IsControlWord(head.substr(back, nameEnd - back))) {
					return true;
				}
			}
			return false;
		}

		// The offset of the last character of a declaration's head, so a member
		// spread over several lines is reported on the line its name is on rather
		// than on the line the previous declaration ended.
		size_t HeadEnd(std::string_view declaration) {
			int depth = 0;
			size_t end = declaration.size();
			for (size_t index = 0; index < declaration.size(); index++) {
				const char c = declaration[index];
				if (c == '(' || c == '[' || c == '{' || c == '<') {
					depth++;
				} else if (c == ')' || c == ']' || c == '}' || c == '>') {
					depth--;
				} else if (c == '=' && depth == 0) {
					end = index;
					break;
				}
			}
			while (end > 0 && (std::isspace(static_cast<unsigned char>(declaration[end - 1])) != 0)) {
				end--;
			}
			return (end == 0) ? 0 : end - 1;
		}

		// Pulls `TYPE NAME` out of one declaration, or reports that it is not a
		// data member.
		bool AsMember(std::string_view declaration, std::string &type, std::string &name) {
			int depth = 0;
			size_t end = declaration.size();
			for (size_t index = 0; index < declaration.size(); index++) {
				const char c = declaration[index];
				if (c == '(' || c == '[' || c == '{') {
					depth++;
				} else if (c == ')' || c == ']' || c == '}') {
					depth--;
				} else if (c == '=' && depth == 0) {
					end = index;
					break;
				}
			}

			std::string head = Collapse(declaration.substr(0, end));

			// An access specifier has no `;` after it, so it arrives glued to the
			// front of the declaration that follows it.
			for (const std::string_view specifier : {"public:", "private:", "protected:"}) {
				const size_t at = head.rfind(specifier);
				if (at != std::string::npos) {
					head = head.substr(at + specifier.size());
				}
			}
			const size_t lead = head.find_first_not_of(' ');
			head = (lead == std::string::npos) ? std::string() : head.substr(lead);

			// A trailing array bound is part of the declarator, not the name.
			const size_t bracket = head.find('[');
			if (bracket != std::string::npos) {
				head = head.substr(0, bracket);
			}
			// A bit-field declares its width after a colon. `::` is qualification
			// and is not one.
			for (size_t scan = 0; scan + 1 < head.size() + 1 && scan < head.size(); scan++) {
				if (head[scan] != ':') {
					continue;
				}
				if (scan + 1 < head.size() && head[scan + 1] == ':') {
					scan++;
					continue;
				}
				head = head.substr(0, scan);
				break;
			}
			while (!head.empty() && head.back() == ' ') {
				head.pop_back();
			}
			if (head.empty() || head.find('(') != std::string::npos || head.find('{') != std::string::npos) {
				return false;
			}

			const size_t nameEnd = head.size();
			if (!IsSourceIdentifierChar(head[nameEnd - 1])) {
				return false;
			}
			size_t nameStart = nameEnd;
			while (nameStart > 0 && IsSourceIdentifierChar(head[nameStart - 1])) {
				nameStart--;
			}
			if (nameStart == 0) {
				return false;
			}

			name = head.substr(nameStart, nameEnd - nameStart);
			std::string front = head.substr(0, nameStart);
			while (!front.empty() && front.back() == ' ') {
				front.pop_back();
			}
			if (front.empty()) {
				return false;
			}

			// `mutable const std::vector<T>` is still a vector of T; the leading
			// specifier is not the type and is not what a rule matches on.
			size_t cursor = 0;
			while (cursor < front.size()) {
				const size_t wordEnd = front.find(' ', cursor);
				const std::string word =
					front.substr(cursor, wordEnd == std::string::npos ? std::string::npos : wordEnd - cursor);
				if (word == "mutable" || word == "const" || word == "volatile") {
					cursor = (wordEnd == std::string::npos) ? front.size() : wordEnd + 1;
					continue;
				}
				break;
			}
			front = front.substr(cursor);
			if (front.empty()) {
				return false;
			}

			const size_t firstEnd = front.find(' ');
			const std::string firstWord = front.substr(0, firstEnd);
			if (IsDeclarationKeyword(firstWord) || firstWord.starts_with("#")) {
				return false;
			}

			type = front;
			return true;
		}
	}

	std::string Strip(std::string_view text) {
		std::string out(text.size(), ' ');
		size_t index = 0;
		while (index < text.size()) {
			const char c = text[index];

			if (c == '\n') {
				out[index] = '\n';
				index++;
				continue;
			}

			if (c == '/' && index + 1 < text.size() && text[index + 1] == '/') {
				while (index < text.size() && text[index] != '\n') {
					index++;
				}
				continue;
			}

			if (c == '/' && index + 1 < text.size() && text[index + 1] == '*') {
				index += 2;
				while (index + 1 < text.size() && !(text[index] == '*' && text[index + 1] == '/')) {
					if (text[index] == '\n') {
						out[index] = '\n';
					}
					index++;
				}
				index = std::min(index + 2, text.size());
				continue;
			}

			// A raw string ends at its own delimiter, so a `//` or a brace inside
			// embedded shader source is text rather than code.
			if (c == 'R' && index + 1 < text.size() && text[index + 1] == '"') {
				const size_t open = text.find('(', index + 2);
				if (open != std::string::npos) {
					const std::string closing =
						")" + std::string(text.substr(index + 2, open - index - 2)) + "\"";
					const size_t close = text.find(closing, open);
					const size_t end = (close == std::string::npos) ? text.size() : close + closing.size();
					for (size_t scan = index; scan < end; scan++) {
						out[scan] = (text[scan] == '\n') ? '\n' : ' ';
					}
					index = end;
					continue;
				}
			}

			if (c == '"' || c == '\'') {
				out[index] = c;
				index++;
				while (index < text.size() && text[index] != c) {
					if (text[index] == '\\') {
						index++;
					}
					if (index < text.size() && text[index] == '\n') {
						out[index] = '\n';
					}
					index++;
				}
				if (index < text.size()) {
					out[index] = c;
					index++;
				}
				continue;
			}

			out[index] = c;
			index++;
		}
		return out;
	}

	std::vector<Record> Records(std::string_view text) {
		std::vector<Record> found;

		// One linear pass with a scope stack. Namespaces and records are pushed
		// by name; every other brace is pushed anonymously, so a function body
		// cannot be mistaken for a record's.
		struct Scope {
			enum class Kind : uint8_t { Namespace, Record, Other };
			Kind What = Kind::Other;
			std::string Name;
			size_t Index = 0;
			size_t BodyStart = 0;
		};
		std::vector<Scope> stack;

		const auto namespaceChain = [&stack]() {
			std::string chain;
			for (const Scope &scope : stack) {
				if (scope.What != Scope::Kind::Namespace || scope.Name.empty()) {
					continue;
				}
				if (!chain.empty()) {
					chain += "::";
				}
				chain += scope.Name;
			}
			return chain;
		};

		const auto enclosingRecord = [&stack]() {
			for (size_t index = stack.size(); index > 0; index--) {
				if (stack[index - 1].What == Scope::Kind::Record) {
					return stack[index - 1].Name;
				}
			}
			return std::string();
		};

		size_t index = 0;
		while (index < text.size()) {
			const char c = text[index];

			if (c == '}') {
				if (!stack.empty()) {
					const Scope scope = stack.back();
					stack.pop_back();
					if (scope.What == Scope::Kind::Record) {
						Record &record = found[scope.Index];
						const std::string_view body = text.substr(scope.BodyStart, index - scope.BodyStart);
						for (const auto &[offset, declaration] : Declarations(body, scope.BodyStart)) {
							if (DeclaresFunction(declaration)) {
								record.HasBehaviour = true;
								continue;
							}
							std::string type;
							std::string name;
							if (AsMember(declaration, type, name)) {
								record.Members.push_back(
									{type, name, LineOf(text, offset + HeadEnd(declaration))}
								);
							}
						}
					}
				}
				index++;
				continue;
			}

			if (c == '{') {
				stack.push_back({Scope::Kind::Other, "", 0, index + 1});
				index++;
				continue;
			}

			if (WordAt(text, index, "namespace")) {
				size_t cursor = SkipSpace(text, index + 9);
				const std::string name = ReadName(text, cursor);
				cursor = SkipSpace(text, cursor);
				if (cursor < text.size() && text[cursor] == '{') {
					stack.push_back({Scope::Kind::Namespace, name, 0, cursor + 1});
					index = cursor + 1;
					continue;
				}
				index += 9;
				continue;
			}

			// `enum class` and `enum struct` open an enumeration, not a record,
			// and reading one as a record would mint a member out of its last
			// enumerator.
			size_t before = index;
			while (before > 0 && (std::isspace(static_cast<unsigned char>(text[before - 1])) != 0)) {
				before--;
			}
			const bool afterEnum = before >= 4 && WordAt(text, before - 4, "enum");

			const bool isStruct = WordAt(text, index, "struct");
			const bool isClass = WordAt(text, index, "class");
			const bool isUnion = WordAt(text, index, "union");
			if (!afterEnum && (isStruct || isClass || isUnion)) {
				const size_t keyword = isClass ? 5u : (isUnion ? 5u : 6u);
				size_t cursor = SkipSpace(text, index + keyword);
				const std::string name = ReadName(text, cursor);
				cursor = SkipSpace(text, cursor);

				// `final`, then an optional base list, then the body. A `;` or a
				// `(` first means a forward declaration or an elaborated type in
				// a parameter, and neither opens a scope.
				if (WordAt(text, cursor, "final")) {
					cursor = SkipSpace(text, cursor + 5);
				}
				if (cursor < text.size() && text[cursor] == ':') {
					while (cursor < text.size() && text[cursor] != '{' && text[cursor] != ';' &&
						   text[cursor] != ')') {
						cursor++;
					}
				}
				if (!name.empty() && cursor < text.size() && text[cursor] == '{') {
					Record record;
					record.Name = name;
					record.Namespace = namespaceChain();
					record.Enclosing = enclosingRecord();
					record.Line = LineOf(text, index);
					found.push_back(record);
					stack.push_back({Scope::Kind::Record, name, found.size() - 1, cursor + 1});
					index = cursor + 1;
					continue;
				}
				index += keyword;
				continue;
			}

			index++;
		}

		return found;
	}

	std::vector<Enumeration> Enums(std::string_view text) {
		std::vector<Enumeration> found;
		std::vector<std::string> namespaces;
		std::vector<bool> named;

		size_t index = 0;
		while (index < text.size()) {
			const char c = text[index];
			if (c == '}') {
				if (!named.empty()) {
					if (named.back()) {
						namespaces.pop_back();
					}
					named.pop_back();
				}
				index++;
				continue;
			}
			if (c == '{') {
				named.push_back(false);
				index++;
				continue;
			}
			if (WordAt(text, index, "namespace")) {
				size_t cursor = SkipSpace(text, index + 9);
				const std::string name = ReadName(text, cursor);
				cursor = SkipSpace(text, cursor);
				if (cursor < text.size() && text[cursor] == '{' && !name.empty()) {
					namespaces.push_back(name);
					named.push_back(true);
					index = cursor + 1;
					continue;
				}
				index += 9;
				continue;
			}
			if (WordAt(text, index, "enum")) {
				size_t cursor = SkipSpace(text, index + 4);
				if (WordAt(text, cursor, "class")) {
					cursor = SkipSpace(text, cursor + 5);
				} else if (WordAt(text, cursor, "struct")) {
					cursor = SkipSpace(text, cursor + 6);
				}
				const std::string name = ReadName(text, cursor);
				if (!name.empty()) {
					std::string chain;
					for (const std::string &part : namespaces) {
						if (!chain.empty()) {
							chain += "::";
						}
						chain += part;
					}
					found.push_back({name, chain, LineOf(text, index)});
				}
				index += 4;
				continue;
			}
			index++;
		}

		return found;
	}

	std::vector<std::string> Includes(std::string_view raw) {
		std::vector<std::string> found;
		size_t line = 0;
		while (line < raw.size()) {
			size_t end = raw.find('\n', line);
			if (end == std::string_view::npos) {
				end = raw.size();
			}
			const std::string_view text = raw.substr(line, end - line);
			const size_t hash = text.find('#');
			if (hash != std::string_view::npos && text.find("include", hash) != std::string_view::npos) {
				const size_t open = text.find_first_of("<\"", hash);
				if (open != std::string_view::npos) {
					const char closing = (text[open] == '<') ? '>' : '"';
					const size_t close = text.find(closing, open + 1);
					if (close != std::string_view::npos) {
						found.emplace_back(text.substr(open + 1, close - open - 1));
					}
				}
			}
			line = end + 1;
		}
		return found;
	}

	namespace {

		// Where a marker starts, when the line is a comment and the marker is the
		// first thing in it.
		//
		// **The line has to begin with the comment**, so that this file's own
		// prose about `// arch-waiver` - and the tool's help text, which prints an
		// example - are not read as markers by a scan of the repository that
		// contains them.
		size_t MarkerAt(std::string_view line, std::string_view marker) {
			const size_t first = line.find_first_not_of(" \t");
			if (first == std::string_view::npos || line.compare(first, 2, "//") != 0) {
				return std::string_view::npos;
			}
			const size_t text = line.find_first_not_of(" \t", first + 2);
			if (text == std::string_view::npos || line.compare(text, marker.size(), marker) != 0) {
				return std::string_view::npos;
			}
			return text;
		}

		// One line of a file, by its one-based number.
		std::string_view LineText(std::string_view raw, size_t number) {
			size_t cursor = 0;
			for (size_t current = 1; cursor < raw.size(); current++) {
				size_t end = raw.find('\n', cursor);
				if (end == std::string_view::npos) {
					end = raw.size();
				}
				if (current == number) {
					return raw.substr(cursor, end - cursor);
				}
				cursor = end + 1;
			}
			return {};
		}

		// The one-based line of the first line at or after `from` that carries
		// something other than a comment or whitespace.
		size_t FirstCodeLine(std::string_view raw, size_t from) {
			size_t line = from;
			size_t number = 0;
			size_t cursor = 0;
			size_t current = 1;
			while (cursor < raw.size()) {
				size_t end = raw.find('\n', cursor);
				if (end == std::string_view::npos) {
					end = raw.size();
				}
				if (current > line) {
					const std::string_view text = raw.substr(cursor, end - cursor);
					const size_t first = text.find_first_not_of(" \t\r");
					if (first != std::string_view::npos && text.compare(first, 2, "//") != 0) {
						number = current;
						break;
					}
				}
				cursor = end + 1;
				current++;
			}
			return number;
		}
	}

	std::vector<Waiver> Waivers(std::string_view raw) {
		std::vector<Waiver> found;
		size_t cursor = 0;
		size_t current = 1;
		while (cursor < raw.size()) {
			size_t end = raw.find('\n', cursor);
			if (end == std::string_view::npos) {
				end = raw.size();
			}
			const std::string_view text = raw.substr(cursor, end - cursor);
			const size_t marker = MarkerAt(text, "arch-waiver ");
			if (marker != std::string_view::npos) {
				const std::string_view rest = text.substr(marker + 12);
				const size_t colon = rest.find(':');
				Waiver waiver;
				waiver.Line = current;
				if (colon == std::string_view::npos) {
					waiver.Rule = std::string(rest);
				} else {
					waiver.Rule = std::string(rest.substr(0, colon));
					std::string reason(rest.substr(colon + 1));
					const size_t first = reason.find_first_not_of(" \t\r");
					reason = (first == std::string::npos) ? std::string() : reason.substr(first);
					while (!reason.empty() && (reason.back() == ' ' || reason.back() == '\r')) {
						reason.pop_back();
					}
					waiver.Reason = reason;
				}
				while (!waiver.Rule.empty() && waiver.Rule.back() == ' ') {
					waiver.Rule.pop_back();
				}
				waiver.Covers = FirstCodeLine(raw, current);

				// A reason wraps, so the comment lines between the marker and the
				// declaration it covers are the rest of the same sentence. Reading only
				// the first line would print half a reason in a build log, which is the
				// half that reads like an excuse.
				for (size_t line = current + 1; line < waiver.Covers; line++) {
					const std::string_view more = LineText(raw, line);
					const size_t at = more.find("//");
					if (at == std::string_view::npos) {
						continue;
					}
					const size_t start = more.find_first_not_of(" \t", at + 2);
					if (start == std::string_view::npos) {
						continue;
					}
					std::string tail(more.substr(start));
					while (!tail.empty() && (tail.back() == ' ' || tail.back() == '\r')) {
						tail.pop_back();
					}
					if (waiver.Reason.empty()) {
						waiver.Reason = tail;
					} else if (!tail.empty()) {
						waiver.Reason += " " + tail;
					}
				}

				found.push_back(waiver);
			}
			cursor = end + 1;
			current++;
		}
		return found;
	}

	std::vector<size_t> Crossings(std::string_view raw) {
		std::vector<size_t> found;
		size_t cursor = 0;
		size_t current = 1;
		while (cursor < raw.size()) {
			size_t end = raw.find('\n', cursor);
			if (end == std::string_view::npos) {
				end = raw.size();
			}
			const std::string_view text = raw.substr(cursor, end - cursor);
			if (MarkerAt(text, "arch-crossing") != std::string_view::npos) {
				const size_t covers = FirstCodeLine(raw, current);
				if (covers != 0) {
					found.push_back(covers);
				}
			}
			cursor = end + 1;
			current++;
		}
		return found;
	}

	File Parse(std::string_view path, std::string_view text) {
		File file;
		file.Path = std::string(path);
		file.Raw = std::string(text);
		file.Stripped = Strip(text);
		file.Records = Records(file.Stripped);
		file.Enums = Enums(file.Stripped);
		file.Includes = Includes(file.Raw);
		file.Waivers = Waivers(file.Raw);
		file.Crossings = Crossings(file.Raw);
		return file;
	}

	namespace {

		bool SkippedDirectory(const std::string &name) {
			// `fixtures` is this tool's own negative test data. Scanning the
			// repository that holds it would otherwise report every deliberate
			// violation in it, and the tool would fail on its own suite.
			return name == "mono.vendor" || name == "node_modules" || name == "fixtures" ||
				   name.starts_with(".");
		}

		std::string Normalise(const fs::path &path) {
			std::string text = path.generic_string();
			return text;
		}
	}

	Tree Scan(const fs::path &root) {
		Tree tree;
		std::vector<fs::path> paths;

		std::error_code error;
		fs::recursive_directory_iterator walk(root, fs::directory_options::skip_permission_denied, error);
		if (error) {
			return tree;
		}
		for (auto entry = walk; entry != fs::recursive_directory_iterator(); entry.increment(error)) {
			if (error) {
				break;
			}
			if (entry->is_directory(error)) {
				if (SkippedDirectory(entry->path().filename().string())) {
					entry.disable_recursion_pending();
				}
				continue;
			}
			const std::string extension = entry->path().extension().string();
			if (extension != ".hpp" && extension != ".cpp" && extension != ".h" && extension != ".inl") {
				continue;
			}
			paths.push_back(entry->path());
		}
		std::sort(paths.begin(), paths.end());

		for (const fs::path &path : paths) {
			std::ifstream stream(path, std::ios::binary);
			if (!stream) {
				continue;
			}
			std::ostringstream buffer;
			buffer << stream.rdbuf();

			const std::string relative = Normalise(fs::relative(path, root));
			File file = Parse(relative, buffer.str());

			// A module is a directory with an `include/` in it. Walking up from
			// the file finds it without a list of module names to maintain.
			fs::path directory = path.parent_path();
			while (directory.string().size() >= root.string().size()) {
				if (fs::is_directory(directory / "include")) {
					file.Module = directory.filename().string();
					file.ModuleDir = Normalise(fs::relative(directory, root));
					const std::string prefix = file.ModuleDir + "/include/";
					if (file.Path.starts_with(prefix)) {
						file.IncludePath = file.Path.substr(prefix.size());
					}
					break;
				}
				if (!directory.has_parent_path() || directory.parent_path() == directory) {
					break;
				}
				directory = directory.parent_path();
			}

			file.Test = file.Path.find("/tests/") != std::string::npos ||
						file.Path.find("/benchmarks/") != std::string::npos ||
						file.Path.starts_with("tests/") || file.Path.starts_with("benchmarks/");

			tree.Files.push_back(std::move(file));
		}

		return tree;
	}
}
