#include <engine/core/Flags.hpp>
#include <engine/core/Log.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace engine::core {
	namespace {
		// One declared flag and what has been said about it.
		struct Row {
			FlagDescription Description;
			std::string Text;
			FlagSource Source = FlagSource::Default;

			// Read once when the text is applied rather than at every read.
			//
			// **Because a read happens in a tick and a parse does not have to.**
			// `Flag::Boolean` is reached from a per-entity loop in the content
			// pump; re-parsing "true" there would be a string compare per row
			// for a value that cannot have changed since `Freeze`.
			bool Boolean = false;
			int64_t Integer = 0;
			double Number = 0.0;

			// A `List`'s entries, in the order they were given. `Text` holds
			// them joined, for the listing.
			std::vector<std::string> Items;
		};

		// The declared table, the freeze, and the generation a `Flag` caches
		// against.
		//
		// **A function-local static rather than a namespace one**, so the order
		// this is built in relative to a file-scope `Flag` in another
		// translation unit is decided by first use instead of by link order.
		struct Table {
			std::vector<Row> Rows;
			bool Frozen = false;
			uint32_t Generation = 0;

			// Guards declaration and `Set`, which happen during startup and may
			// come from more than one thread in a program that opens a service
			// while parsing. Reads after `Freeze` take nothing - that is what
			// the freeze buys.
			std::mutex Lock;
		};

		Table &Contents() {
			static Table table;
			return table;
		}

		// Lowercases in place, ASCII only. Flag names and boolean spellings are
		// both ASCII by construction - a name is declared in this repository and
		// a boolean is one of eight words.
		std::string LoweredFlag(std::string_view text) {
			std::string lowered(text);
			std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char value) {
				return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
			});
			return lowered;
		}

		// Reads a boolean the way a person writes one.
		//
		// **Empty is `true`**, because that is what a bare `--content-gif` on a
		// command line hands over and the alternative is every caller spelling
		// `=true` after a flag whose whole point is that it is a flag.
		bool ReadBoolean(std::string_view text, bool &out) {
			if (text.empty()) {
				out = true;
				return true;
			}

			const std::string lowered = LoweredFlag(text);
			if (lowered == "true" || lowered == "on" || lowered == "yes" || lowered == "1") {
				out = true;
				return true;
			}
			if (lowered == "false" || lowered == "off" || lowered == "no" || lowered == "0") {
				out = false;
				return true;
			}
			return false;
		}

		// **The whole string or nothing.** `strtoll` stopping halfway through
		// `30fps` would read 30 and a caller would never learn that three
		// characters were ignored, which is the class of bug `core::Arguments`
		// declares its options to avoid.
		bool ReadInteger(std::string_view text, int64_t &out) {
			if (text.empty()) {
				return false;
			}

			const std::string owned(text);
			char *end = nullptr;
			errno = 0;
			const long long value = std::strtoll(owned.c_str(), &end, 10);
			if (errno != 0 || end == nullptr || *end != '\0') {
				return false;
			}
			out = static_cast<int64_t>(value);
			return true;
		}

		bool ReadNumber(std::string_view text, double &out) {
			if (text.empty()) {
				return false;
			}

			const std::string owned(text);
			char *end = nullptr;
			errno = 0;
			const double value = std::strtod(owned.c_str(), &end);
			if (errno != 0 || end == nullptr || *end != '\0') {
				return false;
			}
			out = value;
			return true;
		}

		// Applies `text` to `row`, leaving it untouched when the text is not a
		// value of its kind.
		bool Interpret(Row &row, std::string_view text) {
			switch (row.Description.Kind) {
			case FlagKind::Boolean: {
				bool value = false;
				if (!ReadBoolean(text, value)) {
					return false;
				}
				row.Boolean = value;
				row.Text = value ? "true" : "false";
				return true;
			}
			case FlagKind::Integer: {
				int64_t value = 0;
				if (!ReadInteger(text, value)) {
					return false;
				}
				row.Integer = value;
				row.Number = static_cast<double>(value);
				row.Text.assign(text);
				return true;
			}
			case FlagKind::Number: {
				double value = 0.0;
				if (!ReadNumber(text, value)) {
					return false;
				}
				row.Number = value;
				row.Integer = static_cast<int64_t>(value);
				row.Text.assign(text);
				return true;
			}
			case FlagKind::Text:
				row.Text.assign(text);
				return true;
			case FlagKind::List:
				// Handled by `Apply` below, which is the only thing that knows
				// whether this is a replacement or an append.
				return true;
			}
			return false;
		}

		// Rebuilds a list row's display text from its items.
		void Joined(Row &row) {
			row.Text.clear();
			for (const std::string &item : row.Items) {
				if (!row.Text.empty()) {
					row.Text += ", ";
				}
				row.Text += item;
			}
		}

		// Applies `text` to `row`, knowing where it came from.
		//
		// **A list is the only kind that cares about the source**, because it is
		// the only one where "the same source again" is not simply the later
		// value winning: three origins in a config file are three lines, and a
		// scalar rule would make the third the only one.
		bool Apply(Row &row, std::string_view text, FlagSource source) {
			if (row.Description.Kind != FlagKind::List) {
				return Interpret(row, text);
			}

			// **An outranking source replaces**, so one `--flag` on a command
			// line removes what a file named rather than being appended to
			// something the person cannot see.
			if (source > row.Source) {
				row.Items.clear();
			}

			if (text.empty()) {
				// The way to say "none" to a file that named some. An append of
				// nothing would be indistinguishable from not writing the line.
				row.Items.clear();
			} else {
				row.Items.emplace_back(text);
			}

			Joined(row);
			return true;
		}
	}

	const char *Describe(FlagSource source) {
		switch (source) {
		case FlagSource::Default:
			return "default";
		case FlagSource::ConfigFile:
			return "config";
		case FlagSource::Environment:
			return "environment";
		case FlagSource::CommandLine:
			return "command line";
		}
		return "default";
	}

	const char *Describe(FlagStatus status) {
		switch (status) {
		case FlagStatus::Applied:
			return "applied";
		case FlagStatus::NoSuchFlag:
			return "no such flag";
		case FlagStatus::NotAValue:
			return "not a value of that kind";
		case FlagStatus::Outranked:
			return "already set by something that outranks it";
		case FlagStatus::Frozen:
			return "flags are frozen";
		}
		return "unknown";
	}

	bool Flags::Declare(std::span<const FlagDescription> table) {
		Table &contents = Contents();
		const std::lock_guard<std::mutex> held(contents.Lock);

		bool ok = true;
		for (const FlagDescription &description : table) {
			const auto existing =
				std::find_if(contents.Rows.begin(), contents.Rows.end(), [&description](const Row &row) {
					return row.Description.Name == description.Name;
				});
			if (existing != contents.Rows.end()) {
				ENGINE_ERROR("flags: '{}' is declared twice", description.Name);
				ok = false;
				continue;
			}

			Row row;
			row.Description = description;
			if (!Interpret(row, description.Default)) {
				// A default that is not a value of its own kind is a bug in the
				// declaring table, so it is loud rather than silently zero.
				ENGINE_ERROR(
					"flags: '{}' declares a default of '{}', which is not a value of its kind",
					description.Name,
					description.Default
				);
				ok = false;
			}
			contents.Rows.push_back(std::move(row));
		}
		return ok;
	}

	std::span<const FlagDescription> Flags::Declared() {
		// The descriptions are interleaved with the values, so this cannot hand
		// back a span over the rows. It is called by a listing and by a test,
		// neither of which is in a tick.
		static std::vector<FlagDescription> descriptions;
		static uint32_t builtAt = 0;
		static size_t builtCount = 0;

		Table &contents = Contents();
		const std::lock_guard<std::mutex> held(contents.Lock);

		if (builtAt != contents.Generation || builtCount != contents.Rows.size()) {
			descriptions.clear();
			descriptions.reserve(contents.Rows.size());
			for (const Row &row : contents.Rows) {
				descriptions.push_back(row.Description);
			}
			builtAt = contents.Generation;
			builtCount = contents.Rows.size();
		}
		return descriptions;
	}

	bool Flags::Has(std::string_view name) {
		Table &contents = Contents();
		const std::lock_guard<std::mutex> held(contents.Lock);
		return std::any_of(contents.Rows.begin(), contents.Rows.end(), [name](const Row &row) {
			return row.Description.Name == name;
		});
	}

	FlagStatus Flags::Set(std::string_view name, std::string_view text, FlagSource source) {
		Table &contents = Contents();
		const std::lock_guard<std::mutex> held(contents.Lock);

		if (contents.Frozen) {
			ENGINE_WARN("flags: '{}' was set after the flags were frozen, and the value was ignored", name);
			return FlagStatus::Frozen;
		}

		const auto found = std::find_if(contents.Rows.begin(), contents.Rows.end(), [name](const Row &row) {
			return row.Description.Name == name;
		});
		if (found == contents.Rows.end()) {
			return FlagStatus::NoSuchFlag;
		}

		// **Strictly less, so a later value from the same source wins.** Two
		// `--content-gif` on one command line is the same person saying it
		// twice, and the second is what they meant; a config file and a command
		// line disagreeing is not. A `List` reads the same comparison as
		// "append rather than replace" - see `Apply`.
		if (found->Source > source) {
			return FlagStatus::Outranked;
		}

		if (!Apply(*found, text, source)) {
			return FlagStatus::NotAValue;
		}
		found->Source = source;
		return FlagStatus::Applied;
	}

	void Flags::Freeze() {
		Table &contents = Contents();
		const std::lock_guard<std::mutex> held(contents.Lock);
		contents.Frozen = true;
	}

	bool Flags::Frozen() {
		Table &contents = Contents();
		const std::lock_guard<std::mutex> held(contents.Lock);
		return contents.Frozen;
	}

	void Flags::Reset() {
		Table &contents = Contents();
		const std::lock_guard<std::mutex> held(contents.Lock);
		contents.Rows.clear();
		contents.Frozen = false;
		contents.Generation++;
	}

	uint32_t Flags::Generation() {
		Table &contents = Contents();
		const std::lock_guard<std::mutex> held(contents.Lock);
		return contents.Generation;
	}

	std::string Flags::Listing() {
		Table &contents = Contents();
		const std::lock_guard<std::mutex> held(contents.Lock);

		// Sorted by name rather than by declaration order, because a person
		// reading a hundred rows wants to find one and a program declared them
		// in whatever order its registrars ran.
		std::vector<const Row *> sorted;
		sorted.reserve(contents.Rows.size());
		for (const Row &row : contents.Rows) {
			sorted.push_back(&row);
		}
		std::sort(sorted.begin(), sorted.end(), [](const Row *left, const Row *right) {
			return left->Description.Name < right->Description.Name;
		});

		size_t widest = 0;
		for (const Row *row : sorted) {
			widest = std::max(widest, row->Description.Name.size());
		}

		std::string listing;
		for (const Row *row : sorted) {
			listing.append(row->Description.Name);
			listing.append(widest + 2 - row->Description.Name.size(), ' ');
			listing.append(row->Text);
			listing.append("  (");
			listing.append(Describe(row->Source));
			listing.append(")  ");
			listing.append(row->Description.Description);
			listing.push_back('\n');
		}
		return listing;
	}

	FlagTableBuilder &FlagTableBuilder::Add(
		std::string_view name, FlagKind kind, std::string value, std::string_view description
	) {
		const std::string &ownedName = Storage.emplace_back(name);
		const std::string &ownedValue = Storage.emplace_back(std::move(value));
		const std::string &ownedDescription = Storage.emplace_back(description);

		FlagDescription row;
		row.Name = ownedName;
		row.Kind = kind;
		row.Default = ownedValue;
		row.Description = ownedDescription;
		Descriptions.push_back(row);
		return *this;
	}

	FlagTableBuilder &
	FlagTableBuilder::Boolean(std::string_view name, bool value, std::string_view description) {
		return Add(name, FlagKind::Boolean, value ? "true" : "false", description);
	}

	FlagTableBuilder &
	FlagTableBuilder::Integer(std::string_view name, int64_t value, std::string_view description) {
		return Add(name, FlagKind::Integer, std::to_string(value), description);
	}

	FlagTableBuilder &
	FlagTableBuilder::Number(std::string_view name, double value, std::string_view description) {
		// **Not `std::to_string`**, which is `%f` and turns 1e-7 into `0.000000`
		// - a default that reads back as zero. Sixteen significant digits is
		// what round-trips a double.
		std::array<char, 32> text{};
		const int written = std::snprintf(text.data(), text.size(), "%.17g", value);
		return Add(
			name,
			FlagKind::Number,
			std::string(text.data(), written > 0 ? static_cast<size_t>(written) : 0),
			description
		);
	}

	FlagTableBuilder &
	FlagTableBuilder::Text(std::string_view name, std::string_view value, std::string_view description) {
		return Add(name, FlagKind::Text, std::string(value), description);
	}

	FlagTableBuilder &FlagTableBuilder::List(std::string_view name, std::string_view description) {
		return Add(name, FlagKind::List, std::string(), description);
	}

	int32_t Flag::Resolve() const {
		Table &contents = Contents();
		const std::lock_guard<std::mutex> held(contents.Lock);

		if (Index != UNRESOLVED && ResolvedAt == contents.Generation) {
			return Index;
		}

		const auto found = std::find_if(contents.Rows.begin(), contents.Rows.end(), [this](const Row &row) {
			return row.Description.Name == Wanted;
		});

		if (found == contents.Rows.end()) {
			// Once per resolution rather than once per read, which is what
			// makes this safe to leave in a path something calls per entity.
			ENGINE_WARN("flags: nothing declares '{}', so every read of it answers zero", Wanted);
			Index = -1;
		} else {
			Index = static_cast<int32_t>(found - contents.Rows.begin());
		}
		ResolvedAt = contents.Generation;
		return Index;
	}

	bool Flag::IsValid() const {
		return Resolve() >= 0;
	}

	std::string_view Flag::Name() const {
		const int32_t index = Resolve();
		if (index < 0) {
			return {};
		}
		return Contents().Rows[static_cast<size_t>(index)].Description.Name;
	}

	bool Flag::Boolean() const {
		const int32_t index = Resolve();
		return index < 0 ? false : Contents().Rows[static_cast<size_t>(index)].Boolean;
	}

	int64_t Flag::Integer() const {
		const int32_t index = Resolve();
		return index < 0 ? 0 : Contents().Rows[static_cast<size_t>(index)].Integer;
	}

	double Flag::Number() const {
		const int32_t index = Resolve();
		return index < 0 ? 0.0 : Contents().Rows[static_cast<size_t>(index)].Number;
	}

	std::string_view Flag::Text() const {
		const int32_t index = Resolve();
		return index < 0 ? std::string_view{} : Contents().Rows[static_cast<size_t>(index)].Text;
	}

	std::span<const std::string> Flag::Items() const {
		const int32_t index = Resolve();
		if (index < 0) {
			return {};
		}
		return Contents().Rows[static_cast<size_t>(index)].Items;
	}

	FlagSource Flag::Source() const {
		const int32_t index = Resolve();
		return index < 0 ? FlagSource::Default : Contents().Rows[static_cast<size_t>(index)].Source;
	}
}
