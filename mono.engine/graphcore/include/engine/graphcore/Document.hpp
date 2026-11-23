#pragma once

// The line-oriented text both graph edit logs are saved in: a header line, one
// edit per line, quoted names, and named sections for a set of documents.
//
// `bake::Document` and `graph::PipelineDocument` read and write through this,
// so the two formats cannot drift on how a name is quoted, how a number is
// spelled or where one pipeline of a set stops. Each format still owns its own
// words and fields; this owns the lexing and the framing.
// @tier L2 · shared

#include <engine/core/Name.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::graphcore::text {

	// Appends `text` in quotes. Backslash, quote, newline, carriage return and
	// tab are escaped, so a name can never forge a second line or field.
	void AppendQuoted(std::string &out, std::string_view text);

	// Reads a quoted name after optional spaces, advancing past it. Refuses an
	// escape nothing writes, so every name has exactly one spelling.
	bool TakeQuoted(std::string_view &line, std::string &out);

	// The next space-separated word, or empty at the end of the line.
	std::string_view TakeWord(std::string_view &line);

	// Whole-word numbers. Trailing characters in the word refuse.
	bool TakeUnsigned(std::string_view &line, uint32_t &out);
	bool TakeInteger(std::string_view &line, int32_t &out);
	bool TakeFloat(std::string_view &line, float &out);

	// Shortest text that reads back as the same float.
	void AppendFloat(std::string &out, float value);

	// `yes` or `no`.
	bool TakeFlag(std::string_view &line, bool &out);
	std::string_view FlagText(bool value);

	// True when only spaces remain. A line with spare words is refused by callers.
	bool AtEnd(std::string_view line);

	// The next line without its newline, consuming it from `text`. A trailing
	// `\r` is dropped so a file written on another platform reads the same.
	std::string_view NextLine(std::string_view &text);

	// Reads the header line and returns its position in `accepted`, or -1 when
	// the text is empty or starts with anything else.
	int TakeHeader(std::string_view &text, std::span<const std::string_view> accepted);

	// The word that opens a named section of a set.
	inline constexpr std::string_view SECTION = "pipeline";

	// Appends `pipeline "name"` and a newline.
	void AppendSection(std::string &out, std::string_view name);

	// Reads the lines after a set header. Each `pipeline "name"` line calls
	// `open`; every other nonempty line goes to `line` for the open section. A
	// line outside any section, a bad section line, or a `line` refusal stops
	// with that line in `offender`. Returns true when every line was accepted.
	bool ReadSections(
		std::string_view text,
		const std::function<void(std::string_view name)> &open,
		const std::function<bool(std::string_view line)> &line,
		std::string &offender
	);

	// Named documents of one format, which is what a world carries. Names are
	// kept sorted by text, never by `core::Name::operator<` (interning order),
	// so a set written twice with the same contents gives the same bytes.
	template <typename Document> class DocumentSet {
	  public:
		// Adds or replaces. An invalid or empty name is refused because it could
		// not be written back.
		bool Set(core::Name name, Document document) {
			if (!name.IsValid() || name.Text().empty()) {
				return false;
			}
			if (const size_t found = IndexOf(name); found != Order.size()) {
				Documents[found] = std::move(document);
				return true;
			}
			const auto at = std::lower_bound(Order.begin(), Order.end(), name, ByText);
			const size_t index = static_cast<size_t>(at - Order.begin());
			Order.insert(at, name);
			Documents.insert(Documents.begin() + static_cast<ptrdiff_t>(index), std::move(document));
			return true;
		}

		const Document *Find(core::Name name) const {
			const size_t found = IndexOf(name);
			return found == Order.size() ? nullptr : &Documents[found];
		}

		bool Remove(core::Name name) {
			const size_t found = IndexOf(name);
			if (found == Order.size()) {
				return false;
			}
			Order.erase(Order.begin() + static_cast<ptrdiff_t>(found));
			Documents.erase(Documents.begin() + static_cast<ptrdiff_t>(found));
			return true;
		}

		// Sorted by text. Valid until the next Set, Remove or Clear.
		std::span<const core::Name> Names() const {
			return Order;
		}

		size_t Count() const {
			return Order.size();
		}

		void Clear() {
			Order.clear();
			Documents.clear();
		}

	  private:
		// `Name::Text()` takes the registry lock, so small sets compare ids and
		// large ones binary search by text.
		static constexpr size_t LINEAR_LOOKUP_LIMIT = 4096;

		static bool ByText(core::Name left, core::Name right) {
			return left.Text() < right.Text();
		}

		size_t IndexOf(core::Name name) const {
			if (Order.size() < LINEAR_LOOKUP_LIMIT) {
				return static_cast<size_t>(std::find(Order.begin(), Order.end(), name) - Order.begin());
			}
			const auto at = std::lower_bound(Order.begin(), Order.end(), name, ByText);
			return at != Order.end() && *at == name ? static_cast<size_t>(at - Order.begin()) : Order.size();
		}

		// Parallel vectors so Names can hand back a span.
		std::vector<core::Name> Order;
		std::vector<Document> Documents;
	};
}
