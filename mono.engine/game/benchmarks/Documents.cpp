// What a save file costs to read and write.
//
// **Loading is a player waiting.** Opening a place in the studio, or a server
// bringing a world up on start, is one pass of this code over a whole document
// — and unlike a frame, there is no next one to make up for it. A parser that
// costs a microsecond per element turns a hundred-thousand-instance place into
// a tenth of a second before anything is constructed, which is the difference
// between a tool that feels instant and one that does not.
//
// **The element counts run to 200 000 because a place file reaches it.** A
// world is instances inside instances, each carrying a handful of properties,
// and every property is an element. So the interesting figure is per element
// rather than per byte, and the ladder exists to say whether parsing is linear
// in the document — a parser that resolves entities by rescanning, or that
// looks an attribute up by walking a list, is quadratic in a way no small test
// document reveals.
//
// **The refusal rows matter here for a different reason than in `net`.** A
// document is not usually hostile; it is usually *old*, or truncated by a disk
// that filled, or written by a build that has since changed. But `XmlLimits`
// bounds depth, size and element count precisely because a document can be
// hostile — a place file is content, and content arrives from other people. A
// parser that costs more to refuse a 300-deep document than to accept a
// 200-deep one is one that can be made to spend a long time saying no.
//
// The `Values` rows are the other half of a save: a property is formatted to
// text on the way out and parsed back on the way in, once each per property per
// instance. `FormatNumber`'s own comment explains why it is not
// `std::to_string`; this is what that decision costs.

#include <engine/ecs/Classes.hpp>
#include <engine/game/Values.hpp>
#include <engine/game/Xml.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.game.bench.documents")

using engine::ecs::PropertyType;
using engine::game::FormatNumber;
using engine::game::FormatValue;
using engine::game::ParseValue;
using engine::game::ParseXml;
using engine::game::PropertyValue;
using engine::game::TypeFromTag;
using engine::game::TypeTag;
using engine::game::XmlDocument;
using engine::game::XmlLimits;
using engine::game::XmlStatus;
using engine::testing::Consume;

namespace documents_bench {

	// A document shaped like a place file: instances nested a few deep, each
	// carrying properties as child elements with attributes.
	//
	// **Attributes and text both, because they are parsed by different code.**
	// A document of bare tags would measure the tokeniser and nothing else,
	// and attribute parsing is where entity resolution and quoting live.
	const std::string &DocumentOf(size_t instances) {
		static std::vector<std::pair<size_t, std::string>> built;
		for (const auto &[count, text] : built) {
			if (count == instances) {
				return text;
			}
		}

		std::string made;
		made.reserve(instances * 200);
		made += "<Place version=\"1\">\n";

		for (size_t index = 0; index < instances; index++) {
			// Three levels of nesting every eight instances, so the tree has
			// real depth rather than being one flat list under the root.
			const bool opens = (index % 8) == 0;
			if (opens) {
				made += "<Instance class=\"Model\" name=\"Group";
				made += std::to_string(index);
				made += "\">\n";
			}

			made += "<Instance class=\"Part\" name=\"Part";
			made += std::to_string(index);
			made += "\">\n";
			made += "<Property name=\"Position\" type=\"Vector3\">";
			made += std::to_string(index);
			made += ",0,0</Property>\n";
			made += "<Property name=\"Anchored\" type=\"bool\">true</Property>\n";
			// An entity, so the unescaping path runs on a real document rather
			// than only on a contrived one.
			made += "<Property name=\"Name\" type=\"string\">Part &amp; Piece</Property>\n";
			made += "</Instance>\n";

			if ((index % 8) == 7) {
				made += "</Instance>\n";
			}
		}

		// Close any group left open by the loop ending mid-run.
		if ((instances % 8) != 0) {
			made += "</Instance>\n";
		}
		made += "</Place>\n";

		built.emplace_back(instances, std::move(made));
		return built.back().second;
	}

	// A document reused across parses, the way a loader reuses one. `ParseXml`
	// fills a caller's document precisely so its vectors keep their capacity,
	// and parsing into a fresh one each time would measure the allocator.
	XmlDocument &Parsed() {
		static XmlDocument document;
		return document;
	}

	// A nesting of `depth` elements and nothing else, for the depth-limit rows.
	std::string NestedTo(uint32_t depth) {
		std::string made;
		made.reserve(depth * 16);
		for (uint32_t level = 0; level < depth; level++) {
			made += "<n>";
		}
		for (uint32_t level = 0; level < depth; level++) {
			made += "</n>";
		}
		return made;
	}
}

using namespace documents_bench;

// --- parsing --------------------------------------------------------------------
//
// One iteration is one *instance*, and each instance is five elements — so
// divide by five for a per-element figure. Instances rather than elements
// because that is the unit somebody counts when they say how big their place
// is.

BENCH("ParseXml · 1k instances", 1000) {
	const std::string &text = DocumentOf(1000);
	Consume(ParseXml(text, Parsed()) == XmlStatus::Ok);
	Consume(Parsed().Elements.size());
}

BENCH("ParseXml · 20k instances", 20'000) {
	const std::string &text = DocumentOf(20'000);
	Consume(ParseXml(text, Parsed()) == XmlStatus::Ok);
	Consume(Parsed().Elements.size());
}

BENCH("ParseXml · 200k instances", 200'000) {
	// **A large place file, and the row the ladder exists for.** A flat
	// per-instance figure from 1k up to here means parsing is linear and a big
	// world merely takes proportionally longer. A climbing one means something
	// is being searched per element — an attribute lookup that walks, an entity
	// resolver that rescans — and the load time of a big place is quadratic in
	// a way no small document could have shown.
	const std::string &text = DocumentOf(200'000);
	Consume(ParseXml(text, Parsed()) == XmlStatus::Ok);
	Consume(Parsed().Elements.size());
}

BENCH("ParseXml · 20k instances into a fresh document", 20'000) {
	// What a loader pays for not reusing its `XmlDocument`. Every string and
	// every child-index vector is allocated from nothing, and there are five
	// elements per instance each holding several. The gap against the reused row
	// is the argument for the fill-in-place signature.
	const std::string &text = DocumentOf(20'000);
	XmlDocument document;
	Consume(ParseXml(text, document) == XmlStatus::Ok);
	Consume(document.Elements.size());
}

// --- reading the tree back --------------------------------------------------------

BENCH("XmlElement::Attribute · 100k lookups", 100'000) {
	// **Called once per attribute per element by every loader**, and it is a
	// linear walk of two parallel vectors by design — which is right for the two
	// or three attributes an element actually has, and would be badly wrong if
	// something started writing dozens. This row is what makes that assumption
	// checkable rather than implicit.
	const std::string &text = DocumentOf(1000);
	XmlDocument &document = Parsed();
	Consume(ParseXml(text, document) == XmlStatus::Ok);

	size_t bytes = 0;
	for (size_t index = 0; index < 100'000; index++) {
		const engine::game::XmlElement &element =
			document.Elements[index % document.Elements.size()];
		bytes += element.Attribute("name").size();
		bytes += element.Attribute("class").size();
	}
	Consume(bytes);
}

// --- the limits -------------------------------------------------------------------
//
// **Refusing has to be cheaper than accepting**, for the reason every refusal
// row in this repository gives: a document is content, and content arrives from
// other people.

BENCH("ParseXml · 10k documents nested past the depth limit", 10'000) {
	// 300 deep against a limit of 256. The parser should stop at the limit
	// rather than at the end of the document, so this must cost about what 256
	// levels cost and not what 300 do — and it must not cost what a *stack*
	// 300 deep costs, which is the recursive implementation this bounds against.
	static const std::string tooDeep = NestedTo(300);
	XmlDocument &document = Parsed();
	uint32_t accepted = 0;
	for (size_t index = 0; index < 10'000; index++) {
		accepted += ParseXml(tooDeep, document) == XmlStatus::Ok ? 1u : 0u;
	}
	Consume(accepted);
}

BENCH("ParseXml · 10k documents just inside the depth limit", 10'000) {
	// The accepting twin, at 250. Read the two together: a refusal much dearer
	// than this is a refusal that did work before deciding.
	static const std::string deep = NestedTo(250);
	XmlDocument &document = Parsed();
	uint32_t accepted = 0;
	for (size_t index = 0; index < 10'000; index++) {
		accepted += ParseXml(deep, document) == XmlStatus::Ok ? 1u : 0u;
	}
	Consume(accepted);
}

BENCH("ParseXml · 10k truncated documents", 10'000) {
	static const std::string truncated = [] {
		const std::string &whole = DocumentOf(1000);
		return whole.substr(0, whole.size() / 2);
	}();

	XmlDocument &document = Parsed();
	uint32_t accepted = 0;
	for (size_t index = 0; index < 10'000; index++) {
		accepted += ParseXml(truncated, document) == XmlStatus::Ok ? 1u : 0u;
	}
	Consume(accepted);
}

BENCH("ParseXml · 10k documents over the element limit", 10'000) {
	// A tiny limit against an ordinary document, so the parser hits the bound
	// early. **It must stop there rather than parsing the rest and checking
	// afterwards** — the whole purpose of a bound is that the work is not done.
	// If this row costs anything like `ParseXml · 1k instances`, it is being
	// checked at the end.
	static const std::string text = DocumentOf(1000);
	static const XmlLimits tight = [] {
		XmlLimits limits;
		limits.MaximumElements = 16;
		return limits;
	}();

	XmlDocument &document = Parsed();
	uint32_t accepted = 0;
	for (size_t index = 0; index < 10'000; index++) {
		accepted += ParseXml(text, document, tight) == XmlStatus::Ok ? 1u : 0u;
	}
	Consume(accepted);
}

// --- values ------------------------------------------------------------------------
//
// The other half of a save: once per property per instance, each way.

BENCH("FormatNumber · 100k", 100'000) {
	// **Not `std::to_string`**, which is `%f` and writes 60 as "60.000000". The
	// shortest round-tripping form costs more than a fixed one, and this row is
	// how much — multiplied by every number in a place file, twice, if a tool
	// both writes and reloads.
	double value = 0.5;
	size_t bytes = 0;
	for (size_t index = 0; index < 100'000; index++) {
		value = value * 1.000001 + 0.000001;
		bytes += FormatNumber(value).size();
	}
	Consume(bytes);
}

BENCH("FormatValue · 100k Vector3", 100'000) {
	PropertyValue value;
	value.Type = PropertyType::Vector3;
	size_t bytes = 0;
	for (size_t index = 0; index < 100'000; index++) {
		value.Vector3 = engine::core::Vector3(
			static_cast<float>(index), static_cast<float>(index) * 0.5f, static_cast<float>(index) * 0.25f
		);
		bytes += FormatValue(value).size();
	}
	Consume(bytes);
}

BENCH("ParseValue · 100k Vector3", 100'000) {
	PropertyValue out;
	std::string reason;
	uint32_t parsed = 0;
	for (size_t index = 0; index < 100'000; index++) {
		parsed += ParseValue(PropertyType::Vector3, "12.5, -3.25, 8.0", out, reason) ? 1u : 0u;
	}
	Consume(parsed);
}

BENCH("ParseValue · 100k rejected Vector3", 100'000) {
	// The path somebody typing into a field takes on every keystroke that is not
	// yet a valid vector. It fills `reason` with a message — "expected x, y, z"
	// — and building a string per rejection is the thing to watch: an editor
	// calling this per character is allocating per character.
	PropertyValue out;
	std::string reason;
	uint32_t parsed = 0;
	for (size_t index = 0; index < 100'000; index++) {
		parsed += ParseValue(PropertyType::Vector3, "12.5, -3.2", out, reason) ? 1u : 0u;
	}
	Consume(parsed);
	Consume(reason.size());
}

BENCH("TypeTag and TypeFromTag · 100k round trips", 100'000) {
	// Read once per property element while loading. `TypeFromTag` is a lookup
	// by string; whether that is a map or a chain of comparisons is invisible
	// from the header, and at one call per property per instance the difference
	// is a measurable share of a load.
	uint32_t matched = 0;
	for (size_t index = 0; index < 100'000; index++) {
		const auto type = static_cast<PropertyType>(index % 8);
		PropertyType back{};
		if (TypeFromTag(TypeTag(type), back)) {
			matched += back == type ? 1u : 0u;
		}
	}
	Consume(matched);
}
