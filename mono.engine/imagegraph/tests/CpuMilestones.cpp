#include "fixtures/CpuMilestones.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.cpu_milestones")
using Fixture = engine::imagegraph::testing::CpuMilestoneFixture;
using namespace engine::imagegraph;

TEST_CASE("CPU static and animated graphs retain exact colour controls", "[imagegraph][cpu_milestones]") {
	Fixture still(Fixture::Kind::Static2D);
	still.Run();
	const auto &image = still.ImageOutput();
	REQUIRE(image.Width == Fixture::Side);
	REQUIRE(image.Height == Fixture::Side);
	REQUIRE(image.Pixels.size() == Fixture::Side * Fixture::Side * 4);
	bool exactPixels = true;
	const std::array<uint8_t, 4> expected{191, 127, 63, 255};
	for (size_t offset = 0; offset < image.Pixels.size(); ++offset)
		exactPixels &= image.Pixels[offset] == expected[offset % 4];
	CHECK(exactPixels);

	Fixture animation(Fixture::Kind::Animation);
	animation.Run();
	for (size_t tick = 0; tick < Fixture::Frames; ++tick)
		CHECK(animation.FirstPixels[tick] == std::array<uint8_t, 4>{uint8_t(255 - tick * 17), 191, 127, 255});
	const auto before = animation.Fingerprint();
	animation.Run();
	CHECK(animation.Fingerprint() == before);
}
TEST_CASE(
	"CPU feedback streams actual prior generations and resets reproducibly", "[imagegraph][cpu_milestones]"
) {
	Fixture feedback(Fixture::Kind::Feedback);
	feedback.Run();
	REQUIRE(feedback.ImageOutput().Width == Fixture::Side);
	for (size_t tick = 0; tick < Fixture::Frames; ++tick) {
		const uint8_t expected = tick % 2 ? 0 : 255;
		CHECK(feedback.FirstPixels[tick] == std::array<uint8_t, 4>{expected, expected, expected, expected});
	}
	const auto first = feedback.Fingerprint();
	feedback.Run();
	CHECK(feedback.Fingerprint() == first);
	EvaluationRequest request;
	request.Tick = Fixture::Frames;
	request.Subframe = .5;
	CHECK_FALSE(feedback.Replay.Prepare(
		feedback.Authored,
		feedback.Compiled,
		1,
		1,
		request,
		feedback.Failure,
		Limits::MaximumEvaluationBytes,
		"out"
	));
	CHECK(feedback.Failure.Code == Status::InvalidValue);
	CHECK(feedback.Fingerprint() == first);
}
TEST_CASE(
	"CPU fixed simulation preserves grid topology and finite translated poses", "[imagegraph][cpu_milestones]"
) {
	Fixture simulation(Fixture::Kind::Simulation);
	simulation.Run();
	const auto &mesh = simulation.SimulationOutput();
	REQUIRE(mesh.Data);
	const auto &data = mesh.Data->Simulation;
	REQUIRE(data.Points.size() == 81);
	REQUIRE(data.Edges.size() == 144);
	REQUIRE(simulation.Request.SimulationReplay);
	REQUIRE(simulation.Request.SimulationReplay->Entries.size() == 1);
	CHECK(simulation.Request.SimulationReplay->Entries.front().State.Tick == Fixture::Frames - 1);
	CHECK(data.Points.front().Position.Y > data.Points.front().Original.Y);
	bool finitePoses = true, fixedX = true, preservedEdges = true;
	for (const auto &point : data.Points) {
		finitePoses &= std::isfinite(point.Position.X) && std::isfinite(point.Position.Y);
		fixedX &= std::abs(point.Position.X - point.Original.X) < 1e-8;
	}
	for (const auto &edge : data.Edges) {
		const auto a = data.Points[edge.First].Position, b = data.Points[edge.Second].Position;
		preservedEdges &= std::abs(std::hypot(a.X - b.X, a.Y - b.Y) - edge.Distance) < 1e-8;
	}
	CHECK(finitePoses);
	CHECK(fixedX);
	CHECK(preservedEdges);

	const auto first = simulation.Fingerprint();
	simulation.Run();
	CHECK(simulation.Fingerprint() == first);
}
TEST_CASE(
	"CPU 3D preparation retains complete typed geometry and authored transform",
	"[imagegraph][cpu_milestones]"
) {
	Fixture geometry(Fixture::Kind::Mesh3D);
	geometry.Run();
	const auto &mesh = geometry.MeshOutput();
	REQUIRE(mesh.Data);
	REQUIRE(mesh.Data->Parts.size() == 1);
	REQUIRE(mesh.Data->Parts.front().Vertices.size() == 960);
	REQUIRE(mesh.Data->LocalTransforms.size() == 2);
	CHECK(mesh.Data->LocalTransforms.front().Position == Vector3{2, 3, 4});
	bool finiteVertices = true, unitNormals = true;
	for (const auto &vertex : mesh.Data->Parts.front().Vertices) {
		finiteVertices &= std::isfinite(vertex.Position.X) && std::isfinite(vertex.Position.Y) &&
						  std::isfinite(vertex.Position.Z);
		const double length = std::sqrt(
			vertex.Normal.X * vertex.Normal.X + vertex.Normal.Y * vertex.Normal.Y +
			vertex.Normal.Z * vertex.Normal.Z
		);
		unitNormals &= std::abs(length - 1) < 1e-8;
	}
	CHECK(finiteVertices);
	CHECK(unitNormals);

	const auto first = geometry.Fingerprint();
	geometry.Run();
	CHECK(geometry.Fingerprint() == first);
	StatefulOutputEvaluationResult previous = geometry.Evaluated;
	CHECK(
		EvaluateStatefulOutputs(
			geometry.Authored,
			geometry.Compiled,
			geometry.Outputs,
			{},
			geometry.Evaluated,
			geometry.Failure,
			1
		) == Status::LimitExceeded
	);
	CHECK(
		std::get<EvaluatedValue>(geometry.Evaluated.Outputs.front().Output) ==
		std::get<EvaluatedValue>(previous.Outputs.front().Output)
	);
}
