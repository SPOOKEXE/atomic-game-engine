#include <engine/core/Arguments.hpp>
#include <engine/core/Config.hpp>
#include <engine/core/Log.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>

namespace engine::core {
	namespace {
		// The option names, in one place so the declaration and the three
		// readers cannot disagree about a spelling.
		constexpr std::string_view CONFIG_OPTION = "config";
		constexpr std::string_view FLAG_OPTION = "flag";
		constexpr std::string_view FLAGS_OPTION = "flags";
		constexpr std::string_view LOG_OPTION = "log";
		constexpr const char *CONFIG_VARIABLE = "ATOMIC_CONFIG";

		std::string_view Trimmed(std::string_view text) {
			while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
				text.remove_prefix(1);
			}
			while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
				text.remove_suffix(1);
			}
			return text;
		}

		// Strips a trailing comment, honouring one level of double quotes.
		//
		// **Quotes are about the value and not about the format**, which is why
		// they are stripped here rather than parsed as a type: a path with a `#`
		// in it and a value that is deliberately blank are the two cases, and
		// both are values somebody wrote rather than syntax.
		std::string_view ValueOf(std::string_view text) {
			text = Trimmed(text);
			if (text.size() >= 2 && text.front() == '"') {
				const size_t closing = text.find('"', 1);
				if (closing != std::string_view::npos) {
					return text.substr(1, closing - 1);
				}
			}

			const size_t comment = text.find_first_of("#;");
			if (comment != std::string_view::npos) {
				text = text.substr(0, comment);
			}
			return Trimmed(text);
		}

		std::string Lowered(std::string_view text) {
			std::string lowered(text);
			std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char value) {
				return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
			});
			return lowered;
		}

		// The environment variable name a flag would be spelled as.
		//
		// **Derived from the flag rather than the other way round**, which is
		// the whole reason the environment adapter walks the declared table:
		// `server.tick-rate` and `server.tick.rate` produce the same
		// `SERVER_TICK_RATE`, so the mapping is only a function in this
		// direction.
		std::string VariableNameOf(std::string_view prefix, std::string_view flag) {
			std::string name(prefix);
			for (const char letter : flag) {
				if (letter == '.' || letter == '-') {
					name.push_back('_');
				} else if (letter >= 'a' && letter <= 'z') {
					name.push_back(static_cast<char>(letter - 'a' + 'A'));
				} else {
					name.push_back(letter);
				}
			}
			return name;
		}

		constexpr std::string_view LOG_LEVEL_FLAG = "engine.log-level";
		constexpr std::string_view SOURCE_FLAG = "engine.log-source";

		// **One setting rather than two, and a bare level still means what it
		// always did.** The flag grew per-category terms at v0.19 and did not
		// grow a second flag beside it: `engine.log-level` and
		// `engine.log-categories` would be two settings that can disagree about
		// what `net` is set to, and the loser would be whichever the reader did
		// not think to look at.
		constexpr std::array<FlagDescription, 2> ENGINE_FLAGS{{
			{LOG_LEVEL_FLAG,
			 FlagKind::Text,
			 "info",
			 "A level - trace, debug, info, warning, error, off - and any number of "
			 "category=level terms, comma separated"},
			{SOURCE_FLAG, FlagKind::Boolean, "true", "Append file:line to every line that is not info"},
		}};

		// Records a `Set` outcome on a report, naming the first failure.
		void Record(ConfigReport &report, FlagStatus status, std::string_view where, std::string_view name) {
			switch (status) {
			case FlagStatus::Applied:
				report.Applied++;
				return;
			case FlagStatus::Outranked:
				report.Outranked++;
				return;
			case FlagStatus::NoSuchFlag:
			case FlagStatus::NotAValue:
			case FlagStatus::Frozen:
				break;
			}

			if (report.Ok) {
				report.Ok = false;
				report.Error = std::string(where) + ": '" + std::string(name) + "': " + Describe(status);
			}
		}
	}

	std::string Config::FlagNameOf(std::string_view section, std::string_view key) {
		if (section.empty()) {
			return Lowered(key);
		}
		return Lowered(section) + "." + Lowered(key);
	}

	ConfigReport Config::ApplyFile(const std::filesystem::path &path) {
		ConfigReport report;
		if (path.empty()) {
			return report;
		}

		std::error_code failure;
		if (!std::filesystem::exists(path, failure)) {
			// A program with no config file is the ordinary case.
			return report;
		}

		std::ifstream file(path);
		if (!file.is_open()) {
			report.Ok = false;
			report.Error = path.string() + ": cannot be opened";
			return report;
		}

		std::string section;
		std::string line;
		uint32_t number = 0;

		while (std::getline(file, line)) {
			number++;
			const std::string_view trimmed = Trimmed(line);
			if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
				continue;
			}

			const std::string where = path.string() + ":" + std::to_string(number);

			if (trimmed.front() == '[') {
				const size_t closing = trimmed.find(']');
				if (closing == std::string_view::npos) {
					if (report.Ok) {
						report.Ok = false;
						report.Error = where + ": a section header with no ']'";
					}
					continue;
				}
				section = Trimmed(trimmed.substr(1, closing - 1));
				continue;
			}

			const size_t equals = trimmed.find('=');
			if (equals == std::string_view::npos) {
				if (report.Ok) {
					report.Ok = false;
					report.Error = where + ": no '=' on this line";
				}
				continue;
			}

			const std::string_view key = Trimmed(trimmed.substr(0, equals));
			if (key.empty()) {
				if (report.Ok) {
					report.Ok = false;
					report.Error = where + ": no key before the '='";
				}
				continue;
			}

			const std::string name = FlagNameOf(section, key);
			const std::string_view value = ValueOf(trimmed.substr(equals + 1));
			Record(report, Flags::Set(name, value, FlagSource::ConfigFile), where, name);
		}

		return report;
	}

	ConfigReport Config::ApplyEnvironment(std::string_view prefix) {
		ConfigReport report;

		// **The declared table is walked rather than the environment**, because
		// there is no portable way to enumerate the environment and - more to
		// the point - an unknown `ATOMIC_` variable is not this engine's to
		// complain about. See the header.
		for (const FlagDescription &description : Flags::Declared()) {
			const std::string variable = VariableNameOf(prefix, description.Name);
			const char *value = std::getenv(variable.c_str());
			if (value == nullptr) {
				continue;
			}
			Record(
				report,
				Flags::Set(description.Name, value, FlagSource::Environment),
				variable,
				description.Name
			);
		}

		return report;
	}

	ConfigReport Config::ApplyArguments(const Arguments &arguments) {
		ConfigReport report;

		for (const std::string_view given : arguments.GetAll(FLAG_OPTION)) {
			const size_t equals = given.find('=');

			// **A bare `--flag content.gif` is `true`**, so a boolean reads the
			// way a flag reads everywhere else. `Flags::Set` is what decides
			// that an empty value means `true` for a boolean and is not a value
			// at all for the other three kinds.
			const std::string_view name = equals == std::string_view::npos ? given : given.substr(0, equals);
			const std::string_view value =
				equals == std::string_view::npos ? std::string_view{} : given.substr(equals + 1);

			Record(report, Flags::Set(name, value, FlagSource::CommandLine), "--flag", name);
		}

		return report;
	}

	bool Config::DeclareEngineFlags() {
		return Flags::Declare(ENGINE_FLAGS);
	}

	void Config::DeclareOptions(Arguments &arguments) {
		arguments.Value(CONFIG_OPTION, "PATH", "Read settings from this file, before the environment");
		arguments.Value(FLAG_OPTION, "NAME=VALUE", "Set one setting, above everything else. May repeat");
		arguments.Flag(FLAGS_OPTION, "Print every setting, where its value came from, and exit");

		// **A spelling of one flag rather than a second way to set it.**
		// `--log net=trace` is applied as `engine.log-level` at command-line
		// precedence, so it beats a config file and an environment variable for
		// the reason everything typed does, and `--flags` lists one setting
		// with one value. It exists because the flag is the one somebody
		// reaches for while a program is misbehaving, and
		// `--flag engine.log-level=net=trace` has two equals signs in it.
		arguments.Value(LOG_OPTION, "SPEC", "Set log levels: a level, or category=level terms");
	}

	ConfigReport Config::Apply(const Arguments &arguments) {
		// The file first, because it is the lowest of the three and every later
		// source is meant to be able to override it.
		//
		// **`--config` beats `ATOMIC_CONFIG`**, which is the same rule the
		// values themselves follow: what somebody typed outranks what their
		// shell was carrying.
		std::filesystem::path path;
		if (const auto named = arguments.Get(CONFIG_OPTION); named.has_value()) {
			path = std::filesystem::path(*named);
			if (!std::filesystem::exists(path)) {
				// **Named and missing is an error where unnamed and missing is
				// not.** A program launched with `--config prod.cfg` that
				// silently ran on defaults is the deployment failure this whole
				// layer exists to make impossible.
				ConfigReport report;
				report.Ok = false;
				report.Error = path.string() + ": no such file";
				Flags::Freeze();
				return report;
			}
		} else if (const char *variable = std::getenv(CONFIG_VARIABLE); variable != nullptr) {
			path = std::filesystem::path(variable);
		}

		ConfigReport report = ApplyFile(path);

		const ConfigReport environment = ApplyEnvironment();
		report.Applied += environment.Applied;
		report.Outranked += environment.Outranked;
		if (report.Ok && !environment.Ok) {
			report.Ok = false;
			report.Error = environment.Error;
		}

		// Before `ApplyArguments`, so that an explicit `--flag engine.log-level=`
		// on the same command line still wins. Both are `CommandLine`, and
		// `Flags::Set` compares strictly - a later value from the same source
		// replaces an earlier one - so the order here is what decides between
		// two spellings of one setting, and the longer, more explicit spelling
		// is the one that should win.
		if (const auto spelled = arguments.Get(LOG_OPTION); spelled.has_value()) {
			Record(
				report, Flags::Set(LOG_LEVEL_FLAG, *spelled, FlagSource::CommandLine), "--log", LOG_LEVEL_FLAG
			);
		}

		const ConfigReport typed = ApplyArguments(arguments);
		report.Applied += typed.Applied;
		report.Outranked += typed.Outranked;
		if (report.Ok && !typed.Ok) {
			report.Ok = false;
			report.Error = typed.Error;
		}

		// **Applied before the freeze rather than after it**, so a bad spelling
		// is a fault this report carries alongside every other one - and so the
		// rest of startup logs at the level the deployment asked for rather than
		// at the default it was about to leave behind.
		if (Flags::Has(LOG_LEVEL_FLAG)) {
			const Flag level(LOG_LEVEL_FLAG);
			std::string_view unknown;
			if (!Log::Configure(level.Text(), &unknown) && report.Ok) {
				report.Ok = false;
				report.Error = std::string(LOG_LEVEL_FLAG) + ": '" + std::string(unknown) +
							   "' names no level. A term is trace, debug, info, warning, error or "
							   "off, optionally prefixed with 'category='";
			}
		}

		if (Flags::Has(SOURCE_FLAG)) {
			Log::SetSourceLocationShown(Flag(SOURCE_FLAG).Boolean());
		}

		Flags::Freeze();
		return report;
	}

	bool Config::ListingWanted(const Arguments &arguments) {
		return arguments.Has(FLAGS_OPTION);
	}
}
