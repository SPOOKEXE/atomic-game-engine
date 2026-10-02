#include <engine/bake/ComposerXml.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.bake.composer_xml")
TEST_CASE(
	"Native XML parsing preserves source root prolog and distinct attribute text entity rules",
	"[bake][composer]"
) {
	engine::bake::ComposerXml xml;
	std::string failure;
	REQUIRE(
		engine::bake::ReadComposerXml(
			"<?xml version=\"1.0\"?><root a=\"&amp;\"><child>text &amp; raw</child></root>", xml, failure
		)
	);
	CHECK(xml.Root.Type == "root");
	REQUIRE(xml.Prolog);
	CHECK(xml.Prolog->Type == "prolog");
	REQUIRE(xml.Root.Children.size() == 1);
	CHECK(xml.Root.Children[0].Attributes[0].second == "&");
	CHECK(xml.Root.Children[0].Children[0].Text == "text &amp; raw");
	CHECK_FALSE(
		engine::bake::ReadComposerXml("<!DOCTYPE root SYSTEM 'file:///etc/passwd'><root/>", xml, failure)
	);
	CHECK_FALSE(engine::bake::ReadComposerXml("<root><bad></root>", xml, failure));
	CHECK_FALSE(engine::bake::ReadComposerXml("<root/>", xml, failure, 1));
}
