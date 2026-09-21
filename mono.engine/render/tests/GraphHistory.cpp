// Graph history is a CPU cache key and must reject scene changes it does not sign.

#include "GraphHistory.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.graphhistory")

using engine::graph::NodeScope;
using engine::graph::ResourceDesc;
using engine::graph::ResourceLifetime;
using engine::render::GraphHistoryCurrentProducer;
using engine::render::GraphHistoryGeneration;
using engine::render::GraphHistoryOwner;
using engine::render::GraphHistoryReadable;
using engine::render::GraphHistoryReadNeedsValidation;
using engine::render::GraphHistoryReadSource;
using engine::render::GraphHistorySignature;
using engine::render::PresentationDamage;
using engine::render::SelectGraphHistoryRead;
using engine::scene::CameraMatrices;

namespace {
	CameraMatrices Matrices() {
		CameraMatrices matrices;
		matrices.ViewProjection[1][2] = 3.0f;
		matrices.Projection[2][1] = 7.0f;
		return matrices;
	}
}

TEST_CASE("stable graph history inputs reuse their signature", "[render][graph-history]") {
	const CameraMatrices matrices = Matrices();
	const uint64_t first = GraphHistorySignature(31, matrices, 640, 480);
	const uint64_t second = GraphHistorySignature(31, matrices, 640, 480);

	CHECK(first == second);
	CHECK(GraphHistoryReadable(PresentationDamage{}));
}

TEST_CASE("graph history signatures change with camera content and dimensions", "[render][graph-history]") {
	const CameraMatrices matrices = Matrices();
	const uint64_t baseline = GraphHistorySignature(31, matrices, 640, 480);

	auto changedMatrices = matrices;
	changedMatrices.ViewProjection[0][3] = 1.0f;
	CHECK(GraphHistorySignature(31, changedMatrices, 640, 480) != baseline);
	CHECK(GraphHistorySignature(32, matrices, 640, 480) != baseline);
	CHECK(GraphHistorySignature(31, matrices, 641, 480) != baseline);
	CHECK(GraphHistorySignature(31, matrices, 640, 481) != baseline);
}

TEST_CASE("radiance damage resets graph history reads", "[render][graph-history]") {
	for (PresentationDamage damage : {
			 PresentationDamage{.Scene = true},
			 PresentationDamage{.Objects = true},
			 PresentationDamage{.Environment = true},
			 PresentationDamage{.Viewport = true},
			 PresentationDamage{.Portals = true},
		 }) {
		CHECK_FALSE(GraphHistoryReadable(damage));
	}
	CHECK(GraphHistoryReadable(PresentationDamage{.GameInterface = true}));
}

TEST_CASE("graph-owned history reads validate the completed generation", "[render][graph-history]") {
	ResourceDesc history;
	history.Lifetime = ResourceLifetime::History;
	CHECK(GraphHistoryReadNeedsValidation(history, false));
	CHECK_FALSE(GraphHistoryReadNeedsValidation(history, true));

	// Path tracing owns its accumulation image, so it is not an external graph input.
	history.External = false;
	CHECK(GraphHistoryReadNeedsValidation(history, false));
}

TEST_CASE("a direct producer outranks temporal history during graph damage", "[render][graph-history]") {
	CHECK(
		SelectGraphHistoryRead(true, PresentationDamage{.Scene = true}) ==
		GraphHistoryReadSource::CurrentProducer
	);
	CHECK(SelectGraphHistoryRead(false, PresentationDamage{}) == GraphHistoryReadSource::PreviousGeneration);
}

TEST_CASE("current graph history observes command ownership", "[render][graph-history]") {
	CHECK(GraphHistoryCurrentProducer(true, true, false));
	CHECK_FALSE(GraphHistoryCurrentProducer(true, false, false));
	CHECK(GraphHistoryCurrentProducer(false, false, true));
}

TEST_CASE("discarded graph history writes cannot become readable", "[render][graph-history]") {
	// A failed or cancelled submission clears its scheduled writer. With no
	// completed generation valid for this damaged frame, the reader must refuse.
	CHECK(
		SelectGraphHistoryRead(false, PresentationDamage{.Environment = true}) ==
		GraphHistoryReadSource::Unavailable
	);
}

TEST_CASE("command-owned environment generations commit independently", "[render][graph-history]") {
	GraphHistoryGeneration mainGeneration;
	GraphHistoryGeneration separateGeneration;
	uint8_t mainCommand = 0;
	uint8_t separateCommand = 0;
	mainGeneration.Stage(17, &mainCommand);
	separateGeneration.Stage(23, &separateCommand);
	separateGeneration.Commit(&separateCommand);
	CHECK_FALSE(mainGeneration.Ready);
	CHECK(separateGeneration.Ready);
	CHECK_FALSE(mainGeneration.Matches(17, &separateCommand));
	CHECK(separateGeneration.Matches(23, &mainCommand));
	mainGeneration.Commit(&mainCommand);
	CHECK(mainGeneration.Ready);

	GraphHistoryGeneration failedMain;
	failedMain.Stage(29, &mainCommand);
	failedMain.Discard(&mainCommand);
	CHECK_FALSE(failedMain.Ready);
	CHECK_FALSE(failedMain.Matches(29, &mainCommand));
}

TEST_CASE("graph history owners isolate view world and frame scopes", "[render][graph-history]") {
	CHECK(GraphHistoryOwner(NodeScope::View, 3, 70) == 3);
	CHECK(GraphHistoryOwner(NodeScope::View, 4, 70) == 4);
	CHECK(GraphHistoryOwner(NodeScope::World, 3, 70) == 70);
	CHECK(GraphHistoryOwner(NodeScope::World, 3, 71) == 71);
	CHECK(GraphHistoryOwner(NodeScope::Frame, 3, 70) == 0);
}
