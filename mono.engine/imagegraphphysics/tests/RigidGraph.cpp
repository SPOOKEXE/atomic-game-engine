#include "RigidGraphFixture.hpp"

#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
TEST_SUITE_ID("engine.imagegraphphysics.rigid_graph")
using namespace engine::imagegraph;
TEST_CASE(
	"Rigid native graph records contacts, seeks and paused versus played frames", "[rigid][imagegraph]"
) {
	const auto document = engine::imagegraphphysics::testing::RigidGraphFixture();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::imagegraphphysics::RigidProvider provider;
	EvaluationRequest request;
	request.RigidProvider = &provider;
	request.RigidPlaying = true;
	request.RigidFrameProgress = true;
	request.RigidAuthoringRevision = 19;
	StatefulEvaluationResult result;
	auto evaluate = [&] {
		const auto status = EvaluateStateful(document, plan, "image", request, result, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ' ' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
	};
	evaluate();
	const auto first = std::get<Image>(result.Output).Pixels;
	CHECK(std::any_of(first.begin(), first.end(), [](uint8_t byte) { return byte != 0; }));
	for (uint64_t tick = 1; tick <= 12; ++tick) {
		request.Tick = tick;
		request.RigidReplay = &result.Rigid;
		evaluate();
	}
	const auto later = std::get<Image>(result.Output).Pixels;
	CHECK(first != later);
	SourceRigidSnapshot snapshot;
	REQUIRE(
		provider.Replay(
			result.Rigid.Owners[0].History,
			12,
			std::nullopt,
			Limits::MaximumEvaluationBytes,
			snapshot,
			diagnostic
		) == Status::Ok
	);
	CHECK_FALSE(snapshot.Contacts.empty());
	const auto retained = result.Rigid;
	request.Tick = 0;
	request.RigidReplay = nullptr;
	evaluate();
	CHECK(std::get<Image>(result.Output).Pixels == first);
	for (uint64_t tick = 1; tick <= 12; ++tick) {
		request.Tick = tick;
		request.RigidReplay = &result.Rigid;
		evaluate();
	}
	CHECK(result.Rigid == retained);
	CHECK(std::get<Image>(result.Output).Pixels == later);
	request.Tick = 0;
	request.RigidReplay = nullptr;
	request.RigidPlaying = false;
	evaluate();
	const auto paused = std::get<Image>(result.Output).Pixels;
	request.Tick = 0;
	request.RigidReplay = &result.Rigid;
	request.RigidPlaying = true;
	evaluate();
	CHECK(std::get<Image>(result.Output).Pixels == first);
	CHECK(paused != first);
}

TEST_CASE("Rigid native visual uses source GameMaker screen rotation", "[rigid][imagegraph]") {
	auto document = engine::imagegraphphysics::testing::RigidGraphFixture();
	document.Nodes[0].Values.push_back({"strength", 0.});
	document.Nodes[1] = {
		"texture", "image.captured", "rigid", {}, {{"source_id", std::string{"asymmetric"}}}
	};
	// Center texels on pixel centers so this tests rotation rather than quad-edge coverage.
	document.Nodes[2].Values.push_back({"spawn_position", Vector2{16.5, 16.5}});
	document.Nodes[2].Values.push_back({"spawn_rotation", 90.});
	document.Nodes[3].Values = {{"round_position", false}, {"simulate", false}};
	// Replace duplicate authored controls rather than accepting ambiguous inputs.
	document.Nodes[0].Values.erase(document.Nodes[0].Values.begin() + 3);
	document.Nodes[2].Values.erase(document.Nodes[2].Values.begin() + 2);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::imagegraphphysics::RigidProvider provider;
	const std::array sources{
		RequestImageSource{"asymmetric", Image{3, 1, {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255}}}
	};
	EvaluationRequest request;
	request.RigidProvider = &provider;
	request.ImageSources = sources;
	StatefulEvaluationResult result;
	const auto status = EvaluateStateful(document, plan, "image", request, result, diagnostic);
	INFO(diagnostic.NodeId << ':' << diagnostic.Port << ' ' << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto &image = std::get<Image>(result.Output);
	SurfacePixel top{}, middle{}, bottom{};
	REQUIRE(LoadSurfacePixel(image, 16, 15, top));
	REQUIRE(LoadSurfacePixel(image, 16, 16, middle));
	REQUIRE(LoadSurfacePixel(image, 16, 17, bottom));
	CHECK(top == SurfacePixel{1, 0, 0, 1});
	CHECK(middle == SurfacePixel{0, 1, 0, 1});
	CHECK(bottom == SurfacePixel{0, 0, 1, 1});
	for (const auto position :
		 std::array{Vector2{15, 16}, Vector2{17, 16}, Vector2{16, 14}, Vector2{16, 18}}) {
		SurfacePixel outside{};
		REQUIRE(LoadSurfacePixel(
			image, static_cast<uint32_t>(position.X), static_cast<uint32_t>(position.Y), outside
		));
		CHECK(outside == SurfacePixel{});
	}
}
