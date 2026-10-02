#include <engine/imagegraph/SliceStackReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.slice_stack_replay")
using namespace engine::imagegraph;
namespace {
	Document Graph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"cube", "pc.3_d_mesh_cube", "", {}, {}},
			{"slices",
			 "pc.3_d_mesh_stack_slice",
			 "",
			 {},
			 {{"output_dimension", Vector2{2, 2}}, {"slices", int64_t(2)}}}
		};
		document.Links = {{"cube", "mesh", "slices", "mesh"}};
		document.Outputs = {{"slices", "slices", "outputs"}};
		document.SliceStackActions = {{"slices", {}, 1}};
		return document;
	}
}
TEST_CASE(
	"Slice Stack authored Splice action progresses bounded pixels and publishes completed cube slices",
	"[imagegraph][slice_stack_replay]"
) {
	auto document = Graph();
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	SliceStackReplayState state;
	EvaluationRequest request;
	ImageArray output;
	REQUIRE(EvaluateArray(document, plan, "slices", request, output, diagnostic) == Status::Ok);
	REQUIRE(output.Images.empty());
	REQUIRE(AdvanceSliceStack(document, plan, "slices", request, 0, state, diagnostic) == Status::Ok);
	REQUIRE(state.Entries.size() == 1);
	REQUIRE(state.Entries[0].Active);
	REQUIRE(state.Entries[0].Pixel == 0);
	REQUIRE(state.Entries[0].Images.size() == 2);
	REQUIRE(state.Entries[0].Faces.size() == 8);
	REQUIRE(AdvanceSliceStack(document, plan, "slices", request, 1, state, diagnostic) == Status::Ok);
	REQUIRE(state.Entries[0].Pixel == 1);
	REQUIRE(state.Entries[0].Images[0].Pixels[3] == 255);
	REQUIRE(state.Entries[0].Images[0].Pixels[7] == 0);
	const auto halfway = state;
	REQUIRE(
		AdvanceSliceStack(document, plan, "slices", request, 4097, state, diagnostic) == Status::LimitExceeded
	);
	REQUIRE(state == halfway);
	SliceStackReplayState restored;
	REQUIRE(ReadSliceStackReplay(WriteSliceStackReplay(state), restored, diagnostic) == Status::Ok);
	REQUIRE(restored == state);
	REQUIRE(AdvanceSliceStack(document, plan, "slices", request, 7, state, diagnostic) == Status::Ok);
	REQUIRE_FALSE(state.Entries[0].Active);
	REQUIRE(state.Entries[0].Slice == 2);
	for (const auto &image : state.Entries[0].Images)
		for (size_t pixel = 0; pixel < 4; ++pixel)
			REQUIRE(image.Pixels[pixel * 4 + 3] == 255);
	request.SliceStackReplay = &state;
	REQUIRE(EvaluateArray(document, plan, "slices", request, output, diagnostic) == Status::Ok);
	auto published = state.Entries[0].Images;
	for (auto &image : published)
		image.Hash = SurfaceHash(image);
	REQUIRE(output.Images == published);
	REQUIRE(output.Items.size() == 2);
	const auto complete = state;
	REQUIRE(AdvanceSliceStack(document, plan, "slices", request, 1, state, diagnostic) == Status::Ok);
	REQUIRE(state == complete);
	Document roundtrip;
	REQUIRE(Read(Write(document), roundtrip, diagnostic) == Status::Ok);
	REQUIRE(roundtrip == document);
}
TEST_CASE(
	"Slice Stack state and action codecs refuse malformed identities without replacing owners",
	"[imagegraph][slice_stack_replay]"
) {
	auto document = Graph();
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	SliceStackReplayState state;
	REQUIRE(AdvanceSliceStack(document, plan, "slices", {}, 1, state, diagnostic) == Status::Ok);
	const auto before = state;
	REQUIRE(ReadSliceStackReplay("slice_stack_replay 1 4097", state, diagnostic) == Status::LimitExceeded);
	REQUIRE(state == before);
	REQUIRE(
		ReadSliceStackReplay(WriteSliceStackReplay(state) + "trailing", state, diagnostic) ==
		Status::Malformed
	);
	REQUIRE(state == before);
	REQUIRE(
		AdvanceSliceStack(document, plan, "slices", {}, 1, state, diagnostic, 1) == Status::LimitExceeded
	);
	REQUIRE(state == before);
	document.SliceStackActions.push_back(document.SliceStackActions.front());
	REQUIRE(Compile(document, plan, diagnostic) == Status::DuplicateId);
	document.SliceStackActions.resize(1);
	document.FormatVersion = 8;
	REQUIRE(Compile(document, plan, diagnostic) == Status::UnsupportedVersion);
}
