#include <engine/assets/ContentForm.hpp>
#include <engine/assets/ContentPolicy.hpp>
#include <engine/core/Flags.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.assets.contentpolicy")
TEST_DEPENDS("engine.assets.contentform")
TEST_DEPENDS("engine.core.flags")

using engine::assets::AllForms;
using engine::assets::ContentForm;
using engine::assets::ContentPolicy;
using engine::assets::ContentVerb;
using engine::assets::DeclareContentFlags;
using engine::assets::Describe;
using engine::core::Flags;
using engine::core::FlagSource;
using engine::core::FlagStatus;

namespace {
	void Fresh() {
		Flags::Reset();
		REQUIRE(DeclareContentFlags(ContentVerb::Handle));
		REQUIRE(DeclareContentFlags(ContentVerb::Publish));
	}
}

TEST_CASE("a fresh policy allows everything, including what it cannot name", "[content][policy]") {
	const ContentPolicy policy;

	for (const ContentForm form : AllForms()) {
		INFO(Describe(form));
		CHECK(policy.Allows(form));
	}
	CHECK(policy.Allows(ContentForm::Unknown));
	CHECK(policy.RefusedCount() == 0);
	CHECK(policy.RefusedText().empty());
}

TEST_CASE("refusing one form leaves the rest alone", "[content][policy]") {
	ContentPolicy policy;
	policy.Allow(ContentForm::Gif, false);
	policy.Allow(ContentForm::Svg, false);

	CHECK_FALSE(policy.Allows(ContentForm::Gif));
	CHECK_FALSE(policy.Allows(ContentForm::Svg));
	CHECK(policy.Allows(ContentForm::Png));
	CHECK(policy.Allows(ContentForm::ATex));

	// The name form of the question, which is what every caller actually asks.
	CHECK_FALSE(policy.AllowsName("art/logo.gif"));
	CHECK_FALSE(policy.AllowsName("art/logo.SVG"));
	CHECK(policy.AllowsName("art/logo.png"));

	CHECK(policy.RefusedCount() == 2);
	CHECK(policy.RefusedText() == "gif, svg");
}

TEST_CASE("refusing the unnamed form closes the list", "[content][policy]") {
	ContentPolicy policy;
	policy.Allow(ContentForm::Unknown, false);

	// **The one flag that is not about a format.** Everything the build has a
	// row for still passes; anything it does not is now refused, which is how a
	// deployment says "only what this engine understands".
	CHECK(policy.Allows(ContentForm::Png));
	CHECK_FALSE(policy.AllowsName("secrets.tar.zst"));
	CHECK_FALSE(policy.AllowsName("no-extension-at-all"));
	CHECK(policy.RefusedText() == "unknown");
}

TEST_CASE("the two verbs are two flag prefixes over one vocabulary", "[content][policy]") {
	Fresh();

	CHECK(std::string(Describe(ContentVerb::Handle)) == "content.");
	CHECK(std::string(Describe(ContentVerb::Publish)) == "cdn.publish.");

	// Turning a form off for one verb says nothing about the other: an origin
	// that will not publish an SVG and a client that will not decode one are
	// different deployments.
	REQUIRE(Flags::Set("cdn.publish.svg", "false", FlagSource::ConfigFile) == FlagStatus::Applied);

	CHECK(ContentPolicy::FromFlags(ContentVerb::Handle).Allows(ContentForm::Svg));
	CHECK_FALSE(ContentPolicy::FromFlags(ContentVerb::Publish).Allows(ContentForm::Svg));
}

TEST_CASE("every form has a flag under both verbs and it is declared once", "[content][policy]") {
	Fresh();

	for (const ContentForm form : AllForms()) {
		for (const ContentVerb verb : {ContentVerb::Handle, ContentVerb::Publish}) {
			const std::string name = std::string(Describe(verb)) + Describe(form);
			INFO(name);

			// Declared, with the right default, and settable.
			CHECK(engine::core::Flag(name).IsValid());
			CHECK(engine::core::Flag(name).Boolean());
			CHECK(Flags::Set(name, "false", FlagSource::ConfigFile) == FlagStatus::Applied);
		}
	}

	// Everything is off now, both ways, which also proves the bitset reaches
	// every ordinal the table can produce — a form past what `ContentPolicy`
	// holds would still read `false` from its flag and answer `true` here.
	const ContentPolicy handled = ContentPolicy::FromFlags(ContentVerb::Handle);
	for (const ContentForm form : AllForms()) {
		INFO(Describe(form));
		CHECK_FALSE(handled.Allows(form));
	}

	// And declaring one verb twice is refused rather than producing two
	// defaults.
	CHECK_FALSE(DeclareContentFlags(ContentVerb::Handle));
}

TEST_CASE("the flags read what the deployment wrote", "[content][policy]") {
	Fresh();

	REQUIRE(Flags::Set("content.gif", "off", FlagSource::ConfigFile) == FlagStatus::Applied);
	REQUIRE(Flags::Set("content.mp4", "no", FlagSource::Environment) == FlagStatus::Applied);

	const ContentPolicy policy = ContentPolicy::FromFlags(ContentVerb::Handle);
	CHECK_FALSE(policy.Allows(ContentForm::Gif));
	CHECK_FALSE(policy.Allows(ContentForm::Mp4));
	CHECK(policy.Allows(ContentForm::Svg));
	CHECK(policy.RefusedText() == "gif, mp4");
}
