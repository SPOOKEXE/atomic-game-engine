#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <set>
#include <sourcecheck/Rules.hpp>
#include <sourcecheck/Source.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace sourcecheck {

	namespace {

		bool IsIdentifierChar(char c) {
			return (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '_';
		}

		std::string LastSegment(std::string_view qualified) {
			const size_t at = qualified.rfind("::");
			return std::string(at == std::string_view::npos ? qualified : qualified.substr(at + 2));
		}

		// One identifier inside a type, with whatever qualified it.
		struct Token {
			std::string Qualifier;
			std::string Name;
		};

		std::vector<Token> TypeTokens(std::string_view type) {
			std::vector<Token> tokens;
			size_t index = 0;
			while (index < type.size()) {
				if (!IsIdentifierChar(type[index])) {
					index++;
					continue;
				}
				const size_t start = index;
				while (index < type.size() && IsIdentifierChar(type[index])) {
					index++;
				}
				std::string name(type.substr(start, index - start));

				// Walk back over `A::B::` to see what qualified it.
				std::string qualifier;
				size_t back = start;
				while (back >= 2 && type[back - 1] == ':' && type[back - 2] == ':') {
					size_t word = back - 2;
					while (word > 0 && IsIdentifierChar(type[word - 1])) {
						word--;
					}
					if (word == back - 2) {
						break;
					}
					qualifier = std::string(type.substr(word, back - 2 - word)) +
								(qualifier.empty() ? "" : "::" + qualifier);
					back = word;
				}
				tokens.push_back({qualifier, name});
			}
			return tokens;
		}

		// A type declared somewhere, and how a reference to it is recognised.
		struct Declared {
			std::string Name;
			std::string Namespace;
			std::string Module;
			std::string Path;
			size_t Line = 0;
			// The registered component this one belongs to, for a companion enum.
			std::string Component;
		};

		// Whether `token` names `declared` from a file in `module`.
		//
		// A qualified reference matches on the last namespace segment, so
		// `scene::Camera` and `engine::scene::Camera` both find the same type. An
		// unqualified one matches only inside the declaring module, which is what
		// keeps the four unrelated structs named `Entry` in this repository apart.
		bool Names(const Token &token, const Declared &declared, std::string_view module) {
			if (token.Name != declared.Name) {
				return false;
			}
			if (!token.Qualifier.empty()) {
				return LastSegment(token.Qualifier) == LastSegment(declared.Namespace);
			}
			return module == declared.Module;
		}

		// Everything the rules index out of a tree once.
		struct Index {
			// Registered component and resource types.
			std::vector<Declared> Components;

			// Enumerations declared beside a component and used as one of its
			// fields.
			std::vector<Declared> Companions;

			// Every record in the tree, for the world-boundary walk.
			std::vector<Declared> AllRecords;

			// Parallel to `AllRecords`, so a walk can reach the members.
			std::vector<const Record *> Bodies;

			// Name to position in each of the three, so a member's type is looked up
			// rather than compared against every declaration in the tree. There are
			// twenty thousand records in this repository and every member of every one
			// of them asks the question.
			std::map<std::string, std::vector<size_t>> ComponentsByName;
			std::map<std::string, std::vector<size_t>> CompanionsByName;
			std::map<std::string, std::vector<size_t>> RecordsByName;
		};

		std::vector<std::string> RegisteredNames(std::string_view stripped) {
			std::vector<std::string> names;
			static constexpr std::string_view MARKER = "Components::Register<";
			size_t at = stripped.find(MARKER);
			while (at != std::string_view::npos) {
				size_t cursor = at + MARKER.size();
				while (cursor < stripped.size() &&
					   (std::isspace(static_cast<unsigned char>(stripped[cursor])) != 0)) {
					cursor++;
				}
				const size_t start = cursor;
				while (cursor < stripped.size() &&
					   (IsIdentifierChar(stripped[cursor]) || stripped[cursor] == ':')) {
					cursor++;
				}
				if (cursor > start) {
					names.push_back(LastSegment(stripped.substr(start, cursor - start)));
				}
				at = stripped.find(MARKER, at + MARKER.size());
			}
			return names;
		}

		Index Build(const Tree &tree) {
			Index index;

			for (const File &file : tree.Files) {
				if (file.Test) {
					continue;
				}
				for (const Record &record : file.Records) {
					index.AllRecords.push_back(
						{record.Name, record.Namespace, file.Module, file.Path, record.Line, ""}
					);
					index.Bodies.push_back(&record);
				}
			}

			// A registration names a type; the declaration for it is the record of
			// that name in the same module. A test's registrations are ignored,
			// because a suite invents component types on purpose.
			std::set<std::string> registered;
			std::map<std::string, std::set<std::string>> modules;
			for (const File &file : tree.Files) {
				if (file.Test) {
					continue;
				}
				for (const std::string &name : RegisteredNames(file.Stripped)) {
					registered.insert(name);
					modules[name].insert(file.Module);
				}
			}

			for (size_t at = 0; at < index.AllRecords.size(); at++) {
				const Declared &declared = index.AllRecords[at];
				if (!registered.contains(declared.Name)) {
					continue;
				}
				const auto found = modules.find(declared.Name);
				if (found == modules.end() || !found->second.contains(declared.Module)) {
					continue;
				}
				index.Components.push_back(declared);

				// An enumeration declared in the same file and used as one of this
				// component's fields is the component's own vocabulary. A copy of
				// it outside the module is a copy of the fact it encodes.
				const File *owner = nullptr;
				for (const File &file : tree.Files) {
					if (file.Path == declared.Path) {
						owner = &file;
						break;
					}
				}
				if (owner == nullptr) {
					continue;
				}
				for (const Member &member : index.Bodies[at]->Members) {
					for (const Token &token : TypeTokens(member.Type)) {
						for (const Enumeration &enumeration : owner->Enums) {
							if (enumeration.Name != token.Name) {
								continue;
							}
							index.Companions.push_back(
								{enumeration.Name,
								 enumeration.Namespace,
								 owner->Module,
								 owner->Path,
								 enumeration.Line,
								 declared.Name}
							);
						}
					}
				}
			}

			for (size_t at = 0; at < index.Components.size(); at++) {
				index.ComponentsByName[index.Components[at].Name].push_back(at);
			}
			for (size_t at = 0; at < index.Companions.size(); at++) {
				index.CompanionsByName[index.Companions[at].Name].push_back(at);
			}
			for (size_t at = 0; at < index.AllRecords.size(); at++) {
				index.RecordsByName[index.AllRecords[at].Name].push_back(at);
			}

			return index;
		}

		// The positions a name maps to, or nothing.
		const std::vector<size_t> &
		Positions(const std::map<std::string, std::vector<size_t>> &table, const std::string &name) {
			static const std::vector<size_t> NONE;
			const auto found = table.find(name);
			return (found == table.end()) ? NONE : found->second;
		}

		// The waiver covering a line, if there is one.
		const Waiver *WaiverFor(const File &file, std::string_view rule, size_t line) {
			for (const Waiver &waiver : file.Waivers) {
				if (waiver.Rule != rule) {
					continue;
				}
				if (waiver.Covers == line || waiver.Line == line) {
					return &waiver;
				}
			}
			return nullptr;
		}

		// The waiver covering a whole file, for a rule whose subject is the file.
		const Waiver *FileWaiverFor(const File &file, std::string_view rule) {
			for (const Waiver &waiver : file.Waivers) {
				if (waiver.Rule == rule) {
					return &waiver;
				}
			}
			return nullptr;
		}

		void Apply(Finding &finding, const Waiver *waiver) {
			if (waiver == nullptr) {
				return;
			}
			if (waiver->Reason.empty()) {
				finding.Message +=
					" The waiver on this line has no reason after the colon, so it is not honoured.";
				return;
			}
			finding.Reason = waiver->Reason;
			finding.Status = waiver->Reason.starts_with("known violation") ? State::Known : State::Waived;
		}
	}

	namespace {

		std::vector<Finding> EcsCopy(const Tree &tree, const Index &index);
		std::vector<Finding> WorldPointer(const Tree &tree, const Index &index);
	}

	// Each of these builds the index for itself; `Check` builds it once and runs
	// the four over it.
	std::vector<Finding> CheckEcsCopy(const Tree &tree) {
		return EcsCopy(tree, Build(tree));
	}

	std::vector<Finding> CheckWorldPointer(const Tree &tree) {
		return WorldPointer(tree, Build(tree));
	}

	namespace {

		std::vector<Finding> EcsCopy(const Tree &tree, const Index &index) {
			std::vector<Finding> findings;

			for (const File &file : tree.Files) {
				if (file.Test) {
					continue;
				}
				for (const Record &record : file.Records) {
					// An aggregate with no behaviour is an argument list. A camera
					// travelling to a draw call is a value, not a second authority.
					if (!record.HasBehaviour) {
						continue;
					}

					bool isComponent = false;
					for (const Declared &component : index.Components) {
						if (component.Name == record.Name && component.Path == file.Path) {
							isComponent = true;
						}
					}
					if (isComponent) {
						continue;
					}

					for (const Member &member : record.Members) {
						std::string message;
						for (const Token &token : TypeTokens(member.Type)) {
							for (const size_t at : Positions(index.ComponentsByName, token.Name)) {
								const Declared &component = index.Components[at];
								if (!Names(token, component, file.Module)) {
									continue;
								}
								message = record.Name + "::" + member.Name + " is a `" + member.Type +
										  "`. `" + component.Name +
										  "` is a registered component and the store owns it, so this is a "
										  "second copy of the same fact.";
								break;
							}
							if (!message.empty()) {
								break;
							}
							for (const size_t at : Positions(index.CompanionsByName, token.Name)) {
								const Declared &companion = index.Companions[at];
								if (companion.Module == file.Module ||
									!Names(token, companion, file.Module)) {
									continue;
								}
								message = record.Name + "::" + member.Name + " is a `" + member.Type +
										  "`, and `" + companion.Name +
										  "` is what the registered component `" + companion.Component +
										  "` declares its own field with. The store owns that state; read it "
										  "from there.";
								break;
							}
							if (!message.empty()) {
								break;
							}
						}
						if (message.empty()) {
							continue;
						}

						Finding finding;
						finding.Rule = "ecs-copy";
						finding.Path = file.Path;
						finding.Line = member.Line;
						finding.Message = message;
						Apply(finding, WaiverFor(file, "ecs-copy", member.Line));
						findings.push_back(finding);
					}
				}
			}

			return findings;
		}

		std::vector<Finding> WorldPointer(const Tree &tree, const Index &index) {
			std::vector<Finding> findings;

			static constexpr std::string_view INDIRECTIONS[] = {
				"unique_ptr",
				"shared_ptr",
				"weak_ptr",
				"span",
				"string_view",
				"function",
				"reference_wrapper",
				"any",
			};

			std::map<std::string, const File *> byPath;
			for (const File &file : tree.Files) {
				byPath[file.Path] = &file;
			}

			// Walks one crossing type's fields, and its fields' fields, naming the
			// path it took so a finding says where in the message the pointer is.
			//
			// A field's own file and line are reported rather than the crossing type's,
			// because the type that has to change is the one the pointer is in.
			const auto walk = [&](const File &file, const Record &seed) {
				struct Step {
					const Record *Body = nullptr;
					std::string Path;
					const File *Where = nullptr;
				};
				std::vector<Step> queue{{&seed, seed.Name, &file}};
				std::vector<std::string> visited{seed.Namespace + "::" + seed.Name};
				std::vector<Finding> found;

				for (size_t at = 0; at < queue.size() && at < 512; at++) {
					const Record *record = queue[at].Body;
					const std::string path = queue[at].Path;
					const File &where = *queue[at].Where;

					for (const Member &member : record->Members) {
						std::string reason;
						if (member.Type.find('*') != std::string::npos) {
							reason = "a pointer";
						} else if (member.Type.find('&') != std::string::npos) {
							reason = "a reference";
						} else {
							for (const std::string_view indirection : INDIRECTIONS) {
								if (member.Type.find(indirection) != std::string::npos) {
									reason = "a `std::" + std::string(indirection) +
											 "`, which is a pointer with a nicer name";
									break;
								}
							}
						}

						if (!reason.empty()) {
							Finding finding;
							finding.Rule = "world-pointer";
							finding.Path = where.Path;
							finding.Line = member.Line;
							finding.Message = path + "::" + member.Name + " is " + reason + ". `" +
											  seed.Name +
											  "` is marked as crossing a world boundary, and a crossing "
											  "carries a copy - one shared pointer added because it is only "
											  "threads today ends the process option permanently.";
							Apply(finding, WaiverFor(where, "world-pointer", finding.Line));
							found.push_back(finding);
							continue;
						}

						// Not an indirection, so follow it into whatever it is.
						for (const Token &token : TypeTokens(member.Type)) {
							for (const size_t which : Positions(index.RecordsByName, token.Name)) {
								const Declared &declared = index.AllRecords[which];
								if (!Names(token, declared, where.Module)) {
									continue;
								}
								const std::string key = declared.Namespace + "::" + declared.Name;
								if (std::find(visited.begin(), visited.end(), key) != visited.end()) {
									continue;
								}
								visited.push_back(key);
								const auto owner = byPath.find(declared.Path);
								queue.push_back(
									{index.Bodies[which],
									 path + "::" + member.Name,
									 (owner == byPath.end()) ? &where : owner->second}
								);
							}
						}
					}
				}
				return found;
			};

			for (const File &file : tree.Files) {
				if (file.Test) {
					continue;
				}
				for (const Record &record : file.Records) {
					if (std::find(file.Crossings.begin(), file.Crossings.end(), record.Line) ==
						file.Crossings.end()) {
						continue;
					}
					for (const Finding &finding : walk(file, record)) {
						findings.push_back(finding);
					}
				}
			}

			return findings;
		}
	}

	namespace {

		// The argument list of a call whose `(` is at `open`.
		std::string_view Arguments(std::string_view text, size_t open) {
			int depth = 0;
			for (size_t index = open; index < text.size(); index++) {
				if (text[index] == '(') {
					depth++;
				} else if (text[index] == ')') {
					depth--;
					if (depth == 0) {
						return text.substr(open + 1, index - open - 1);
					}
				}
			}
			return {};
		}

		bool SerialiserName(std::string_view name) {
			static constexpr std::string_view PREFIXES[] = {
				"Write",
				"Encode",
				"Serialise",
				"Serialize",
				"Emit",
				"Put",
			};
			for (const std::string_view prefix : PREFIXES) {
				if (name.starts_with(prefix)) {
					return true;
				}
			}
			return false;
		}

		bool MentionsIdCall(std::string_view arguments) {
			size_t at = arguments.find("Id");
			while (at != std::string_view::npos) {
				const bool member =
					at >= 1 && (arguments[at - 1] == '.' ||
								(at >= 2 && arguments[at - 2] == '-' && arguments[at - 1] == '>'));
				size_t after = at + 2;
				while (after < arguments.size() &&
					   (std::isspace(static_cast<unsigned char>(arguments[after])) != 0)) {
					after++;
				}
				const bool call =
					after + 1 < arguments.size() && arguments[after] == '(' && arguments[after + 1] == ')';
				if (member && call) {
					return true;
				}
				at = arguments.find("Id", at + 2);
			}
			return false;
		}

		size_t LineAt(std::string_view text, size_t offset) {
			return static_cast<size_t>(
					   std::count(text.begin(), text.begin() + static_cast<long>(offset), '\n')
				   ) +
				   1;
		}
	}

	std::vector<Finding> CheckNameId(const Tree &tree) {
		std::vector<Finding> findings;

		for (const File &file : tree.Files) {
			if (file.Test) {
				continue;
			}
			const std::string_view text = file.Stripped;

			for (size_t index = 0; index < text.size(); index++) {
				if (text[index] != '(' || index == 0 || !IsIdentifierChar(text[index - 1])) {
					continue;
				}
				size_t start = index;
				while (start > 0 && IsIdentifierChar(text[start - 1])) {
					start--;
				}
				const std::string_view name = text.substr(start, index - start);
				const std::string_view arguments = Arguments(text, index);

				std::string message;
				if (SerialiserName(name) && name != "WriteName" && MentionsIdCall(arguments)) {
					message = std::string(name) +
							  " is handed an `Id()`. Ids are assigned in first-seen order and mean nothing "
							  "in another process or another run: serialise `Text()`, or call `WriteName`.";
				} else if (SerialiserName(name) && arguments.find("sizeof") != std::string_view::npos &&
						   arguments.find("Name") != std::string_view::npos) {
					message = std::string(name) +
							  " writes a `Name`'s object representation. That is its process-local id with "
							  "extra steps - the failure `client::DrawList` had at v0.7. Write `Text()`.";
				} else if (name == "FromId" && arguments.find("Read") != std::string_view::npos) {
					message = "Name::FromId is fed straight from a reader. An id read back from a stream "
							  "names whatever happened to be interned first in this process: read the text "
							  "and intern it.";
				}
				if (message.empty()) {
					continue;
				}

				Finding finding;
				finding.Rule = "name-id";
				finding.Path = file.Path;
				finding.Line = LineAt(text, start);
				finding.Message = message;
				Apply(finding, WaiverFor(file, "name-id", finding.Line));
				findings.push_back(finding);
			}
		}

		return findings;
	}

	std::vector<Finding> CheckPublicHeader(const Tree &tree) {
		std::vector<Finding> findings;

		// Include target -> the module directory it belongs to.
		std::map<std::string, const File *> published;
		for (const File &file : tree.Files) {
			if (!file.IncludePath.empty()) {
				published[file.IncludePath] = &file;
			}
		}

		std::map<std::string, size_t> outside;
		for (const File &file : tree.Files) {
			for (const std::string &target : file.Includes) {
				const auto found = published.find(target);
				if (found == published.end()) {
					continue;
				}
				const File &header = *found->second;
				if (&file == &header) {
					continue;
				}
				// A program consumes its own surface from its `main.cpp`, so an
				// `app/` include counts the way another module's does.
				const bool sameModule = file.ModuleDir == header.ModuleDir;
				const bool fromApp = file.Path.starts_with(header.ModuleDir + "/app/");
				if (!sameModule || fromApp) {
					outside[target]++;
				}
			}
		}

		for (const auto &[target, header] : published) {
			if (outside[target] != 0) {
				continue;
			}
			Finding finding;
			finding.Rule = "public-header";
			finding.Path = header->Path;
			finding.Line = 1;
			finding.Message = "nothing outside " + header->ModuleDir + " includes <" + target +
							  ">. Either it belongs beside the sources that use it, or it is forward API and "
							  "should say so.";
			Apply(finding, FileWaiverFor(*header, "public-header"));
			findings.push_back(finding);
		}

		return findings;
	}

	std::vector<std::string> RuleNames() {
		return {"ecs-copy", "world-pointer", "name-id", "public-header"};
	}

	bool Gating(std::string_view rule) {
		return rule != "public-header";
	}

	Report Check(const Tree &tree) {
		Report report;
		report.Scanned = tree.Files.size();

		const Index index = Build(tree);
		report.Components = index.Components.size();
		for (const File &file : tree.Files) {
			// A suite's crossing markers live inside raw string literals and are
			// never walked, so counting them would overstate what is checked.
			if (!file.Test) {
				report.Crossings += file.Crossings.size();
			}
			if (!file.IncludePath.empty()) {
				report.PublicHeaders++;
			}
		}

		for (const Finding &finding : EcsCopy(tree, index)) {
			report.Findings.push_back(finding);
		}
		for (const Finding &finding : WorldPointer(tree, index)) {
			report.Findings.push_back(finding);
		}
		for (const Finding &finding : CheckNameId(tree)) {
			report.Findings.push_back(finding);
		}
		for (const Finding &finding : CheckPublicHeader(tree)) {
			report.Findings.push_back(finding);
		}

		return report;
	}
}
