#include <engine/imagegraphfont/GraphFontInputs.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

TEST_SUITE_ID("engine.imagegraphfont.graph_font_inputs")
using namespace engine::imagegraph;
using namespace engine::imagegraphfont;

static_assert(!std::is_move_constructible_v<GraphFontInputs>);
static_assert(!std::is_copy_constructible_v<GraphFontInputs>);

TEST_CASE(
	"font owner binds immutable held playback and actual retained configuration", "[graphfont_inputs]"
) {
	GraphFontConfiguration configuration;
	configuration.Context.AliasMapKnown = true;
	configuration.Context.DefaultFontPath = std::string{};
	configuration.Context.Playing = false;
	configuration.Context.TextCaseProfile = FontTextCaseProfile::UnicodeDefault;
	configuration.Context.BitmapTextureProfile = FontBitmapTextureProfile::NativeFrameUv;
	configuration.Context.Aliases = {{"Label", "/explicit/label.bdf"}};
	configuration.ReadGrants = {{"text", "/explicit/label.bdf", false, "font"}};
	GraphFontInputs owner;
	Diagnostic diagnostic;
	REQUIRE(owner.Replace(configuration, {}, Limits::MaximumEvaluationBytes, diagnostic));
	CHECK(owner.Revision() == 1);
	const auto ownedBytes = owner.RetainedBytes();
	REQUIRE(ownedBytes >= *GraphFontConfigurationRetainedBytes(owner.Configuration()));
	configuration.Context.Aliases[0].second = "/mutated";
	SourceFontContext held;
	EvaluationRequest request;
	REQUIRE(owner.Bind(true, held, request, Limits::MaximumEvaluationBytes, diagnostic));
	CHECK(request.SourceFonts == &held);
	CHECK(held.Playing == true);
	CHECK(owner.Configuration().Context.Playing == false);
	CHECK(held.Aliases[0].second == "/explicit/label.bdf");
	CHECK(request.SourceFontHostResidentBytes == ownedBytes);
	REQUIRE(request.FontProvider);
	const auto provider = request.FontProvider;
	const auto before = held;
	CHECK_FALSE(owner.Bind(false, held, request, ownedBytes - 1, diagnostic));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(held == before);
	CHECK(request.FontProvider == provider);
	CHECK(request.SourceFontHostResidentBytes == ownedBytes);
}

TEST_CASE(
	"font configuration replacement preserves provider revision and grants on refusal", "[graphfont_inputs]"
) {
	GraphFontInputs owner;
	GraphFontConfiguration configuration;
	configuration.Context.AliasMapKnown = true;
	configuration.Context.DefaultFontPath = std::string{};
	configuration.ReadGrants = {{"text", "/explicit/a.bdf", false, "font"}};
	Diagnostic diagnostic;
	REQUIRE(owner.Replace(configuration, {}, Limits::MaximumEvaluationBytes, diagnostic));
	const auto revision = owner.Revision(), bytes = owner.RetainedBytes();
	SourceFontContext held;
	EvaluationRequest request;
	REQUIRE(owner.Bind(false, held, request, Limits::MaximumEvaluationBytes, diagnostic));
	auto *provider = request.FontProvider;
	auto malformed = configuration;
	malformed.ReadGrants[0].Write = true;
	CHECK_FALSE(owner.Replace(malformed, {}, Limits::MaximumEvaluationBytes, diagnostic));
	CHECK(owner.Revision() == revision);
	CHECK(owner.RetainedBytes() == bytes);
	CHECK(owner.Configuration().ReadGrants[0].Write == false);
	CHECK_FALSE(owner.Replace(configuration, {}, bytes - 1, diagnostic));
	CHECK(owner.Revision() == revision);
	REQUIRE(owner.Bind(false, held, request, Limits::MaximumEvaluationBytes, diagnostic));
	CHECK(request.FontProvider == provider);
	malformed = configuration;
	malformed.ReadGrants.push_back(malformed.ReadGrants[0]);
	CHECK_FALSE(owner.Replace(malformed, {}, Limits::MaximumEvaluationBytes, diagnostic));
	CHECK(owner.Revision() == revision);
	configuration.Context.DefaultFontPath = "/explicit/b.bdf";
	REQUIRE(owner.Replace(configuration, {}, Limits::MaximumEvaluationBytes, diagnostic));
	CHECK(owner.Revision() == revision + 1);
	REQUIRE(owner.Bind(true, held, request, Limits::MaximumEvaluationBytes, diagnostic));
	CHECK(held.DefaultFontPath == "/explicit/b.bdf");
}
