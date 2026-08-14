// The parser, and mostly the things it refuses.
//
// **The interesting tests here are the negative ones.** A reader that parses a
// well-formed document is table stakes; a reader that is handed a game file
// from the internet and does not read `/etc/passwd` is the reason this file is
// hand-written rather than vendored. Every refusal below is a named XML attack.

#include <engine/game/Xml.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.game.xml")

using engine::game::EscapeXml;
using engine::game::ParseXml;
using engine::game::XmlDocument;
using engine::game::XmlLimits;
using engine::game::XmlStatus;
using engine::game::XmlWriter;

TEST_CASE("a document parses into elements, attributes and text", "[game][xml]") {
	XmlDocument document;
	const XmlStatus status = ParseXml(
		R"(<?xml version="1.0"?>
		<Game format="1" name="Test">
			<World name="Start"><Item class="Part" name="A" /></World>
		</Game>)",
		document
	);

	REQUIRE(status == XmlStatus::Ok);
	REQUIRE(document.Root() != nullptr);
	CHECK(document.Root()->Name == "Game");
	CHECK(document.Root()->Attribute("format") == "1");
	CHECK(document.Root()->Attribute("name") == "Test");

	REQUIRE(document.Root()->Children.size() == 1);
	const auto *world = document.At(document.Root()->Children[0]);
	REQUIRE(world != nullptr);
	CHECK(world->Name == "World");

	REQUIRE(world->Children.size() == 1);
	const auto *item = document.At(world->Children[0]);
	REQUIRE(item != nullptr);
	CHECK(item->Attribute("class") == "Part");
}

TEST_CASE("a doctype is refused rather than parsed", "[game][xml]") {
	// **Billion laughs, and every variant of it.** The attack is an entity
	// declared in terms of itself ten times over, so a two-kilobyte document
	// expands to gigabytes — and there is no version of "expand entities
	// safely" simpler than not having entities. Refusing the DOCTYPE removes
	// the whole family in one line.
	XmlDocument document;
	const XmlStatus status = ParseXml(
		R"(<!DOCTYPE lolz [<!ENTITY lol "lol">]>
		<Game format="1" />)",
		document
	);

	CHECK(status == XmlStatus::Refused);
	CHECK(document.Elements.empty());
}

TEST_CASE("an undeclared entity is refused, not ignored", "[game][xml]") {
	// XXE: `<!ENTITY xxe SYSTEM "file:///etc/passwd">` and then `&xxe;`. The
	// declaration is already refused above; this is the reference, refused on
	// its own so that a document which somehow got one past is still stopped at
	// the point of use.
	//
	// **Counted as `Refused` and not `Malformed`**, because a document that
	// names an entity is a document that expected a declaration — somebody
	// tried something, rather than something being wrong.
	XmlDocument document;
	CHECK(ParseXml(R"(<Game format="1">&xxe;</Game>)", document) == XmlStatus::Refused);
}

TEST_CASE("the five predefined entities and numeric references are read", "[game][xml]") {
	XmlDocument document;
	REQUIRE(ParseXml(R"(<T a="&lt;&amp;&gt;&quot;&apos;">&#233;&#x4E2D;</T>)", document) == XmlStatus::Ok);

	CHECK(document.Root()->Attribute("a") == "<&>\"'");

	// UTF-8, not a byte. A save file that round-tripped a player's name through
	// `&#233;` and got one byte back would corrupt every non-ASCII string in
	// the game.
	CHECK(document.Root()->Text == "é中");
}

TEST_CASE("nesting past the limit is its own refusal", "[game][xml]") {
	// A stack-exhaustion attack is a *well-formed* document, which is why this
	// is `TooDeep` and not `Malformed` — a refusal that said "malformed" would
	// send whoever read the log looking for a typo.
	std::string text = "<a>";
	for (int depth = 0; depth < 64; depth++) {
		text += "<b>";
	}
	for (int depth = 0; depth < 64; depth++) {
		text += "</b>";
	}
	text += "</a>";

	XmlLimits limits;
	limits.MaximumDepth = 8;

	XmlDocument document;
	CHECK(ParseXml(text, document, limits) == XmlStatus::TooDeep);

	// And the same document under a limit that allows it.
	limits.MaximumDepth = 256;
	CHECK(ParseXml(text, document, limits) == XmlStatus::Ok);
}

TEST_CASE("size and element limits refuse before the work is done", "[game][xml]") {
	XmlLimits limits;
	limits.MaximumBytes = 8;

	XmlDocument document;
	CHECK(ParseXml("<Game format=\"1\" />", document, limits) == XmlStatus::TooLarge);

	limits = XmlLimits{};
	limits.MaximumElements = 2;
	CHECK(ParseXml("<a><b /><c /><d /></a>", document, limits) == XmlStatus::TooManyElements);
}

TEST_CASE("a mismatched or truncated document is refused", "[game][xml]") {
	XmlDocument document;
	CHECK(ParseXml("<a></b>", document) == XmlStatus::Mismatched);
	CHECK(ParseXml("<a><b>", document) == XmlStatus::Truncated);
	CHECK(ParseXml("<a attr>", document) == XmlStatus::Malformed);
}

TEST_CASE("CDATA survives a closing sequence inside it", "[game][xml]") {
	// **The one that corrupts a file silently if it is wrong.** A program
	// containing `]]>` is ordinary code — two array closes and a comparison —
	// and a writer that emitted it raw would end the section early and drop the
	// rest of the script into the document as markup.
	const std::string program = "local x = a[b[c]]> 3";

	XmlWriter writer;
	writer.Open("Source");
	writer.Verbatim(program);
	writer.Close();

	XmlDocument document;
	REQUIRE(ParseXml(writer.Finish(), document) == XmlStatus::Ok);
	CHECK(document.Root()->Text == program);
}

TEST_CASE("a script's ampersand is source and not a reference", "[game][xml]") {
	// **A save file's scripts are CDATA and CDATA is text**, so the refusal of
	// an undeclared entity sits at each point a reference is actually read
	// rather than in a sweep over the document. A sweep would refuse this file
	// while naming an entity nobody wrote — which is the bug a real `.rbxmx` in
	// this repository's corpus found, one format along, and the reason
	// `core/Xml.hpp` keeps the two policies apart.
	const std::string program = "local mask = \"[&;]\"\nif a and b then end";

	XmlWriter writer;
	writer.Open("Source");
	writer.Verbatim(program);
	writer.Close();

	XmlDocument document;
	REQUIRE(ParseXml(writer.Finish(), document) == XmlStatus::Ok);
	CHECK(document.Root()->Text == program);

	// Outside a section it is still a reference, and still refused. What is
	// exempt is CDATA, not this format.
	CHECK(ParseXml("<Source>local mask = &mask;</Source>", document) == XmlStatus::Refused);
}

TEST_CASE("character data before the root is refused", "[game][xml]") {
	// A document that begins with text is not one this format wrote, and a CDATA
	// section is character data however much of markup's punctuation it borrows —
	// the scanner steps over one as text, so the refusal is the loader's to make.
	XmlDocument document;
	CHECK(ParseXml("junk<Game format=\"1\" />", document) == XmlStatus::Malformed);
	CHECK(ParseXml("<![CDATA[junk]]><Game format=\"1\" />", document) == XmlStatus::Malformed);

	// **What is deliberately not asserted here is text between the declaration
	// and the root**, which this reads and the v0.7 one refused. Catching it
	// needs the scanner to stop after each comment it steps over, or this file to
	// know what a comment looks like, and neither is worth it for a run of text
	// that expands to nothing: `D00128` records it as the one thing the move down
	// widened.
	CHECK(ParseXml("<?xml version=\"1.0\"?><Game format=\"1\" />", document) == XmlStatus::Ok);
}

TEST_CASE("an element carrying more attributes than the limit is refused", "[game][xml]") {
	// **The one count this reader used to take on trust**, closed when the
	// scanner moved down at `D00128`: every other number a document states is
	// checked before the vector holding it grows, and this is now the same. The
	// default is far past the nine a `<Game>` carries, so the limit is driven
	// from a test rather than from a document nobody would write.
	std::string element = "<Game";
	for (int index = 0; index < 8; index++) {
		element += " a" + std::to_string(index) + "=\"1\"";
	}
	element += " />";

	XmlLimits limits;
	limits.MaximumAttributes = 4;

	XmlDocument document;
	CHECK(ParseXml(element, document, limits) == XmlStatus::TooManyAttributes);
	CHECK(document.Elements.empty());

	limits.MaximumAttributes = 8;
	CHECK(ParseXml(element, document, limits) == XmlStatus::Ok);
	CHECK(document.Root()->AttributeNames.size() == 8);
}

TEST_CASE("what the writer writes, the parser reads back", "[game][xml]") {
	XmlWriter writer;
	writer.Open("Game");
	writer.Attribute("format", "1");
	writer.Attribute("name", "a \"quoted\" & <angled> name");
	writer.Open("Empty");
	writer.Close();
	writer.Open("Value");
	writer.Text("4, 1, 2");
	writer.Close();
	writer.Close();

	REQUIRE_FALSE(writer.Failed());

	XmlDocument document;
	REQUIRE(ParseXml(writer.Finish(), document) == XmlStatus::Ok);
	CHECK(document.Root()->Attribute("name") == "a \"quoted\" & <angled> name");

	REQUIRE(document.Root()->Children.size() == 2);
	CHECK(document.At(document.Root()->Children[0])->Name == "Empty");
	CHECK(document.At(document.Root()->Children[1])->Text == "4, 1, 2");
}

TEST_CASE("an unbalanced or out-of-order write is reported", "[game][xml]") {
	// A save file that cannot be loaded is worse than one missing a field, so
	// the writer refuses to produce a document that will not re-parse and says
	// so rather than doing its best.
	XmlWriter unbalanced;
	unbalanced.Open("a");
	unbalanced.Finish();
	CHECK(unbalanced.Failed());

	XmlWriter late;
	late.Open("a");
	late.Open("b");
	late.Close();
	late.Attribute("after", "the child");
	late.Close();
	CHECK(late.Failed());
}

TEST_CASE("escaping covers exactly the five reserved characters", "[game][xml]") {
	CHECK(EscapeXml("<&>\"'") == "&lt;&amp;&gt;&quot;&apos;");
	CHECK(EscapeXml("plain text") == "plain text");
}
