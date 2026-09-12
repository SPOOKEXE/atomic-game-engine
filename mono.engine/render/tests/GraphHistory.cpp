// Graph history is a CPU cache key and must reject scene changes it does not sign.

#include "GraphHistory.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.graphhistory")

using engine::graph::NodeScope;
using engine::render::GraphHistoryOwner;
using engine::render::GraphHistoryReadable;
using engine::render::GraphHistorySignature;
using engine::render::PresentationDamage;
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

TEST_CASE("graph history owners isolate view world and frame scopes", "[render][graph-history]") {
	CHECK(GraphHistoryOwner(NodeScope::View, 3, 70) == 3);
	CHECK(GraphHistoryOwner(NodeScope::View, 4, 70) == 4);
	CHECK(GraphHistoryOwner(NodeScope::World, 3, 70) == 70);
	CHECK(GraphHistoryOwner(NodeScope::World, 3, 71) == 71);
	CHECK(GraphHistoryOwner(NodeScope::Frame, 3, 70) == 0);
}
