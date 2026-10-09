#include <engine/imagegraph/Reference.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.reference")
using namespace engine::imagegraph;

TEST_CASE("live image references retain portable asset and instance identities", "[imagegraph][reference]") {
	Reference reference;
	REQUIRE(ParseReference("imagegraph://ui/panel.aimagegraph#image", reference));
	CHECK(reference.Kind == ReferenceKind::Asset);
	CHECK(reference.Name == "ui/panel.aimagegraph");
	CHECK(reference.Output == "image");
	REQUIRE(ParseReference("imagegraph-instance://Panel_1#normal", reference));
	CHECK(reference.Kind == ReferenceKind::Instance);
	CHECK(reference.Name == "Panel_1");
	CHECK(reference.Output == "normal");
	for (const auto invalid :
		 {"imagegraph://../panel.aimagegraph#image",
		  "imagegraph:///panel.aimagegraph#image",
		  "imagegraph://panel.imagegraph#image",
		  "imagegraph://panel.aimagegraph",
		  "imagegraph://panel.aimagegraph#",
		  "imagegraph://panel.aimagegraph#image#other",
		  "imagegraph://a//b.aimagegraph#image",
		  "imagegraph://a%2fb.aimagegraph#image",
		  "imagegraph-instance://..#image",
		  "imagegraph-instance://world/panel#image"}) {
		CHECK_FALSE(ParseReference(invalid, reference));
		CHECK(reference.Name == "Panel_1");
	}
}

TEST_CASE(
	"editable image source references use canonical bounded local identities", "[imagegraph][reference]"
) {
	CHECK(IsEditableImageReference("editable-image://1"));
	CHECK(IsEditableImageReference("editable-image://18446744073709551615"));
	for (const auto invalid :
		 {"editable-image://",
		  "editable-image://01",
		  "editable-image://-1",
		  "editable-image://+1",
		  "editable-image://0",
		  "editable-image://18446744073709551616",
		  "editable-image://1#image",
		  "editable-image://1/path",
		  "editable-image://1 ",
		  "data:image/png;base64,AA=="})
		CHECK_FALSE(IsEditableImageReference(invalid));
}
