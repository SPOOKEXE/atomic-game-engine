#include <engine/ecs/EnumTable.hpp>
#include <engine/script/Vocabulary.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <studio/Complete.hpp>
#include <studio/Widgets.hpp>
#include <unordered_set>

namespace studio {

	using engine::core::Name;
	using engine::ecs::Classes;
	using engine::ecs::ClassId;
	using engine::ecs::ClassInfo;
	using engine::ecs::EnumTable;
	using engine::ecs::PropertyDescriptor;
	using engine::script::Language;
	using engine::script::NameKind;
	using engine::script::ScriptSurface;
	using engine::script::VocabularyEntry;

	namespace {

		// What may appear in an identifier in either language. `$` is
		// JavaScript's alone but costs nothing to accept in Luau: this decides
		// where a word starts, not whether it is legal.
		bool IsWordCharacter(const char character) {
			const auto value = static_cast<unsigned char>(character);
			return std::isalnum(value) != 0 || character == '_' || character == '$';
		}

		// The calls whose first string argument is a class name.
		//
		// **Named rather than inferred**, because there is no way to tell a
		// string that wants a class from any other string, and offering eighty
		// class names inside every quoted literal would make the popup useless
		// exactly where somebody is typing prose.
		bool WantsClassName(std::string_view call) {
			// The receiver does not matter - `part:IsA` and `thing:IsA` are the
			// same question - so only the last segment is compared.
			if (const size_t separator = call.find_last_of(".:"); separator != std::string_view::npos) {
				call.remove_prefix(separator + 1);
			}

			return call == "new" || call == "GetService" || call == "IsA" ||
				   call == "FindFirstChildOfClass" || call == "FindFirstChildWhichIsA" ||
				   call == "FindFirstAncestorOfClass" || call == "FindFirstAncestorWhichIsA";
		}

		// Whether the caret sits inside a string literal, and where that
		// literal began.
		//
		// **Counted from the start of the line rather than the start of the
		// file.** A quote count over a whole script is wrong the moment
		// anything contains an apostrophe in a comment, and being wrong there
		// would silently switch completion off for everything below it.
		bool InsideString(std::string_view text, const size_t caret, size_t &openedAt) {
			size_t lineStart = text.rfind('\n', caret == 0 ? 0 : caret - 1);
			lineStart = lineStart == std::string_view::npos ? 0 : lineStart + 1;

			bool inside = false;
			char quote = '\0';

			for (size_t at = lineStart; at < caret; at++) {
				const char character = text[at];

				// A quote after a backslash is content. Two backslashes are an
				// escaped backslash and leave the next quote real, which is why
				// this counts them rather than looking at one.
				if (inside && character == '\\') {
					size_t slashes = 0;
					while (at + slashes + 1 < caret && text[at + slashes + 1] == '\\') {
						slashes++;
					}
					at += slashes + 1;
					continue;
				}

				if (character != '"' && character != '\'') {
					continue;
				}

				if (!inside) {
					inside = true;
					quote = character;
					openedAt = at;
				} else if (character == quote) {
					inside = false;
				}
			}

			return inside;
		}

		// Walks left over a dotted chain of identifiers, answering where it
		// began. `workspace.Model.Part` from its end gives its start.
		size_t ChainStart(std::string_view text, size_t at) {
			while (at > 0) {
				const char character = text[at - 1];
				if (IsWordCharacter(character) || character == '.' || character == ':') {
					at--;
					continue;
				}
				break;
			}
			return at;
		}

		// Adds one entry when it matches, scored the way every other ranked
		// list in this editor is scored.
		void Offer(
			std::vector<Completion> &into,
			const std::string_view prefix,
			const std::string_view text,
			const std::string_view detail,
			const CompletionKind kind
		) {
			int score = 0;
			if (!FuzzyMatch(prefix, text, score)) {
				return;
			}

			Completion entry;
			entry.Text.assign(text);
			entry.Detail.assign(detail);
			entry.Kind = kind;
			entry.Score = score;
			into.push_back(std::move(entry));
		}

		// How many assignments an alias or a clone is followed through. A chain
		// longer than this belongs to a generated file rather than to somebody
		// typing, and following one has to stop somewhere.
		constexpr int ASSIGNMENT_DEPTH = 8;

		std::string_view Trim(std::string_view value) {
			const size_t first = value.find_first_not_of(" \t\r");
			if (first == std::string_view::npos) {
				return {};
			}
			return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
		}

		// A name, with no separator and no digit in front of it.
		bool IsIdentifier(const std::string_view value) {
			if (value.empty() || std::isdigit(static_cast<unsigned char>(value.front())) != 0) {
				return false;
			}
			return std::all_of(value.begin(), value.end(), IsWordCharacter);
		}

		// A dotted chain and nothing else - `part`, `game:GetService`,
		// `Instance.new`. A callee that is anything more than this is one this
		// file cannot read, which is the answer rather than a problem.
		bool IsChain(const std::string_view value) {
			if (value.empty()) {
				return false;
			}
			return std::all_of(value.begin(), value.end(), [](const char character) {
				return IsWordCharacter(character) || character == '.' || character == ':';
			});
		}

		// The class named by a string literal, when that name is one the class
		// table really has.
		//
		// **`Name::Exists` before `Name`**, because constructing one interns it
		// - and this runs against whatever is inside the quotes on every
		// keystroke, most of which is prose.
		Name ClassNamed(const std::string_view literal) {
			if (literal.empty() || !Name::Exists(literal)) {
				return {};
			}
			const Name candidate(literal);
			return Classes::Find(candidate).IsValid() ? candidate : Name{};
		}

		// The first argument of a call, when it is a string literal.
		// `GetService("Workspace")` answers `Workspace`; `print(x, "hi")`
		// answers nothing, because the literal is not what the call was given
		// first.
		std::string_view FirstStringArgument(const std::string_view arguments) {
			const std::string_view value = Trim(arguments);
			if (value.size() < 2 || (value.front() != '"' && value.front() != '\'')) {
				return {};
			}
			const size_t close = value.find(value.front(), 1);
			return close == std::string_view::npos ? std::string_view{} : value.substr(1, close - 1);
		}

		// An expression with any trailing line comment cut off. `--` is Luau's
		// and `//` is JavaScript's, and neither counts inside a string - an
		// asset id in a literal would otherwise truncate the line it is on.
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

		Name ClassOfLocal(std::string_view text, size_t before, std::string_view local, int depth);

		// The class an expression evaluates to, when the text says so outright.
		//
		// **Three shapes, and the boundary between them is the whole point.** A
		// class written as a literal - `Instance.new("Part")`,
		// `game:GetService("Lighting")`, `FindFirstChildOfClass("Part")` - is
		// read. A `:Clone()` carries its receiver's class, because a clone of a
		// `Part` is a `Part` whatever else the file does. A name standing in for
		// another name is followed. **Everything else answers nothing**, which
		// is the union: `FindFirstChild` and `WaitForChild` return a *child*,
		// and a child of a `Model` is not a `Model`, so narrowing to the
		// receiver there would be a guess wearing a fact's clothes.
		Name ClassOfExpression(
			const std::string_view text,
			const size_t before,
			const std::string_view expression,
			const int depth
		) {
			if (depth <= 0) {
				return {};
			}

			std::string_view value = Trim(WithoutComment(expression));

			// JavaScript's statement terminator, which is the only difference
			// between the two languages this function can see.
			while (!value.empty() && value.back() == ';') {
				value = Trim(value.substr(0, value.size() - 1));
			}

			if (value.empty()) {
				return {};
			}

			if (IsIdentifier(value)) {
				return ClassOfLocal(text, before, value, depth - 1);
			}

			if (value.back() != ')') {
				return {};
			}

			// Back to the parenthesis this one closes, so `f(g())` is read as
			// one call rather than as `g` with something in front of it.
			size_t open = std::string_view::npos;
			int nesting = 0;
			for (size_t at = value.size(); at > 0; at--) {
				const char character = value[at - 1];
				if (character == ')') {
					nesting++;
				} else if (character == '(') {
					nesting--;
					if (nesting == 0) {
						open = at - 1;
						break;
					}
				}
			}
			if (open == std::string_view::npos) {
				return {};
			}

			const std::string_view callee = Trim(value.substr(0, open));
			const std::string_view arguments = value.substr(open + 1, value.size() - open - 2);
			if (!IsChain(callee)) {
				return {};
			}

			// **Both accessors, because both languages are written here.**
			// `part:Clone()` and `part.Clone()` are the same call, and which one
			// an author wrote says nothing about what it returns.
			const size_t separator = callee.find_last_of(".:");
			const std::string_view method =
				separator == std::string_view::npos ? callee : callee.substr(separator + 1);
			const std::string_view receiver =
				separator == std::string_view::npos ? std::string_view{} : callee.substr(0, separator);

			if (WantsClassName(callee)) {
				return ClassNamed(FirstStringArgument(arguments));
			}

			if (method == "Clone" && Trim(arguments).empty() && IsIdentifier(receiver)) {
				return ClassOfLocal(text, before, receiver, depth - 1);
			}

			return {};
		}

		// The class a local holds, read off the last assignment to it.
		//
		// **The last one and only the last one.** An earlier assignment that
		// *can* be read has stopped being a fact about the local the moment
		// something else was put in it, so a local set from `Instance.new` and
		// then from a call this file cannot read falls back to the union rather
		// than keeping the class it used to have.
		Name ClassOfLocal(
			const std::string_view text, const size_t before, const std::string_view local, const int depth
		) {
			if (depth <= 0 || !IsIdentifier(local)) {
				return {};
			}

			size_t assignedAt = std::string_view::npos;
			std::string_view initializer;
			size_t at = 0;

			while (at < before) {
				const size_t hit = text.find(local, at);
				if (hit == std::string_view::npos || hit >= before) {
					break;
				}
				at = hit + local.size();

				// A whole word, so `part` does not match inside `parts` - and
				// not a member, so `model.part = x` is not an assignment to a
				// local called `part`.
				const bool wordStart = hit == 0 || (!IsWordCharacter(text[hit - 1]) && text[hit - 1] != '.' &&
													text[hit - 1] != ':');
				const bool wordEnd = at >= text.size() || !IsWordCharacter(text[at]);
				if (!wordStart || !wordEnd) {
					continue;
				}

				// Nothing but spaces between the name and the `=`, so
				// `local a, b = ...` does not read as a declaration of `a` and
				// `count += 1` does not read as one at all.
				size_t equals = at;
				while (equals < text.size() && (text[equals] == ' ' || text[equals] == '\t')) {
					equals++;
				}
				if (equals >= before || text[equals] != '=') {
					continue;
				}

				// `p == q` asks about the local rather than answering for it.
				if (equals + 1 < text.size() && text[equals + 1] == '=') {
					continue;
				}

				// One line, because a continuation is a shape this file does not
				// read and half of one would resolve to something arbitrary.
				const size_t newline = text.find('\n', equals);
				const size_t lineEnd =
					std::min(newline == std::string_view::npos ? text.size() : newline, before);

				assignedAt = hit;
				initializer = text.substr(equals + 1, lineEnd - equals - 1);
			}

			if (assignedAt == std::string_view::npos) {
				return {};
			}

			// Resolved as of the assignment rather than as of the caret, which
			// is both the scope an author means and what makes `a = b` and
			// `b = a` in one file terminate: each hop moves strictly earlier.
			return ClassOfExpression(text, assignedAt, initializer, depth);
		}

		// The class the buffer says a subject holds, or an invalid name when
		// nothing in it says.
		Name
		NarrowedClassOf(const std::string_view text, const size_t caret, const std::string_view subject) {
			// A chain is a member of something, and this file resolves locals.
			// `script.Parent` is not a local and guessing at it would be the one
			// thing this must not do.
			if (subject.empty() || subject.find_first_of(".:") != std::string_view::npos) {
				return {};
			}
			return ClassOfLocal(text, caret, subject, ASSIGNMENT_DEPTH);
		}

		// Every scriptable property of one class, or of every class when none
		// is known.
		//
		// **Each row names whose property it is, and that is load-bearing.** A
		// narrowed row reads `bool on Part` and a union row reads
		// `bool on some class` - the difference between "this class has this"
		// and "one of these classes has this". `Complete.hpp` carries the
		// argument for why the marker exists at all.
		//
		// **`ClassInfo::Properties` is already the inherited span**, so a class
		// that is known needs no ancestry walk - which is the one place this
		// file does less work than `mono.tools/bindings`, whose declaration
		// files have to strip inheritance back out to write `extends`.
		void OfferProperties(std::vector<Completion> &into, const std::string_view prefix, const Name klass) {
			std::unordered_set<std::string_view> seen;

			const ClassId narrowed = klass.IsValid() ? Classes::Find(klass) : ClassId{};
			const std::string owner =
				narrowed.IsValid() ? std::string(klass.Text()) : std::string("some class");

			const auto offer = [&](const ClassInfo &info) {
				for (const PropertyDescriptor &property : info.Properties) {
					// **A property a script may not touch is not offered.** The
					// bindings refuse to declare one for the reason that applies
					// here twice over: a name in a list that then does not exist
					// is worse than a name nobody was told about.
					if (!property.Scriptable || !property.Name.IsValid()) {
						continue;
					}
					if (!seen.insert(property.Name.Text()).second) {
						continue;
					}
					const std::string detail =
						std::string(engine::ecs::Describe(property.Type)) + " on " + owner;
					Offer(into, prefix, property.Name.Text(), detail, CompletionKind::Property);
				}
			};

			if (narrowed.IsValid()) {
				offer(Classes::Describe(narrowed));
				return;
			}

			// Nothing the buffer can be read for, so everything a script could
			// write. A longer list, never a wrong one.
			for (size_t index = 0; index < Classes::Count(); index++) {
				offer(Classes::Describe(ClassId{static_cast<uint32_t>(index)}));
			}
		}

		void OfferInstanceMembers(
			std::vector<Completion> &into, const std::string_view prefix, const CompletionSources &sources
		) {
			if (sources.Surface == nullptr) {
				return;
			}
			for (const std::string &member : sources.Surface->InstanceMembers) {
				Offer(into, prefix, member, "", CompletionKind::Member);
			}
		}

		// The identifiers already written in this file.
		//
		// **Everything that looks like a word, minus what is already offered.**
		// A real scope analysis would need the grammar; a script's own names are
		// the ones an author most wants completed and they are all right there,
		// so the cheap version is most of the value. Duplicates are dropped and
		// the word under the caret is skipped, because offering somebody the
		// thing they are halfway through typing is noise.
		void OfferLocals(
			std::vector<Completion> &into, const std::string_view text, const CompletionQuery &query
		) {
			std::unordered_set<std::string_view> seen;
			seen.insert(query.Prefix);

			size_t at = 0;
			while (at < text.size()) {
				if (!IsWordCharacter(text[at]) || std::isdigit(static_cast<unsigned char>(text[at])) != 0) {
					at++;
					continue;
				}

				const size_t start = at;
				while (at < text.size() && IsWordCharacter(text[at])) {
					at++;
				}

				const std::string_view word = text.substr(start, at - start);
				if (word.size() < 2 || !seen.insert(word).second) {
					continue;
				}
				Offer(into, query.Prefix, word, "in this file", CompletionKind::Local);
			}
		}

	}

	CompletionQuery ScanBackwards(const std::string_view text, size_t caret) {
		caret = std::min(caret, text.size());

		CompletionQuery query;

		size_t openedAt = 0;
		if (InsideString(text, caret, openedAt)) {
			query.InString = true;
			query.Prefix = text.substr(openedAt + 1, caret - openedAt - 1);

			// Back past the quote and one `(` to the name being called.
			size_t before = openedAt;
			while (before > 0 && (text[before - 1] == ' ' || text[before - 1] == '\t')) {
				before--;
			}
			if (before > 0 && text[before - 1] == '(') {
				before--;
				const size_t start = ChainStart(text, before);
				query.Call = text.substr(start, before - start);
			}
			return query;
		}

		const size_t wordStart = [&] {
			size_t at = caret;
			while (at > 0 && IsWordCharacter(text[at - 1])) {
				at--;
			}
			return at;
		}();

		query.Prefix = text.substr(wordStart, caret - wordStart);

		if (wordStart == 0) {
			return query;
		}

		const char separator = text[wordStart - 1];
		if (separator != '.' && separator != ':') {
			return query;
		}

		query.Separator = separator;

		const size_t subjectStart = ChainStart(text, wordStart - 1);
		query.Subject = text.substr(subjectStart, wordStart - 1 - subjectStart);

		// A chain that ends in a separator has no subject worth resolving -
		// `a..b` and `.foo` both land here - and answering an empty one lets
		// the caller fall back to the union rather than look up "".
		if (!query.Subject.empty() && (query.Subject.back() == '.' || query.Subject.back() == ':')) {
			query.Subject = {};
		}

		return query;
	}

	std::vector<engine::ecs::ClassId> InsertableClasses() {
		std::vector<ClassId> ids;

		const ClassId instanceClass = Classes::Find(Name("Instance"));
		const ClassId serviceClass = Classes::Find(Name("Service"));

		for (size_t index = 0; index < Classes::Count(); index++) {
			const ClassId id{static_cast<uint32_t>(index)};
			const ClassInfo &info = Classes::Describe(id);

			if (!info.Name.IsValid()) {
				continue;
			}

			// Everything under `Instance`, which is every class an author can
			// put in a tree. A class registered by some other module for its own
			// storage is not one of those.
			if (instanceClass.IsValid() && !Classes::IsA(id, instanceClass)) {
				continue;
			}

			// **A category, not nine names.** A world has exactly one of each
			// service and `scene::InstallServices` is what puts it there, so
			// offering one is offering a second that nothing resolves - and
			// asking `IsA` is what keeps a tenth service out of this function.
			if (serviceClass.IsValid() && Classes::IsA(id, serviceClass)) {
				continue;
			}

			// The abstract bases. Roblox does not let you insert an `Instance`
			// or a `BasePart` either, and the run time *would* mint one -
			// `LuauInstances.cpp` looks the name up and takes whatever it finds - so
			// this is the only place that refuses.
			const std::string_view name = info.Name.Text();
			if (name == "Instance" || name == "PVInstance" || name == "BasePart" ||
				name == "LuaSourceContainer") {
				continue;
			}

			ids.push_back(id);
		}

		return ids;
	}

	std::vector<Completion> CompleteAt(
		const std::string_view text, const size_t caret, const CompletionSources &sources, const size_t limit
	) {
		const CompletionQuery query = ScanBackwards(text, caret);
		std::vector<Completion> offered;

		if (query.InString) {
			// Only where a class name is what the string is for. Everywhere
			// else a quote is prose, a path or an asset id, and a list of
			// classes over it would be noise in the one place somebody is
			// certainly not writing one.
			if (!WantsClassName(query.Call)) {
				return {};
			}

			// `IsA` and the `WhichIsA` pair take a base, so the abstract ones
			// are exactly what they are for - `part:IsA("BasePart")` is the
			// question the class tree exists to answer.
			const bool bases = query.Call.ends_with("IsA");

			if (bases) {
				for (size_t index = 0; index < Classes::Count(); index++) {
					const ClassInfo &info = Classes::Describe(ClassId{static_cast<uint32_t>(index)});
					if (info.Name.IsValid()) {
						Offer(offered, query.Prefix, info.Name.Text(), "class", CompletionKind::Class);
					}
				}
			} else {
				for (const ClassId id : InsertableClasses()) {
					const ClassInfo &info = Classes::Describe(id);
					const std::string_view parent =
						info.Parent.IsValid() ? Classes::Describe(info.Parent).Name.Text() : "";
					Offer(offered, query.Prefix, info.Name.Text(), parent, CompletionKind::Class);
				}
			}
		} else if (query.Separator != '\0') {
			const std::string_view subject = query.Subject;

			// `Enum` and `Enum.<Set>`, both read from the table the VM resolves
			// them against rather than from a copy.
			if (subject == "Enum") {
				for (const Name set : EnumTable::Names()) {
					Offer(offered, query.Prefix, set.Text(), "enum", CompletionKind::Enum);
				}
			} else if (subject.starts_with("Enum.") && subject.find('.', 5) == std::string_view::npos) {
				const Name set(subject.substr(5));
				for (const Name member : EnumTable::MembersOf(set)) {
					Offer(offered, query.Prefix, member.Text(), set.Text(), CompletionKind::Enum);
				}
			} else {
				// A global the VM installed, whose members the walk found.
				const VocabularyEntry *global = nullptr;
				if (sources.Surface != nullptr && subject.find_first_of(".:") == std::string_view::npos) {
					for (const VocabularyEntry &entry : sources.Surface->Globals) {
						if (entry.Name == subject) {
							global = &entry;
							break;
						}
					}
				}

				if (global != nullptr && !global->Members.empty()) {
					for (const std::string &member : global->Members) {
						Offer(offered, query.Prefix, member, global->Name, CompletionKind::Global);
					}
				}

				// **Methods after a colon, properties after a dot, and both
				// after a dot in JavaScript.** Luau's colon passes the instance
				// and its dot does not, so offering a method after a dot would
				// be offering a call missing its first argument. JavaScript has
				// one accessor and the distinction does not exist.
				const bool methods = query.Separator == ':' || sources.Language == Language::JavaScript;
				const bool properties = query.Separator == '.';

				if (global == nullptr || global->Members.empty()) {
					if (methods) {
						OfferInstanceMembers(offered, query.Prefix, sources);
					}
					if (properties) {
						OfferProperties(offered, query.Prefix, NarrowedClassOf(text, caret, subject));
					}
				}

				// The tree beside this script, which is the one thing an
				// external language server cannot know.
				if (properties) {
					for (const std::string &child : sources.Children) {
						Offer(offered, query.Prefix, child, "in the tree", CompletionKind::Child);
					}
				}
			}
		} else {
			if (sources.Surface != nullptr) {
				const std::span<const std::string_view> withheld = engine::script::Withheld(sources.Language);

				for (const VocabularyEntry &entry : sources.Surface->Globals) {
					if (std::find(withheld.begin(), withheld.end(), entry.Name) != withheld.end()) {
						continue;
					}
					Offer(
						offered,
						query.Prefix,
						entry.Name,
						entry.Kind == NameKind::Function ? "function" : "",
						CompletionKind::Global
					);
				}
			}

			for (const std::string_view keyword : engine::script::Keywords(sources.Language)) {
				Offer(offered, query.Prefix, keyword, "keyword", CompletionKind::Keyword);
			}

			OfferLocals(offered, text, query);
		}

		// Best first, and ties broken by name rather than by the order the
		// tables happened to be walked in - so two runs of the same editor
		// against the same file offer the same list.
		std::sort(offered.begin(), offered.end(), [](const Completion &left, const Completion &right) {
			if (left.Score != right.Score) {
				return left.Score > right.Score;
			}
			return left.Text < right.Text;
		});

		if (offered.size() > limit) {
			offered.resize(limit);
		}
		return offered;
	}

	namespace {

		// One keyword and its sentence. The tables are checked against
		// `script::Keywords` by the suite, so a word missing from either side
		// is a test failure rather than a silent gap.
		struct KeywordNote {
			std::string_view Word;
			std::string_view Doc;
		};

		constexpr KeywordNote LUAU_KEYWORD_DOCS[] = {
			{"and", "answers the left value when it is false or nil, otherwise the right"},
			{"break", "leaves the innermost loop"},
			{"continue", "skips to the next pass of the innermost loop"},
			{"do", "opens a block with its own locals, closed by end"},
			{"else", "what runs when no condition above held"},
			{"elseif", "another condition, tried when the ones above were false"},
			{"end", "closes a function, if, for, while or do block"},
			{"export", "makes a type visible to files that require this one"},
			{"false", "the boolean false"},
			{"for", "a counted or iterator loop: for i = 1, 10 do, or for k, v in pairs(t) do"},
			{"function", "declares a function, closed by end"},
			{"if", "runs a block when a condition holds: if x then ... end"},
			{"in", "separates a for loop's names from what it iterates"},
			{"local", "declares a variable scoped to this block"},
			{"nil", "the absence of a value: a missing member reads as nil"},
			{"not", "answers true for false and nil, false for everything else"},
			{"or", "answers the left value unless it is false or nil, then the right"},
			{"repeat", "a loop that runs at least once, closed by until"},
			{"return", "hands values back to the caller and ends the function"},
			{"then", "separates an if or elseif condition from its block"},
			{"true", "the boolean true"},
			{"type", "declares a type alias: type Point = { x: number, y: number }"},
			{"until", "closes a repeat loop: it runs again while the condition is false"},
			{"while", "loops while a condition holds: while x do ... end"},
		};

		constexpr KeywordNote JAVASCRIPT_KEYWORD_DOCS[] = {
			{"async", "marks a function that returns a promise and may await"},
			{"await", "waits for a promise, inside an async function"},
			{"break", "leaves the innermost loop or switch"},
			{"case", "one branch of a switch"},
			{"catch", "handles whatever a try block threw"},
			{"class", "declares a class"},
			{"const", "declares a binding that cannot be reassigned"},
			{"continue", "skips to the next pass of the innermost loop"},
			{"debugger", "a breakpoint statement: does nothing without a debugger attached"},
			{"default", "the switch branch taken when no case matched"},
			{"delete", "removes a property from an object"},
			{"do", "a loop that runs at least once, closed by while"},
			{"else", "what runs when the if condition did not hold"},
			{"export", "makes a binding visible to importing modules"},
			{"extends", "names a class's parent"},
			{"false", "the boolean false"},
			{"finally", "runs after a try block whether it threw or not"},
			{"for", "a loop: for (;;), for..of over values, for..in over keys"},
			{"function", "declares a function"},
			{"if", "runs a block when a condition holds"},
			{"import", "brings another module's exports into scope"},
			{"in", "whether an object has a property, or a for..in loop over keys"},
			{"instanceof", "whether a value was built by a constructor"},
			{"let", "declares a block scoped variable"},
			{"new", "constructs an instance: new Thing()"},
			{"null", "the deliberate absence of a value"},
			{"of", "a for..of loop over values"},
			{"return", "hands a value back to the caller and ends the function"},
			{"static", "a class member on the class rather than on instances"},
			{"super", "the parent class's constructor or members"},
			{"switch", "branches on one value across cases"},
			{"this", "the receiver the function was called on"},
			{"throw", "raises a value as an error"},
			{"true", "the boolean true"},
			{"try", "runs a block and hands what it throws to catch"},
			{"typeof", "a value's type name, as a string"},
			{"var", "declares a function scoped variable: prefer let or const"},
			{"void", "evaluates an expression and answers undefined"},
			{"while", "loops while a condition holds"},
			{"yield", "pauses a generator and hands a value out"},
		};

		// Whether a byte sits at or past a line comment on its own line.
		// `WithoutComment` already knows both languages' markers and what a
		// string does to them; this only asks where the line it kept ends.
		bool InsideComment(const std::string_view text, const size_t offset) {
			size_t lineStart = text.rfind('\n', offset == 0 ? 0 : offset - 1);
			lineStart = (lineStart == std::string_view::npos || offset == 0) ? 0 : lineStart + 1;

			const size_t newline = text.find('\n', lineStart);
			const std::string_view line = text.substr(
				lineStart, (newline == std::string_view::npos ? text.size() : newline) - lineStart
			);

			return offset - lineStart >= WithoutComment(line).size();
		}

	}

	std::string_view Describe(const CompletionKind kind) {
		switch (kind) {
		case CompletionKind::Class:
			return "class";
		case CompletionKind::Property:
			return "property";
		case CompletionKind::Member:
			return "method or signal on every instance";
		case CompletionKind::Enum:
			return "enum";
		case CompletionKind::Global:
			return "global";
		case CompletionKind::Keyword:
			return "keyword";
		case CompletionKind::Local:
			return "in this file";
		case CompletionKind::Child:
			return "in the tree beside this script";
		}
		return "completion";
	}

	std::string_view KeywordDoc(const Language language, const std::string_view word) {
		const std::span<const KeywordNote> notes =
			language == Language::Luau ? std::span<const KeywordNote>(LUAU_KEYWORD_DOCS)
									   : std::span<const KeywordNote>(JAVASCRIPT_KEYWORD_DOCS);

		for (const KeywordNote &note : notes) {
			if (note.Word == word) {
				return note.Doc;
			}
		}
		return {};
	}

	std::string_view WordAt(const std::string_view text, const size_t offset) {
		if (offset >= text.size() || !IsWordCharacter(text[offset])) {
			return {};
		}

		size_t start = offset;
		while (start > 0 && IsWordCharacter(text[start - 1])) {
			start--;
		}
		size_t end = offset;
		while (end < text.size() && IsWordCharacter(text[end])) {
			end++;
		}
		return text.substr(start, end - start);
	}

	std::string
	HoverText(const std::string_view text, const size_t offset, const CompletionSources &sources) {
		const std::string_view word = WordAt(text, offset);

		// A bare number is a number: there is nothing to say about `10`.
		if (word.empty() || std::isdigit(static_cast<unsigned char>(word.front())) != 0) {
			return {};
		}

		// A keyword quoted in a string or written in a comment is prose.
		size_t openedAt = 0;
		if (InsideString(text, offset, openedAt) || InsideComment(text, offset)) {
			return {};
		}

		for (const std::string_view keyword : engine::script::Keywords(sources.Language)) {
			if (keyword == word) {
				const std::string_view doc = KeywordDoc(sources.Language, word);
				return doc.empty() ? std::string("keyword") : std::string("keyword\n").append(doc);
			}
		}

		if (sources.Surface != nullptr) {
			for (const VocabularyEntry &entry : sources.Surface->Globals) {
				if (entry.Name != word) {
					continue;
				}
				if (entry.Kind == NameKind::Function) {
					return "global function";
				}
				if (entry.Kind == NameKind::Container && !entry.Members.empty()) {
					// A few members by name, so the tooltip says what a dot
					// after this would reach rather than only that one would.
					std::string described =
						"global with " + std::to_string(entry.Members.size()) + " member(s): ";
					constexpr size_t NAMED = 4;
					for (size_t index = 0; index < std::min(entry.Members.size(), NAMED); index++) {
						described += index == 0 ? "" : ", ";
						described += entry.Members[index];
					}
					if (entry.Members.size() > NAMED) {
						described += ", ...";
					}
					return described;
				}
				return "global";
			}

			for (const std::string &member : sources.Surface->InstanceMembers) {
				if (member == word) {
					return std::string(Describe(CompletionKind::Member));
				}
			}
		}

		for (const Name set : EnumTable::Names()) {
			if (set.Text() == word) {
				return "enum set with " + std::to_string(EnumTable::MembersOf(set).size()) +
					   " member(s): Enum." + std::string(word);
			}
		}

		if (const Name klass = ClassNamed(word); klass.IsValid()) {
			const ClassInfo &info = Classes::Describe(Classes::Find(klass));
			if (info.Parent.IsValid()) {
				return "class, extends " + std::string(Classes::Describe(info.Parent).Name.Text());
			}
			return "class";
		}

		for (const std::string &child : sources.Children) {
			if (child == word) {
				return "an instance beside this script";
			}
		}

		// The same narrowing `CompleteAt` does after a dot, and only that:
		// a class the buffer states outright, never an inference.
		if (const Name held = ClassOfLocal(text, offset, word, ASSIGNMENT_DEPTH); held.IsValid()) {
			return "a " + std::string(held.Text()) + ", by its last assignment";
		}

		// A property, attributed to the classes that declare it - the ones
		// whose parent does not already carry it, because `ClassInfo::
		// Properties` is the inherited span and every descendant repeats it.
		Name declaringClass;
		size_t declarers = 0;
		std::string_view propertyType;
		bool oneType = true;

		for (size_t index = 0; index < Classes::Count(); index++) {
			const ClassId id{static_cast<uint32_t>(index)};
			const ClassInfo &info = Classes::Describe(id);

			for (const PropertyDescriptor &property : info.Properties) {
				if (!property.Scriptable || !property.Name.IsValid() || property.Name.Text() != word) {
					continue;
				}

				bool inherited = false;
				if (info.Parent.IsValid()) {
					for (const PropertyDescriptor &above : Classes::Describe(info.Parent).Properties) {
						if (above.Name == property.Name) {
							inherited = true;
							break;
						}
					}
				}
				if (inherited) {
					continue;
				}

				declarers++;
				declaringClass = info.Name;
				const std::string_view described = engine::ecs::Describe(property.Type);
				oneType = oneType && (propertyType.empty() || propertyType == described);
				propertyType = described;
			}
		}

		if (declarers == 1) {
			return std::string(propertyType) + " on " + std::string(declaringClass.Text());
		}
		if (declarers > 1) {
			return oneType ? std::string(propertyType) + " on " + std::to_string(declarers) + " classes"
						   : "a property on " + std::to_string(declarers) + " classes";
		}

		return {};
	}

}
