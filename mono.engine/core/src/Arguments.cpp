#include <engine/core/Arguments.hpp>

#include <algorithm>
#include <charconv>
#include <sstream>

namespace engine::core {

	namespace {
		// Accepts `--name` and `-name`, so that a single dash is a spelling
		// rather than a separate concept. There are no bundled short flags:
		// `-abc` meaning three options is a source of surprise that this engine
		// has no need for.
		std::optional<std::string_view> AsOptionName(std::string_view argument) {
			if (argument.size() > 2 && argument.substr(0, 2) == "--") {
				return argument.substr(2);
			}
			if (argument.size() > 1 && argument[0] == '-') {
				return argument.substr(1);
			}
			return std::nullopt;
		}
	}

	Arguments::Arguments(std::string_view program, std::string_view summary)
		: Program(program), Summary(summary) {
		Flag("help", "Show this text");
	}

	Arguments &Arguments::Flag(std::string_view name, std::string_view description) {
		// Every field named, and named designators rather than a run of empty
		// braces, so the two booleans cannot be read the wrong way round.
		//
		// All of them, including the ones an aggregate would have
		// value-initialised anyway: the `ci` preset turns
		// `-Wmissing-field-initializers` into an error, and GCC counts a
		// designator left out the same as a positional one left off the end. So
		// "say every field" is the rule here rather than "say the interesting
		// ones" — and a field added to `Option` later then fails the build
		// instead of silently defaulting at two call sites.
		Options.push_back(
			Option{
				.Name = name,
				.ValueName = {},
				.Description = description,
				.TakesValue = false,
				.Present = false,
				.Given = {},
				.All = {},
			}
		);
		return *this;
	}

	Arguments &
	Arguments::Value(std::string_view name, std::string_view valueName, std::string_view description) {
		Options.push_back(
			Option{
				.Name = name,
				.ValueName = valueName,
				.Description = description,
				.TakesValue = true,
				.Present = false,
				.Given = {},
				.All = {},
			}
		);
		return *this;
	}

	Arguments::Option *Arguments::Find(std::string_view name) {
		auto found = std::find_if(Options.begin(), Options.end(), [name](const Option &option) {
			return option.Name == name;
		});
		return found == Options.end() ? nullptr : &*found;
	}

	const Arguments::Option *Arguments::Find(std::string_view name) const {
		return const_cast<Arguments *>(this)->Find(name);
	}

	Arguments::Result Arguments::Parse(int argc, char **argv) {
		Result result;

		for (int index = 1; index < argc; index++) {
			const std::string_view argument = argv[index];

			// Everything past `--` is positional, including things that look
			// like options. A file called "--stats" is a legitimate file.
			if (argument == "--") {
				for (int rest = index + 1; rest < argc; rest++) {
					Positionals.emplace_back(argv[rest]);
				}
				break;
			}

			auto name = AsOptionName(argument);
			if (!name) {
				Positionals.push_back(argument);
				continue;
			}

			// `--name=value` — split before the lookup, so that the option name
			// is what gets matched.
			std::optional<std::string_view> inlineValue;
			const auto equals = name->find('=');
			if (equals != std::string_view::npos) {
				inlineValue = name->substr(equals + 1);
				name = name->substr(0, equals);
			}

			Option *option = Find(*name);
			if (!option) {
				result.Ok = false;
				result.Error = "unknown option: " + std::string(argument);
				return result;
			}

			option->Present = true;

			if (!option->TakesValue) {
				if (inlineValue) {
					result.Ok = false;
					result.Error = "--" + std::string(*name) + " does not take a value";
					return result;
				}
				continue;
			}

			if (inlineValue) {
				option->Given = *inlineValue;
				option->All.push_back(*inlineValue);
				continue;
			}

			if (index + 1 >= argc) {
				result.Ok = false;
				result.Error = "--" + std::string(*name) + " needs a value";
				return result;
			}

			// A value that looks like an option is almost always a forgotten
			// value rather than a deliberate one, and saying so here is
			// cheaper than debugging the behaviour it would otherwise cause.
			const std::string_view next = argv[index + 1];
			if (AsOptionName(next) && Find(*AsOptionName(next))) {
				result.Ok = false;
				result.Error =
					"--" + std::string(*name) + " needs a value, but " + std::string(next) + " is an option";
				return result;
			}

			option->Given = next;
			option->All.push_back(next);
			index++;
		}

		result.HelpRequested = Has("help");
		return result;
	}

	bool Arguments::Has(std::string_view name) const {
		const Option *option = Find(name);
		return option && option->Present;
	}

	std::optional<std::string_view> Arguments::Get(std::string_view name) const {
		const Option *option = Find(name);
		if (!option || !option->Present || !option->TakesValue) {
			return std::nullopt;
		}
		return option->Given;
	}

	std::vector<std::string_view> Arguments::GetAll(std::string_view name) const {
		const Option *option = Find(name);
		if (!option || !option->Present || !option->TakesValue) {
			return {};
		}
		return option->All;
	}

	int64_t Arguments::GetInteger(std::string_view name, int64_t fallback) const {
		auto value = Get(name);
		if (!value || value->empty()) {
			return fallback;
		}

		int64_t parsed = 0;
		const auto *begin = value->data();
		const auto *end = begin + value->size();
		const auto outcome = std::from_chars(begin, end, parsed);
		if (outcome.ec != std::errc{} || outcome.ptr != end) {
			return fallback;
		}
		return parsed;
	}

	double Arguments::GetNumber(std::string_view name, double fallback) const {
		auto value = Get(name);
		if (!value || value->empty()) {
			return fallback;
		}

		// from_chars for double is not available everywhere libstdc++ is old,
		// and this is not a hot path.
		try {
			size_t consumed = 0;
			const std::string text(*value);
			const double parsed = std::stod(text, &consumed);
			if (consumed != text.size()) {
				return fallback;
			}
			return parsed;
		} catch (...) {
			return fallback;
		}
	}

	std::string Arguments::Help() const {
		std::ostringstream out;
		out << Summary << "\n\n";
		out << "usage: " << Program << " [options]\n\n";

		size_t width = 0;
		for (const auto &option : Options) {
			size_t length = option.Name.size() + 2;
			if (option.TakesValue) {
				length += option.ValueName.size() + 1;
			}
			width = std::max(width, length);
		}

		for (const auto &option : Options) {
			std::string spelling = "--" + std::string(option.Name);
			if (option.TakesValue) {
				spelling += " " + std::string(option.ValueName);
			}

			out << "  " << spelling;
			out << std::string(width - spelling.size() + 2, ' ');
			out << option.Description << "\n";
		}

		return out.str();
	}
}
