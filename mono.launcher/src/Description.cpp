#include <engine/core/Log.hpp>
#include <engine/parallel/Capture.hpp>

#include <launcher/Description.hpp>
#include <launcher/Programs.hpp>
#include <nlohmann/json.hpp>

namespace launcher {

	namespace {
		// A string field, or empty when it is absent or not a string.
		//
		// **Tolerant on purpose.** The producer is `core::Arguments::Describe`
		// and the two are built from the same tree, so a missing field means a
		// staged binary older than the field - and the useful thing to do with
		// an older client is show the options it *did* report, not refuse the
		// whole mode.
		std::string Text(const nlohmann::json &object, const char *key) {
			const auto found = object.find(key);
			if (found == object.end() || !found->is_string()) {
				return {};
			}
			return found->get<std::string>();
		}

		bool Boolean(const nlohmann::json &object, const char *key) {
			const auto found = object.find(key);
			return found != object.end() && found->is_boolean() && found->get<bool>();
		}
	}

	const DescribedOption *Description::Option(std::string_view name) const {
		for (const DescribedOption &option : Options) {
			if (option.Name == name) {
				return &option;
			}
		}
		return nullptr;
	}

	std::optional<Description> ParseDescription(const std::string &json, std::string &failure) {
		// **`parse` with throwing off.** The input is another process's stdout,
		// which is the one input this program has no control over at all: a
		// program that logged a line before its JSON, or crashed halfway
		// through printing it, must leave a message on the screen rather than
		// take the launcher down with it.
		const nlohmann::json document = nlohmann::json::parse(json, nullptr, false);
		if (document.is_discarded() || !document.is_object()) {
			failure = "not a JSON object - the program printed something else";
			return std::nullopt;
		}

		Description description;
		description.Program = Text(document, "program");
		description.Summary = Text(document, "summary");
		description.Version = Text(document, "version");

		const auto options = document.find("options");
		if (options == document.end() || !options->is_array()) {
			failure = "no options array - is this binary older than v0.18?";
			return std::nullopt;
		}
		for (const nlohmann::json &entry : *options) {
			if (!entry.is_object()) {
				continue;
			}
			DescribedOption option;
			option.Name = Text(entry, "name");
			option.TakesValue = Boolean(entry, "takesValue");
			option.ValueName = Text(entry, "valueName");
			option.Description = Text(entry, "description");
			if (!option.Name.empty()) {
				description.Options.push_back(std::move(option));
			}
		}

		// Absent is not an error: `settings` is empty for a program that
		// declares no flag table, and empty and absent should look the same to
		// whoever is drawing the tab.
		const auto settings = document.find("settings");
		if (settings != document.end() && settings->is_array()) {
			for (const nlohmann::json &entry : *settings) {
				if (!entry.is_object()) {
					continue;
				}
				DescribedSetting setting;
				setting.Name = Text(entry, "name");
				setting.Kind = Text(entry, "kind");
				setting.Default = Text(entry, "default");
				setting.Description = Text(entry, "description");
				if (!setting.Name.empty()) {
					description.Settings.push_back(std::move(setting));
				}
			}
		}

		return description;
	}

	std::optional<Description> ReadDescription(const std::filesystem::path &program, std::string &failure) {
		if (!ProgramPresent(program)) {
			failure = "not staged at " + program.string();
			return std::nullopt;
		}

		const engine::parallel::CaptureResult result =
			engine::parallel::Capture({program.string(), "--describe"});

		if (!result.Started) {
			failure = "could not be run: " + program.string();
			return std::nullopt;
		}
		if (result.ExitCode != 0) {
			failure = "exited " + std::to_string(result.ExitCode) + " for --describe";
			return std::nullopt;
		}

		// **The object rather than the whole of stdout.** Capture merges stderr
		// in, and a program that logged one line at startup would otherwise
		// hand a parser a log line followed by JSON. The object is the last
		// thing printed, so the search is for the first `{` on the final line's
		// side of things - in practice, the first `{` at all, since a log line
		// with a brace in it before `--describe` has never happened and would
		// be visible in `failure` if it did.
		const size_t opened = result.Output.find('{');
		if (opened == std::string::npos) {
			failure = "printed no JSON object";
			return std::nullopt;
		}

		return ParseDescription(result.Output.substr(opened), failure);
	}

	void
	Descriptions::Load(const std::filesystem::path &stageRoot, const std::vector<std::string> &programs) {
		for (const std::string &program : programs) {
			if (Loaded.find(program) != Loaded.end()) {
				continue;
			}

			std::string failure;
			auto description = ReadDescription(ProgramPath(stageRoot, program), failure);
			if (!description.has_value()) {
				Failures[program] = failure;
				ENGINE_INFO("launcher: {} is unavailable - {}", program, failure);
				continue;
			}

			ENGINE_INFO(
				"launcher: {} {} declares {} options and {} settings",
				program,
				description->Version,
				description->Options.size(),
				description->Settings.size()
			);
			Failures.erase(program);
			Loaded.emplace(program, std::move(*description));
		}
	}

	const Description *Descriptions::Find(std::string_view program) const {
		const auto found = Loaded.find(program);
		return found == Loaded.end() ? nullptr : &found->second;
	}

	std::string Descriptions::Failure(std::string_view program) const {
		const auto found = Failures.find(program);
		return found == Failures.end() ? std::string() : found->second;
	}
}
