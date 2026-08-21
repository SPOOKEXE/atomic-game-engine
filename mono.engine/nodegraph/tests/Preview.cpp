// Pictures, and who is allowed to make one.
//
// **A picture belongs to the wire, not to the node.** An input's payload was
// made upstream by a type the reader has never heard of, and the only thing
// both ends agree on is what the wire carries - so a panel drawing a node's
// *inputs* asks the data type. These cases pin that direction, because the
// obvious implementation puts `Preview` on the node and works right up until
// somebody tries to draw an input.

#include "Fixture.hpp"

#include <engine/nodegraph/Evaluate.hpp>
#include <engine/nodegraph/Graph.hpp>
#include <engine/nodegraph/Preview.hpp>
#include <engine/nodegraph/Types.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <any>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.nodegraph.preview")

using namespace engine::nodegraph;
using fixture::RegisterFixtureNodes;

TEST_CASE("a payload draws through the type its wire carries", "[nodegraph]") {
	RegisterFixtureNodes();
	Graph graph;

	const NodeId source = graph.Add("field.source", 0.0f, 0.0f);
	graph.Find(source)->Widgets["resolution"].Text = "16";

	Evaluator runner;
	runner.RunToCompletion(graph);
	const std::any *made = runner.Output(source, "Out");
	REQUIRE(made != nullptr);

	// **`nullptr` for the node type, which is the assertion.** With no node
	// override in play, a picture coming back at all proves it came from
	// `data.FIELD`'s own `Preview` - the wire, not the node.
	PreviewImage image;
	REQUIRE(PictureOf(nullptr, "data.FIELD", *made, image));
	CHECK(image.Valid());
	CHECK(image.Side == 16);
	CHECK(image.Rgba.size() == static_cast<size_t>(16 * 16 * 4));

	// **A payload with no picture makes none, rather than a grey square.**
	// `data.NUMBER` registers no `Preview`, and the honest answer to "draw this
	// number" is that there is nothing to draw.
	PreviewImage nothing;
	CHECK(!PictureOf(nullptr, "data.NUMBER", std::any{2.5}, nothing));
	CHECK(!nothing.Valid());

	// An unregistered wire type is not an error and not a picture.
	CHECK(!PictureOf(nullptr, "data.TYPO", *made, nothing));
}

TEST_CASE("a payload the type cannot read makes no picture", "[nodegraph]") {
	RegisterFixtureNodes();

	// The type is registered and the payload is the wrong thing. `std::any_cast`
	// returning null is the ordinary case here rather than a bug, because what
	// travels on a wire is the host's business and a document can be wrong.
	PreviewImage image;
	CHECK(!PictureOf(nullptr, "data.FIELD", std::any{std::string("not a field")}, image));
}

TEST_CASE("a height field reads back as elevation, unshaded", "[nodegraph]") {
	RegisterFixtureNodes();
	Graph graph;

	const NodeId source = graph.Add("field.source", 0.0f, 0.0f);
	graph.Find(source)->Widgets["resolution"].Text = "16";

	Evaluator runner;
	runner.RunToCompletion(graph);
	const std::any *made = runner.Output(source, "Out");
	REQUIRE(made != nullptr);

	// **The numbers, not the thumbnail.** A thumbnail of a height field is
	// already shaded, so re-reading the shading as elevation would put the
	// lighting into the geometry.
	Surface surface;
	REQUIRE(SurfaceOf("data.FIELD", *made, surface));
	CHECK(surface.Valid());
	CHECK(surface.Side == 16);
	CHECK(surface.Heights.size() == static_cast<size_t>(16 * 16));

	// Clamped rather than wrapped: a surface is a patch and not a tile, and
	// wrapping would join the far edge to the near one.
	CHECK(surface.At(99, 99) == surface.At(15, 15));

	// A wire that is not carrying a landscape offers no surface, so an inspector
	// shows no 3-D button rather than a flat plane.
	Surface flat;
	CHECK(!SurfaceOf("data.NUMBER", std::any{1.0}, flat));
}

TEST_CASE("an exported picture is a PNG a decoder would accept", "[nodegraph]") {
	RegisterFixtureNodes();

	PreviewImage image;
	image.Side = 4;
	image.Rgba.assign(4 * 4 * 4, 200);
	REQUIRE(image.Valid());

	const std::vector<uint8_t> png = EncodePng(image);
	REQUIRE(png.size() > 8);

	// The eight-byte signature every decoder checks first.
	const std::vector<uint8_t> signature = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	for (size_t index = 0; index < signature.size(); index++) {
		CHECK(png[index] == signature[index]);
	}

	// IHDR immediately after it, and IEND at the end - a file that stopped early
	// still opens in some viewers and is truncated in others.
	CHECK(std::string(png.begin() + 12, png.begin() + 16) == "IHDR");
	CHECK(std::string(png.end() - 8, png.end() - 4) == "IEND");

	// An invalid image encodes to nothing rather than to a header with no body.
	PreviewImage broken;
	broken.Side = 4;
	CHECK(EncodePng(broken).empty());
}
