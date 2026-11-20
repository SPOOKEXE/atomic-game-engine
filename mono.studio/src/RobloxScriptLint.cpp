// Finds the places a recovered Roblox script will reach for something this
// engine does not have.
//
// This is a scan, not a parser. It looks for the few call shapes whose argument
// names a runtime table entry, and asks the engine's own tables about each:
//
// - `GetService("X")`, `FindService("X")` and `game.X`: a root instance a
//   fresh world installs, or a catalogued service bound for Luau;
// - `Instance.new("X")`: a registered, creatable class;
// - `Enum.X` and `Enum.X.Y`: the engine `EnumTable`;
// - `require(12345)`: an asset id, which no import can resolve.
//
// Comments are skipped. Each script reports a given name once, at its first line.

#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/game/Game.hpp>
#include <engine/gui/Services.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/ServiceCatalogue.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <string>
#include <string_view>
#include <studio/RobloxImport.hpp>
#include <vector>

namespace studio {
	namespace {
		// `game.X` members that are not services.
		constexpr std::array<std::string_view, 16> GAME_MEMBERS{
			"Workspace",	   "PlaceId",	"PlaceVersion", "JobId",	   "GameId",	 "CreatorId",
			"CreatorType",	   "Loaded",	"Close",		"Name",		   "ClassName",	 "Parent",
			"PrivateServerId", "IsLoaded",	"BindToClose",	"PrivateServerOwnerId",
		};

		bool IsIdentifierCharacter(char character) {
			return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
		}

		// Blanks comments with spaces, keeping newlines so offsets still map to
		// lines. Strings are kept, since their contents are what is checked.
		std::string StripComments(std::string_view source) {
			std::string out(source);
			size_t at = 0;
			while (at < out.size()) {
				const char character = out[at];
				if (character == '"' || character == '\'') {
					at++;
					while (at < out.size() && out[at] != character && out[at] != '\n') {
						at += out[at] == '\\' ? 2 : 1;
					}
					at++;
					continue;
				}
				if (character != '-' || at + 1 >= out.size() || out[at + 1] != '-') {
					at++;
					continue;
				}
				size_t end = out.find('\n', at);
				if (at + 2 < out.size() && out[at + 2] == '[') {
					// A long comment: `--[[ ]]` or `--[==[ ]==]`.
					size_t level = at + 3;
					while (level < out.size() && out[level] == '=') {
						level++;
					}
					if (level < out.size() && out[level] == '[') {
						const std::string close = "]" + std::string(level - at - 3, '=') + "]";
						const size_t found = out.find(close, level);
						end = found == std::string::npos ? out.size() : found + close.size();
					}
				}
				if (end == std::string::npos) {
					end = out.size();
				}
				for (size_t blank = at; blank < end; blank++) {
					if (out[blank] != '\n') {
						out[blank] = ' ';
					}
				}
				at = end;
			}
			return out;
		}

		size_t SkipSpace(std::string_view text, size_t at) {
			while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])) != 0) {
				at++;
			}
			return at;
		}

		std::string_view IdentifierAt(std::string_view text, size_t at) {
			size_t end = at;
			while (end < text.size() && IsIdentifierCharacter(text[end])) {
				end++;
			}
			return text.substr(at, end - at);
		}

		// The quoted name of a call's first argument: `("X")`, `('X')` or `"X"`.
		std::string_view QuotedArgument(std::string_view text, size_t at) {
			at = SkipSpace(text, at);
			if (at < text.size() && text[at] == '(') {
				at = SkipSpace(text, at + 1);
			}
			if (at >= text.size() || (text[at] != '"' && text[at] != '\'')) {
				return {};
			}
			const char quote = text[at];
			const size_t end = text.find(quote, at + 1);
			if (end == std::string_view::npos) {
				return {};
			}
			const std::string_view name = text.substr(at + 1, end - at - 1);
			return std::all_of(name.begin(), name.end(), IsIdentifierCharacter) ? name : std::string_view{};
		}

		// Whether `word` occurs at `at` as a whole identifier.
		bool WordAt(std::string_view text, size_t at, std::string_view word) {
			if (text.compare(at, word.size(), word) != 0) {
				return false;
			}
			const bool startOk = at == 0 || (!IsIdentifierCharacter(text[at - 1]) && text[at - 1] != '.');
			const size_t after = at + word.size();
			return startOk && (after >= text.size() || !IsIdentifierCharacter(text[after]));
		}

		struct Checker {
			const engine::ecs::Store &Services;

			bool HasService(std::string_view name) const {
				if (name == "Workspace" || Services.FindFirstRoot(name) != engine::ecs::NULL_ENTITY) {
					return true;
				}
				const engine::script::ServiceDefinition *known =
					engine::script::FindService(std::string(name).c_str());
				return known != nullptr &&
					   engine::script::Binds(known->Languages, engine::script::ServiceLanguages::Luau);
			}
		};

		struct ScriptScan {
			const engine::bake::RobloxScript &Script;
			std::string_view Text;
			std::set<std::pair<std::string, std::string>> Seen;
			std::vector<RobloxScriptFinding> &Out;

			size_t LineOf(size_t at) const {
				return 1 + static_cast<size_t>(std::count(Text.begin(), Text.begin() + at, '\n'));
			}

			void Report(size_t at, std::string kind, std::string name, std::string message) {
				if (!Seen.emplace(kind, name).second) {
					return;
				}
				Out.push_back({Script.InstancePath, LineOf(at), std::move(kind), std::move(name), std::move(message)});
			}
		};
	}

	std::vector<RobloxScriptFinding> LintRobloxScripts(const engine::bake::RobloxModel &model) {
		std::vector<RobloxScriptFinding> findings;
		if (model.Scripts.empty()) {
			return findings;
		}

		engine::game::RegisterGameClasses();
		engine::effects::RegisterEffectClasses();
		// A fresh world's roots are the services a script will find by name.
		engine::ecs::Store services("roblox-script-lint");
		(void)engine::scene::InstallServices(services);
		(void)engine::gui::InstallGuiServices(services);
		const Checker checker{services};

		for (const engine::bake::RobloxScript &script : model.Scripts) {
			const std::string stripped = StripComments(script.Source);
			const std::string_view text = stripped;
			ScriptScan scan{script, text, {}, findings};

			for (size_t at = 0; at < text.size(); at++) {
				if (!IsIdentifierCharacter(text[at]) || (at > 0 && IsIdentifierCharacter(text[at - 1]))) {
					continue;
				}
				const std::string_view word = IdentifierAt(text, at);

				if ((word == "GetService" || word == "FindService") && at > 0 && text[at - 1] == ':') {
					const std::string_view name = QuotedArgument(text, at + word.size());
					if (!name.empty() && !checker.HasService(name)) {
						scan.Report(at, "service", std::string(name), "service is not provided by this engine");
					}
				} else if (WordAt(text, at, "game") && SkipSpace(text, at + 4) < text.size() &&
						   text[SkipSpace(text, at + 4)] == '.') {
					const size_t member = SkipSpace(text, SkipSpace(text, at + 4) + 1);
					const std::string_view name = IdentifierAt(text, member);
					const bool isMember =
						std::find(GAME_MEMBERS.begin(), GAME_MEMBERS.end(), name) != GAME_MEMBERS.end();
					if (!name.empty() && std::isupper(static_cast<unsigned char>(name[0])) != 0 && !isMember &&
						!checker.HasService(name)) {
						scan.Report(at, "service", std::string(name), "service is not provided by this engine");
					}
				} else if (WordAt(text, at, "Instance") && text.compare(SkipSpace(text, at + 8), 4, ".new") == 0) {
					const std::string_view name = QuotedArgument(text, SkipSpace(text, at + 8) + 4);
					if (!name.empty()) {
						const engine::ecs::ClassId id = engine::ecs::Classes::Find(engine::core::Name(std::string(name)));
						if (!id.IsValid()) {
							scan.Report(at, "class", std::string(name), "class does not exist in this engine");
						} else if (!engine::ecs::Classes::Describe(id).Creatable) {
							scan.Report(at, "class", std::string(name), "class exists but cannot be created");
						}
					}
				} else if (WordAt(text, at, "Enum") && at + 4 < text.size() && text[at + 4] == '.') {
					const std::string_view enumName = IdentifierAt(text, at + 5);
					if (enumName.empty()) {
						continue;
					}
					const engine::core::Name enumKey{std::string(enumName)};
					if (!engine::ecs::EnumTable::Known(enumKey)) {
						scan.Report(at, "enum", "Enum." + std::string(enumName), "enum does not exist in this engine");
						continue;
					}
					const size_t dot = at + 5 + enumName.size();
					if (dot < text.size() && text[dot] == '.') {
						const std::string_view member = IdentifierAt(text, dot + 1);
						if (!member.empty() && member != "GetEnumItems" &&
							!engine::ecs::EnumTable::Has(enumKey, engine::core::Name(std::string(member)))) {
							scan.Report(
								at,
								"enum",
								"Enum." + std::string(enumName) + "." + std::string(member),
								"enum member does not exist in this engine"
							);
						}
					}
				} else if (WordAt(text, at, "require")) {
					const size_t open = SkipSpace(text, at + 7);
					if (open < text.size() && text[open] == '(') {
						const size_t digits = SkipSpace(text, open + 1);
						size_t end = digits;
						while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])) != 0) {
							end++;
						}
						const size_t close = SkipSpace(text, end);
						if (end > digits && close < text.size() && text[close] == ')') {
							scan.Report(
								at,
								"require",
								std::string(text.substr(digits, end - digits)),
								"requires a Roblox asset id, which cannot be imported"
							);
						}
					}
				}
				at += word.empty() ? 0 : word.size() - 1;
			}
		}
		return findings;
	}
}
