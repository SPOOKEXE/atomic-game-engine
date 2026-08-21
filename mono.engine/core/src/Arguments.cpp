#include <engine/core/Arguments.hpp>
#include <engine/core/Flags.hpp>
#include <engine/core/Version.hpp>

#include <algorithm>
#include <charconv>
#include <sstream>

namespace engine::core {

	namespace {
		// Accepts `--name` and `-name`, so that a single dash is a spelling
		// rather than a separate concept. There are no bundled short flags:
		// `-abc` meaning three options is a source of surprise that this engine
		// has no need for.
		// JSON string escaping, and only what a settings table can contain.
		//
		// **Written here rather than reached for**, because `Vendor::json` is a
		// parser and this module is L0 - the layer everything else sits on.
		// Emitting is a quoted string and a comma; reading one is the hard half,
		// and the hard half belongs to whoever is reading, which today is
		// `mono.launcher` at L13 where a vendored parser costs nothing.
		void WriteJsonString(std::ostringstream &out, std::string_view text) {
			out << '"';
			for (const char character : text) {
				switch (character) {
				case '"':
					out << "\\\"";
					break;
				case '\\':
					out << "\\\\";
					break;
				case '\n':
					out << "\\n";
					break;
				case '\r':
					out << "\\r";
					break;
				case '\t':
					out << "\\t";
					break;
				default:
					// Anything below space has no literal form in JSON.
					// Above it, bytes pass through: the source is UTF-8
					// already and re-encoding it would only be a chance to
					// corrupt it.
					if (static_cast<unsigned char>(character) < 0x20) {
						out << "\\u00" << (character < 0x10 ? "0" : "1")
							<< "0123456789abcdef"[character & 0x0F];
					} else {
						out << character;
					}
					break;
				}
			}
			out << '"';
		}

		// The spelling a config file would use, so a reader can round-trip it.
		const char *NameOfKind(FlagKind kind) {
			switch (kind) {
			case FlagKind::Boolean:
				return "boolean";
			case FlagKind::Integer:
				return "integer";
			case FlagKind::Number:
				return "number";
			case FlagKind::Text:
				return "text";
			case FlagKind::List:
				return "list";
			}
			return "text";
		}

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

		// Declared here beside `help` for the same reason `help` is: it is a
		// property of having a command line at all, not of any one program, and
		// a flag every program declares for itself is a flag one of them will
		// spell differently.
		Flag("version", "Print the version and exit");

		// The third of the same kind. A program that had to opt in to being
		// describable is a program somebody forgets to opt in, and the launcher
		// would then be missing exactly the mode nobody tested.
		Flag("describe", "Print every option and setting as JSON, and exit");
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
		// ones" - and a field added to `Option` later then fails the build
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

			// `--name=value` - split before the lookup, so that the option name
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
		result.VersionRequested = Has("version");
		result.DescribeRequested = Has("describe");
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

	std::string Arguments::VersionLine() const {
		return std::string(Program) + " " + std::string(Version()) + "\n";
	}

	std::string Arguments::Describe() const {
		std::ostringstream out;
		out << "{";
		out << "\"program\":";
		WriteJsonString(out, Program);
		out << ",\"summary\":";
		WriteJsonString(out, Summary);
		out << ",\"version\":";
		WriteJsonString(out, Version());

		out << ",\"options\":[";
		bool first = true;
		for (const Option &option : Options) {
			if (!first) {
				out << ",";
			}
			first = false;
			out << "{\"name\":";
			WriteJsonString(out, option.Name);
			out << ",\"takesValue\":" << (option.TakesValue ? "true" : "false");
			out << ",\"valueName\":";
			WriteJsonString(out, option.ValueName);
			out << ",\"description\":";
			WriteJsonString(out, option.Description);
			out << "}";
		}
		out << "]";

		// **The declared table rather than the current values.** A launcher is
		// building a form, so what it wants is the name, the kind and the
		// built-in default; what a value happens to be inside the child that
		// answered this question is that child's business and about to end with
		// it. `--flags` is the listing that reports values, and it reports them
		// with their sources, which is a different question.
		out << ",\"settings\":[";
		first = true;
		for (const FlagDescription &setting : Flags::Declared()) {
			if (!first) {
				out << ",";
			}
			first = false;
			out << "{\"name\":";
			WriteJsonString(out, setting.Name);
			out << ",\"kind\":";
			WriteJsonString(out, NameOfKind(setting.Kind));
			out << ",\"default\":";
			WriteJsonString(out, setting.Default);
			out << ",\"description\":";
			WriteJsonString(out, setting.Description);
			out << "}";
		}
		out << "]}\n";
		return out.str();
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
