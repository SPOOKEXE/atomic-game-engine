// The scanner, and mostly the things it refuses.
//
// **The interesting tests here are the negative ones.** A reader that scans a
// well-formed document is table stakes; a reader that is handed a file from the
// internet and does not read `/etc/passwd` is the reason this is hand-written
// rather than vendored. Every refusal below is a named XML attack.
//
// The three callers keep their own suites and they are not duplicates of this
// one: `game/tests/Xml.cpp`, `bake/tests/RobloxModel.cpp` and
// `bake/tests/Image.cpp` drive the same attacks through a save file, a model
// and a drawing, because a defence that holds in the scanner and is not reached
// by a caller is a defence nobody has.

#include <engine/core/Xml.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.core.xml")

namespace xml = engine::core::xml;

using engine::core::xml::Attribute;
using engine::core::xml::Failure;
using engine::core::xml::Fault;
using engine::core::xml::Options;
using engine::core::xml::Scan;
using engine::core::xml::Tag;

namespace {
	constexpr Options DEFAULTS{};

	// Every tag in a document, or the refusal that ended the scan.
	std::vector<std::string> Tags(std::string_view document, Failure &failure, const Options &options) {
		std::vector<std::string> names;
		std::string_view text = document;

		for (;;) {
			Tag tag;
			const Scan scan = xml::NextTag(text, options, tag, failure);
			if (scan != Scan::Tag) {
				return names;
			}
			names.emplace_back(std::string(tag.Closing ? "/" : "") + std::string(tag.Name));
		}
	}

	// The refusal a document ends with, whatever it was scanning at the time.
	Failure Refusal(std::string_view document, const Options &options = DEFAULTS) {
		Failure failure;
		Tags(document, failure, options);
		return failure;
	}

	bool Mentions(const Failure &failure, std::string_view text) {
		return failure.Message.find(text) != std::string::npos;
	}
}

TEST_CASE("a document scans into tags, attributes and text", "[core][xml]") {
	Failure failure;
	const std::vector<std::string> names = Tags(
		R"(<?xml version="1.0"?><!-- a comment --><Game format="1"><World /></Game>)", failure, DEFAULTS
	);

	CHECK(failure.Reason == Fault::None);
	CHECK(names == std::vector<std::string>{"Game", "World", "/Game"});

	std::string_view text = R"(<Game format="1" name="a &amp; b">between<World /></Game>)";
	Tag tag;
	REQUIRE(xml::NextTag(text, DEFAULTS, tag, failure) == Scan::Tag);

	std::vector<Attribute> attributes;
	REQUIRE(xml::ReadAttributes(tag.Attributes, DEFAULTS, attributes, failure));
	REQUIRE(attributes.size() == 2);
	CHECK(xml::Find(attributes, "format")->Value == "1");

	// Still escaped: unescaping is the caller's, on the values it actually uses.
	CHECK(xml::Find(attributes, "name")->Value == "a &amp; b");
	CHECK(xml::Find(attributes, "missing") == nullptr);

	std::string content;
	REQUIRE(xml::ReadContent(text, DEFAULTS, content, failure));
	CHECK(content == "between");
}

TEST_CASE("a doctype is refused rather than parsed", "[core][xml]") {
	// **Billion laughs, and every variant of it.** The attack is an entity
	// declared in terms of itself ten times over, so a two-kilobyte document
	// expands to gigabytes — and there is no version of "expand entities safely"
	// simpler than not having entities. Refusing the declaration removes the
	// whole family in one line.
	const Failure bomb = Refusal(
		R"(<?xml version="1.0"?><!DOCTYPE lolz [<!ENTITY lol "lol"><!ENTITY lol2 ")"
		R"(&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;">]><a>&lol2;</a>)"
	);
	CHECK(bomb.Reason == Fault::Refused);
	CHECK(Mentions(bomb, "DOCTYPE"));
	CHECK(Mentions(bomb, "ENTITY"));

	// An external entity is the same declaration and the same refusal — and it
	// is a file read, performed by a reader that opens nothing.
	const Failure external = Refusal(R"(<!DOCTYPE a [<!ENTITY x SYSTEM "file:///etc/passwd">]><a/>)");
	CHECK(external.Reason == Fault::Refused);
	CHECK(Mentions(external, "DOCTYPE"));

	// **The second lock, and it has to hold on its own.** No declaration
	// survives the scanner, so a reference to anything but the five predefines
	// names something nobody could have declared — refused where it would have
	// been expanded rather than dropped, because a dropped one makes a bomb look
	// like a file with a typo in it.
	Failure failure;
	std::string out;
	CHECK_FALSE(xml::Unescape("&xxe;", DEFAULTS, out, failure));
	CHECK(failure.Reason == Fault::Refused);
	CHECK(Mentions(failure, "xxe"));

	// And the refusal is `Refused` rather than `Malformed`, which is the whole
	// reason a kind is reported at all: somebody tried something here, where
	// `&#zz;` is a typo.
	CHECK_FALSE(xml::Unescape("&#zz;", DEFAULTS, out, failure));
	CHECK(failure.Reason == Fault::Malformed);
}

TEST_CASE("the five predefined entities and numeric references are read", "[core][xml]") {
	Failure failure;
	std::string out;
	REQUIRE(xml::Unescape("&lt;&amp;&gt;&quot;&apos;", DEFAULTS, out, failure));
	CHECK(out == "<&>\"'");

	// UTF-8, not a byte. A reader that round-tripped a name through `&#233;` and
	// got one byte back would corrupt every non-ASCII string that went through
	// it.
	out.clear();
	REQUIRE(xml::Unescape("&#233;&#x4E2D;", DEFAULTS, out, failure));
	CHECK(out == "é中");

	// A reference past the last code point names nothing, and accumulating one
	// unchecked overflows the accumulator rather than failing.
	out.clear();
	CHECK_FALSE(xml::Unescape("&#x110000;", DEFAULTS, out, failure));
	CHECK(failure.Reason == Fault::Malformed);
	CHECK(Mentions(failure, "code point"));
}

TEST_CASE("nothing here recurses, so depth is the caller's to bound", "[core][xml]") {
	// **Unbounded nesting is the third attack, and a parser that recursed would
	// meet it as a stack overflow with no file named.** `NextTag` is a scan and
	// the caller keeps the stack, so a million levels is a million tags and a
	// count somebody bounded — this case is the proof that the scanner itself
	// does not care.
	constexpr int DEEP = 100000;

	std::string nested;
	for (int level = 0; level < DEEP; level++) {
		nested += "<a>";
	}

	Failure failure;
	const std::vector<std::string> names = Tags(nested, failure, DEFAULTS);
	CHECK(failure.Reason == Fault::None);
	CHECK(names.size() == DEEP);
}

TEST_CASE("a document-wide sweep and a per-reference refusal are different policies", "[core][xml]") {
	// **This is the case that fails if somebody collapses the two.** They exist
	// side by side for one reason: a caller that never unescapes has no point at
	// which a reference would be met, and a caller that reads CDATA has text
	// that is not markup and must not be swept.
	//
	// The document below is the shape of a real `.rbxmx` in this repository's
	// corpus: a Luau pattern `"[&;]"` inside a script's CDATA section. It is a
	// valid file and a sweep refuses it while naming an entity nobody wrote.
	constexpr std::string_view SCRIPT = R"(<Source><![CDATA[local mask = "[&;]"]]></Source>)";

	Failure swept;
	CHECK_FALSE(xml::CheckEntityReferences(SCRIPT, DEFAULTS, swept));
	CHECK(swept.Reason == Fault::Refused);

	std::string_view text = SCRIPT;
	Tag tag;
	Failure failure;
	REQUIRE(xml::NextTag(text, DEFAULTS, tag, failure) == Scan::Tag);

	std::string content;
	REQUIRE(xml::ReadContent(text, DEFAULTS, content, failure));
	CHECK(content == R"(local mask = "[&;]")");

	// And the other direction: outside a CDATA section the per-reference route
	// refuses exactly what the sweep does, so the difference is CDATA and not
	// strictness.
	std::string_view loose = "<Source>&payload;</Source>";
	REQUIRE(xml::NextTag(loose, DEFAULTS, tag, failure) == Scan::Tag);
	CHECK_FALSE(xml::ReadContent(loose, DEFAULTS, content, failure));
	CHECK(failure.Reason == Fault::Refused);
	CHECK(Mentions(failure, "payload"));

	CHECK_FALSE(xml::CheckEntityReferences("<Source>&payload;</Source>", DEFAULTS, failure));
	CHECK(failure.Reason == Fault::Refused);
}

TEST_CASE("markup that is not well formed is refused by kind", "[core][xml]") {
	CHECK(Refusal("<a b>").Reason == Fault::None); // a tag with a bare attribute scans; the run is read later

	Failure failure;
	std::vector<Attribute> attributes;
	CHECK_FALSE(xml::ReadAttributes("b", DEFAULTS, attributes, failure));
	CHECK(failure.Reason == Fault::Malformed);
	CHECK_FALSE(xml::ReadAttributes("b=1", DEFAULTS, attributes, failure));
	CHECK(Mentions(failure, "unquoted"));
	CHECK_FALSE(xml::ReadAttributes(R"(1b="x")", DEFAULTS, attributes, failure));
	CHECK(Mentions(failure, "not an attribute name"));

	// A `<` inside a value is not legal XML, and a scanner that allowed one
	// would read a document whose markup is ambiguous to everything else.
	CHECK_FALSE(xml::ReadAttributes(R"(b="<c>")", DEFAULTS, attributes, failure));
	CHECK(Mentions(failure, "'<'"));

	// Every count a document states is checked before the vector holding it
	// grows.
	Options two = DEFAULTS;
	two.MaximumAttributes = 2;
	CHECK_FALSE(xml::ReadAttributes(R"(a="1" b="2" c="3")", two, attributes, failure));
	CHECK(failure.Reason == Fault::TooManyAttributes);

	// A name that is not a name, which is where a scanner that stopped at the
	// first space would take `<1 2 3>` for an element.
	CHECK(Refusal("<1abc/>").Reason == Fault::Malformed);
	CHECK(Refusal("<a><!-- never closed").Reason == Fault::Truncated);
	CHECK(Refusal("<a><![CDATA[never closed").Reason == Fault::Truncated);
	CHECK(Refusal("<a").Reason == Fault::Truncated);
	CHECK(Refusal("<>").Reason == Fault::Malformed);
}

TEST_CASE("a namespace prefix is dropped only when a caller asked", "[core][xml]") {
	// **A prefix means nothing to a drawing and might mean something to a save
	// file**, so it is the caller's claim rather than the scanner's default:
	// `svg:rect` is a rect, and `x:Game` is not this engine's `Game`.
	Options dropping = DEFAULTS;
	dropping.DropNamespacePrefix = true;

	Failure failure;
	CHECK(Tags("<svg:rect/>", failure, dropping) == std::vector<std::string>{"rect"});
	CHECK(Tags("<svg:rect/>", failure, DEFAULTS) == std::vector<std::string>{"svg:rect"});

	// The attribute keeps its prefix either way: a prefix on an attribute is
	// meaningful where a prefix on an element is not.
	std::string_view text = R"(<svg:svg xmlns:xlink="http://example" />)";
	Tag tag;
	REQUIRE(xml::NextTag(text, dropping, tag, failure) == Scan::Tag);

	std::vector<Attribute> attributes;
	REQUIRE(xml::ReadAttributes(tag.Attributes, dropping, attributes, failure));
	REQUIRE(attributes.size() == 1);
	CHECK(attributes[0].Name == "xmlns:xlink");
}

TEST_CASE("the format's name is what a refusal calls itself", "[core][xml]") {
	// "svg: a tag is never closed" is what the author of a drawing needs; "xml:"
	// would leave them wondering which of their files it was about.
	Options drawing = DEFAULTS;
	drawing.Format = "svg";
	CHECK(Refusal("<a", drawing).Message.starts_with("svg: "));
	CHECK(Refusal("<a").Message.starts_with("xml: "));
}
