#include <algorithm>
#include <cctype>
#include <launcher/Plan.hpp>

namespace launcher {

	namespace {
		char Lower(char character) {
			return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		}

		bool ContainsFolded(std::string_view haystack, std::string_view needle) {
			if (needle.size() > haystack.size()) {
				return false;
			}
			for (size_t start = 0; start + needle.size() <= haystack.size(); start++) {
				size_t index = 0;
				while (index < needle.size() && Lower(haystack[start + index]) == Lower(needle[index])) {
					index++;
				}
				if (index == needle.size()) {
					return true;
				}
			}
			return false;
		}

		// The part of a name before its first separator, which is the grouping
		// the declarations were already written with.
		std::string_view PrefixOf(std::string_view name, char separator) {
			const size_t split = name.find(separator);
			return split == std::string_view::npos ? name : name.substr(0, split);
		}

	}

	BrowseShape BrowseShapeOf(const DescribedOption &option) {
		if (!option.TakesValue) {
			return BrowseShape::None;
		}
		if (option.ValueName == "PATH") {
			return BrowseShape::File;
		}
		if (option.ValueName == "DIR") {
			return BrowseShape::Folder;
		}
		return BrowseShape::None;
	}

	bool AnyBrowses(const Description &description, const std::vector<std::string> &names) {
		for (const std::string &name : names) {
			const DescribedOption *option = description.Option(name);
			if (option != nullptr && BrowseShapeOf(*option) != BrowseShape::None) {
				return true;
			}
		}
		return false;
	}

	bool IsBooleanSetting(const DescribedSetting &setting) {
		return setting.Kind == "boolean";
	}

	bool IsLauncherOwnedOption(std::string_view name) {
		// `help`, `version` and `describe` print and exit, so a form offering
		// them offers a button that starts nothing. `flags` is the same. `flag`
		// is how this launcher emits the settings tab, so a second, hand-typed
		// one would be two places writing the same argument with no rule about
		// which wins.
		return name == "help" || name == "version" || name == "describe" || name == "flags" || name == "flag";
	}

	Form NewForm(const Mode &mode, const Description &description) {
		Form form;
		form.ModeId = mode.Id;

		for (const DescribedOption &option : description.Options) {
			if (IsLauncherOwnedOption(option.Name)) {
				continue;
			}
			form.Options.push_back(
				FieldState{
					.Option = option.Name,
					.Enabled = false,
					.Value = {},
				}
			);
		}

		// **After every row exists, and by name.** A preset naming an option
		// the program does not declare is silently nothing rather than an
		// error: the catalogue is written against the programs in this tree,
		// and a launcher run beside an older staged client should offer the
		// options that client has rather than refuse the mode.
		for (const ModePreset &preset : mode.Presets) {
			for (FieldState &field : form.Options) {
				if (field.Option != preset.Option) {
					continue;
				}
				field.Enabled = true;
				field.Value = preset.Value;
			}
		}

		for (const DescribedSetting &setting : description.Settings) {
			form.Settings.push_back(
				SettingState{
					.Name = setting.Name,
					.Enabled = false,
					.Value = setting.Default,
				}
			);
		}

		return form;
	}

	std::vector<OptionGroup> GroupOptions(const Description &description) {
		// How many options share each prefix, so that a prefix with one user
		// does not become a collapsing header containing a single row.
		std::vector<std::pair<std::string_view, size_t>> counts;
		for (const DescribedOption &option : description.Options) {
			if (IsLauncherOwnedOption(option.Name)) {
				continue;
			}
			const std::string_view prefix = PrefixOf(option.Name, '-');
			auto found = std::find_if(counts.begin(), counts.end(), [&](const auto &entry) {
				return entry.first == prefix;
			});
			if (found == counts.end()) {
				counts.emplace_back(prefix, 1);
			} else {
				found->second++;
			}
		}

		std::vector<OptionGroup> groups;
		OptionGroup leftovers;
		leftovers.Title = "Everything else";

		for (const DescribedOption &option : description.Options) {
			if (IsLauncherOwnedOption(option.Name)) {
				continue;
			}

			const std::string_view prefix = PrefixOf(option.Name, '-');
			const auto found = std::find_if(counts.begin(), counts.end(), [&](const auto &entry) {
				return entry.first == prefix;
			});

			if (found == counts.end() || found->second < 2) {
				leftovers.Options.push_back(option.Name);
				continue;
			}

			auto group = std::find_if(groups.begin(), groups.end(), [&](const OptionGroup &candidate) {
				return candidate.Title == prefix;
			});
			if (group == groups.end()) {
				groups.push_back(OptionGroup{.Title = std::string(prefix), .Options = {}});
				group = std::prev(groups.end());
			}
			group->Options.push_back(option.Name);
		}

		// Last, because it is the group with no theme and the one somebody
		// scrolls past rather than to.
		if (!leftovers.Options.empty()) {
			groups.push_back(std::move(leftovers));
		}
		return groups;
	}

	std::vector<OptionGroup> GroupSettings(const Description &description) {
		std::vector<OptionGroup> groups;
		for (const DescribedSetting &setting : description.Settings) {
			const std::string_view prefix = PrefixOf(setting.Name, '.');

			auto group = std::find_if(groups.begin(), groups.end(), [&](const OptionGroup &candidate) {
				return candidate.Title == prefix;
			});
			if (group == groups.end()) {
				groups.push_back(OptionGroup{.Title = std::string(prefix), .Options = {}});
				group = std::prev(groups.end());
			}
			group->Options.push_back(setting.Name);
		}
		return groups;
	}

	bool Matches(std::string_view query, std::string_view name, std::string_view description) {
		return query.empty() || ContainsFolded(name, query) || ContainsFolded(description, query);
	}

	std::vector<std::string> MatchingOptions(
		const Description &description, const std::vector<std::string> &names, std::string_view query
	) {
		std::vector<std::string> kept;
		for (const std::string &name : names) {
			const DescribedOption *option = description.Option(name);
			if (option != nullptr && Matches(query, option->Name, option->Description)) {
				kept.push_back(name);
			}
		}
		return kept;
	}

	std::vector<std::string> MatchingSettings(
		const Description &description, const std::vector<std::string> &names, std::string_view query
	) {
		std::vector<std::string> kept;
		for (const std::string &name : names) {
			const DescribedSetting *setting = description.Setting(name);
			if (setting != nullptr && Matches(query, setting->Name, setting->Description)) {
				kept.push_back(name);
			}
		}
		return kept;
	}

	size_t OptionHits(const Description &description, std::string_view query) {
		size_t hits = 0;
		for (const DescribedOption &option : description.Options) {
			if (!IsLauncherOwnedOption(option.Name) && Matches(query, option.Name, option.Description)) {
				hits++;
			}
		}
		return hits;
	}

	size_t SettingHits(const Description &description, std::string_view query) {
		size_t hits = 0;
		for (const DescribedSetting &setting : description.Settings) {
			if (Matches(query, setting.Name, setting.Description)) {
				hits++;
			}
		}
		return hits;
	}

	size_t RowsFor(const Form &form, std::string_view option) {
		size_t rows = 0;
		for (const FieldState &field : form.Options) {
			if (field.Option == option) {
				rows++;
			}
		}
		return rows;
	}

	size_t AddRow(Form &form, size_t row) {
		if (row >= form.Options.size()) {
			return row;
		}

		// Switched on, because an added row that arrived off would be an empty
		// row somebody has to notice and tick before it does anything - and the
		// reason to press `+` is to give the option another value.
		form.Options.insert(
			form.Options.begin() + static_cast<ptrdiff_t>(row) + 1,
			FieldState{.Option = form.Options[row].Option, .Enabled = true, .Value = {}}
		);
		return row + 1;
	}

	bool RemoveRow(Form &form, size_t row) {
		if (row >= form.Options.size()) {
			return false;
		}

		if (RowsFor(form, form.Options[row].Option) < 2) {
			form.Options[row].Enabled = false;
			form.Options[row].Value.clear();
			return false;
		}

		form.Options.erase(form.Options.begin() + static_cast<ptrdiff_t>(row));
		return true;
	}

	void SetRows(Form &form, size_t row, const std::vector<std::string> &values) {
		if (row >= form.Options.size() || values.empty()) {
			return;
		}

		form.Options[row].Value = values.front();
		form.Options[row].Enabled = true;

		const std::string option = form.Options[row].Option;
		for (size_t index = 1; index < values.size(); index++) {
			form.Options.insert(
				form.Options.begin() + static_cast<ptrdiff_t>(row + index),
				FieldState{.Option = option, .Enabled = true, .Value = values[index]}
			);
		}
	}

	std::vector<std::string> CommandLine(const Form &form, const Description &description) {
		std::vector<std::string> arguments;

		for (const FieldState &field : form.Options) {
			if (!field.Enabled) {
				continue;
			}

			const DescribedOption *declared = description.Option(field.Option);

			// An option the program did not declare would be refused by the
			// child's own parser - `core::Arguments` treats an unknown option
			// as an error rather than as silence - so dropping it here turns a
			// launcher that cannot start anything into one that starts what it
			// understood. It cannot happen from the form, which is built from
			// the same declarations; it can happen from a preset.
			if (declared == nullptr) {
				continue;
			}

			arguments.push_back("--" + field.Option);

			// **Two entries rather than `--name=value`.** Both spellings parse,
			// and this one has no character in it that a value could also
			// contain - a `--flag content.gif=false` given as one entry would
			// have to be split by whoever reads it, and a path with an `=` in
			// it is legal.
			if (declared->TakesValue) {
				arguments.push_back(field.Value);
			}
		}

		for (const SettingState &setting : form.Settings) {
			if (!setting.Enabled) {
				continue;
			}
			arguments.push_back("--flag");
			arguments.push_back(setting.Name + "=" + setting.Value);
		}

		return arguments;
	}

	std::string DisplayCommandLine(
		const std::filesystem::path &program, const Form &form, const Description &description
	) {
		const auto quote = [](const std::string &word) {
			if (!word.empty() && word.find_first_of(" \t\"'\\$`*?()[]{}|&;<>#~!") == std::string::npos) {
				return word;
			}

			// Single quotes, because inside them a shell expands nothing at
			// all. The one character that cannot appear is the quote itself,
			// which is closed, escaped and reopened.
			std::string quoted = "'";
			for (const char character : word) {
				if (character == '\'') {
					quoted += "'\\''";
				} else {
					quoted.push_back(character);
				}
			}
			quoted.push_back('\'');
			return quoted;
		};

		std::string line = quote(program.string());
		for (const std::string &argument : CommandLine(form, description)) {
			line.push_back(' ');
			line += quote(argument);
		}
		return line;
	}
}
