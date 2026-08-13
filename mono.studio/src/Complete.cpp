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
			// The receiver does not matter — `part:IsA` and `thing:IsA` are the
			// same question — so only the last segment is compared.
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

		// The class a local was given on the line that declared it.
		//
		// **One shape and only one: an `Instance.new` with a literal.** That is
		// where the class is actually written down, so resolving it is reading
		// rather than inferring — and stopping there is what keeps this from
		// being a type checker that is wrong in ways nobody can predict. The
		// last declaration before the caret wins, because that is the one in
		// scope.
		Name DeclaredClassOf(const std::string_view text, const size_t caret, const std::string_view local) {
			if (local.empty()) {
				return {};
			}

			Name found;
			size_t at = 0;

			while (at < caret) {
				const size_t hit = text.find(local, at);
				if (hit == std::string_view::npos || hit >= caret) {
					break;
				}
				at = hit + local.size();

				// A whole word, so `part` does not match inside `parts`.
				const bool wordStart = hit == 0 || !IsWordCharacter(text[hit - 1]);
				const bool wordEnd = at >= text.size() || !IsWordCharacter(text[at]);
				if (!wordStart || !wordEnd) {
					continue;
				}

				const size_t assign = text.find('=', at);
				if (assign == std::string_view::npos || assign >= caret) {
					continue;
				}

				// Nothing but spaces between the name and the `=`, so
				// `local a, b = ...` does not read as a declaration of `a`.
				if (text.substr(at, assign - at).find_first_not_of(" \t") != std::string_view::npos) {
					continue;
				}

				const size_t call = text.find("Instance.new(", assign);
				if (call == std::string_view::npos || call >= caret) {
					continue;
				}

				const size_t open = text.find_first_of("\"'", call);
				if (open == std::string_view::npos || open >= caret) {
					continue;
				}
				const size_t close = text.find(text[open], open + 1);
				if (close == std::string_view::npos) {
					continue;
				}

				const std::string_view klass = text.substr(open + 1, close - open - 1);
				if (const Name candidate(klass); Classes::Find(candidate).IsValid()) {
					found = candidate;
				}
			}

			return found;
		}

		// Every scriptable property of one class, or of every class when none
		// is known.
		//
		// **`ClassInfo::Properties` is already the inherited span**, so a class
		// that is known needs no ancestry walk — which is the one place this
		// file does less work than `mono.tools/bindings`, whose declaration
		// files have to strip inheritance back out to write `extends`.
		void OfferProperties(std::vector<Completion> &into, const std::string_view prefix, const Name klass) {
			std::unordered_set<std::string_view> seen;

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
					Offer(
						into,
						prefix,
						property.Name.Text(),
						engine::ecs::Describe(property.Type),
						CompletionKind::Property
					);
				}
			};

			if (klass.IsValid()) {
				if (const ClassId id = Classes::Find(klass); id.IsValid()) {
					offer(Classes::Describe(id));
					return;
				}
			}

			// Nothing known about the subject, so everything a script could
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

		// A chain that ends in a separator has no subject worth resolving —
		// `a..b` and `.foo` both land here — and answering an empty one lets
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
			// offering one is offering a second that nothing resolves — and
			// asking `IsA` is what keeps a tenth service out of this function.
			if (serviceClass.IsValid() && Classes::IsA(id, serviceClass)) {
				continue;
			}

			// The abstract bases. Roblox does not let you insert an `Instance`
			// or a `BasePart` either, and the run time *would* mint one —
			// `Instances.cpp` looks the name up and takes whatever it finds — so
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
			// are exactly what they are for — `part:IsA("BasePart")` is the
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
						OfferProperties(offered, query.Prefix, DeclaredClassOf(text, caret, subject));
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
		// tables happened to be walked in — so two runs of the same editor
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

}
