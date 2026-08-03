// The client's own tick, without a window.
//
// This is a client test rather than an engine one because what it covers is
// client code: the demo scene's components, resources and systems, and the
// shape of the data they hand to the renderer. It links several engine modules
// because the client does, not because it is testing them — each of those has
// its own suite under mono.engine/<module>/tests/.
//
// Headless on purpose. Everything up to the point where a GPU is required is
// testable on a machine with no GPU, and drawing the line there is what keeps
// the suite runnable everywhere.

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/render/DebugPanels.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <client/Demo.hpp>

TEST_SUITE_ID("client.demo.tick")
TEST_DEPENDS("engine.ecs.scheduler")
TEST_DEPENDS("engine.ecs.store")
TEST_DEPENDS("engine.ecs.resources")
TEST_DEPENDS("engine.render.debugpanels")
TEST_DEPENDS("engine.core.framegraph")
TEST_DEPENDS("engine.scene.components")
TEST_DEPENDS("engine.scene.drawinstance")

using Catch::Approx;
using engine::core::FrameGraph;
using engine::core::Metrics;
using engine::ecs::Phase;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::scene::ActiveCamera;
using engine::scene::Camera;
using engine::scene::DrawInstance;
using engine::scene::PreviousTransform;
using engine::scene::Transform;
using engine::scene::Visual;
using engine::scene::WorldBounds;

namespace {
	constexpr uint32_t ENTITIES = 512;
	constexpr float STEP = 1.0f / 60.0f;

	// A world and the scheduler that ticks it. Nothing else — which is the
	// point: after BuildDemoWorld returns, everything the tick reads and writes
	// is inside the store.
	struct Session {
		Store World{"integration"};
		Scheduler Systems;

		Session() {
			client::BuildDemoWorld(World, Systems, ENTITIES);
		}

		void Tick(int ticks, float step = STEP) {
			for (int tick = 0; tick < ticks; tick++) {
				Systems.Tick(World, step);
			}
		}

		const std::vector<DrawInstance> &Drawn() const {
			return World.Resource<client::DrawList>()->Instances;
		}
	};
}

TEST_CASE("a built scene produces one instance per entity", "[demo]") {
	Session session;
	session.Tick(1);

	REQUIRE(session.Drawn().size() == ENTITIES);

	// The camera is a row too and carries a Transform, so the query that counts
	// drawable things names Visual as well — a count of Transform alone would be
	// one too many and would read as an off-by-one in the build loop.
	REQUIRE(session.World.CountMatching<Transform, Visual>() == ENTITIES);
}

TEST_CASE("the world holds the scene's state, not a scene object", "[demo]") {
	Session session;

	// Every one of these is something that used to be a member of a C++ class
	// the systems captured by pointer. A second world could not have its own,
	// the affinity check did not cover them, and none of them would survive
	// being serialised with the world.
	REQUIRE(session.World.HasResource<WorldBounds>());
	REQUIRE(session.World.HasResource<ActiveCamera>());
	REQUIRE(session.World.HasResource<client::DrawList>());

	// And the clock, which every store has from birth.
	REQUIRE(session.World.Time().Tick == 0);
}

TEST_CASE("two worlds tick independently", "[demo]") {
	// The property the private-member version could not have had. Two scenes
	// sharing one clock or one draw list is a bug that only appears the day
	// something hosts two worlds, which is exactly when it is hardest to find.
	Session first;
	Session second;

	first.Tick(10);
	second.Tick(3);

	REQUIRE(first.World.Time().Tick == 10);
	REQUIRE(second.World.Time().Tick == 3);
	REQUIRE(first.World.Time().Elapsed > second.World.Time().Elapsed);

	REQUIRE(first.Drawn().size() == ENTITIES);
	REQUIRE(second.Drawn().size() == ENTITIES);
	REQUIRE(first.Drawn()[0].Frame.Position.X != Approx(second.Drawn()[0].Frame.Position.X));
}

TEST_CASE("the instance buffer is rebuilt, not appended to", "[demo]") {
	Session session;

	// The collect system clears and refills. Getting that wrong grows the
	// buffer without bound and is invisible for the first few seconds.
	session.Tick(60);
	REQUIRE(session.Drawn().size() == ENTITIES);
}

TEST_CASE("entities actually move", "[demo]") {
	Session session;
	session.Tick(1);
	const engine::core::Vector3 before = session.Drawn()[0].Frame.Position;

	session.Tick(30);
	const engine::core::Vector3 after = session.Drawn()[0].Frame.Position;

	const bool moved = before.X != after.X || before.Z != after.Z;
	REQUIRE(moved);
}

TEST_CASE("the camera is a row the systems move, not a resource holding a value", "[demo]") {
	Session session;

	// A lookup, not a search: the resource names which of the world's cameras
	// is live and the placement is a Transform on that entity.
	const engine::ecs::Entity camera = session.World.Resource<ActiveCamera>()->Entity;
	REQUIRE(session.World.Has<Camera>(camera));

	const auto atRest = session.World.Get<Transform>(camera)->Frame;
	session.Tick(120);
	const auto later = session.World.Get<Transform>(camera)->Frame;

	const bool moved =
		atRest.Position.X != Approx(later.Position.X) || atRest.Position.Z != Approx(later.Position.Z);
	REQUIRE(moved);

	// Far enough out to hold the scene. The camera reads WorldBounds rather
	// than a number it was built with, so a world with a different extent gets
	// a camera that fits it.
	const float extent = session.World.Resource<WorldBounds>()->HalfExtent;
	REQUIRE(session.World.Get<Camera>(camera)->FarPlane > extent);
}

namespace {
	// Simulation state, not render output. The two differ once interpolation
	// exists, and most of what these tests are about is the former.
	//
	// Cubes only. The camera has a Transform as well, and a query that swept it
	// up would compare the nth *row* rather than the nth cube.
	engine::core::Vector3 PositionOf(Store &store, int nth) {
		engine::core::Vector3 found;
		int seen = 0;
		store.Each<const Transform, const Visual>(
			[&](engine::ecs::Entity, const Transform &transform, const Visual &) {
				if (seen++ == nth) {
					found = transform.Frame.Position;
				}
			}
		);
		return found;
	}
}

TEST_CASE("the scene is a function of elapsed time, not of tick count", "[demo]") {
	// Two sessions stepped over the same total duration in different-sized
	// steps must agree. Without that, a frame-time comparison between two runs
	// compares two different scenes.
	Session coarse;
	Session fine;

	coarse.Tick(60, 1.0f / 60.0f);
	fine.Tick(120, 1.0f / 120.0f);

	REQUIRE(coarse.World.Time().Elapsed == Approx(fine.World.Time().Elapsed).margin(1e-6));

	// Compared on the Transform rather than on the emitted instance, because
	// the instance is *interpolated* and at alpha 0 it shows the previous
	// tick — 59/60 of a second against 119/120, which are legitimately
	// different places. What has to match is the simulation.
	const auto a = PositionOf(coarse.World, 7);
	const auto b = PositionOf(fine.World, 7);

	// Orbit position is derived from the world's clock, so it matches. Spin
	// integrates per tick and deliberately does not, which is why only the
	// translation is compared here.
	REQUIRE(a.X == Approx(b.X).margin(1e-3));
	REQUIRE(a.Y == Approx(b.Y).margin(1e-3));
	REQUIRE(a.Z == Approx(b.Z).margin(1e-3));
}

TEST_CASE("rendering interpolates between the last two ticks", "[demo]") {
	Session session;
	session.Tick(4);

	const auto previousTick = session.Drawn()[3].Frame.Position;

	// Alpha only affects the PreRender phase, so setting it and re-running that
	// phase alone moves the drawn position without advancing the simulation at
	// all. That the clock has a place to put alpha is what makes this two
	// calls rather than a setter on a scene object.
	session.World.SetFrame(0.0f, 1.0f);
	session.Systems.RunPhases(session.World, Phase::PreRender, Phase::PreRender);
	const auto currentTick = session.Drawn()[3].Frame.Position;

	// At alpha 1 the render is at the tick it just simulated; at 0 it is a
	// whole tick behind. That lag is inherent to interpolating — you can only
	// draw between two states you already have — and it is what buys smooth
	// motion at any frame rate.
	REQUIRE(currentTick.X != Approx(previousTick.X));

	// And it lands exactly on the simulation, not somewhere past it.
	const auto simulated = PositionOf(session.World, 3);
	REQUIRE(currentTick.X == Approx(simulated.X).margin(1e-4));
	REQUIRE(currentTick.Z == Approx(simulated.Z).margin(1e-4));
}

TEST_CASE("alpha zero draws the previous tick exactly", "[demo]") {
	Session session;
	session.Tick(4);

	// Default alpha is 0, which is where a frame that lands exactly on a tick
	// boundary sits.
	const auto drawn = session.Drawn()[3].Frame.Position;

	engine::core::Vector3 previous;
	int seen = 0;
	session.World.Each<const PreviousTransform>([&](engine::ecs::Entity, const PreviousTransform &transform) {
		if (seen++ == 3) {
			previous = transform.Frame.Position;
		}
	});

	REQUIRE(drawn.X == Approx(previous.X).margin(1e-4));
	REQUIRE(drawn.Z == Approx(previous.Z).margin(1e-4));
}

TEST_CASE("a simulation system cannot reach the frame delta", "[demo]") {
	Session session;

	// A frame far shorter than a tick, which is the ordinary case at a high
	// frame rate. If anything in the simulation read it, the scene would land
	// somewhere different from the same world stepped without it.
	Session control;

	for (int tick = 0; tick < 30; tick++) {
		session.World.SetFrame(1.0f / 500.0f, 0.0f);
		session.Systems.Tick(session.World, STEP);
	}
	control.Tick(30);

	const auto a = PositionOf(session.World, 11);
	const auto b = PositionOf(control.World, 11);
	REQUIRE(a.X == Approx(b.X));
	REQUIRE(a.Z == Approx(b.Z));
}

TEST_CASE("two sessions built the same way stay identical", "[demo]") {
	Session first;
	Session second;

	first.Tick(45);
	second.Tick(45);

	REQUIRE(first.Drawn().size() == second.Drawn().size());
	for (size_t index = 0; index < first.Drawn().size(); index++) {
		const auto &a = first.Drawn()[index];
		const auto &b = second.Drawn()[index];
		REQUIRE(a.Tint.R == Approx(b.Tint.R));
		REQUIRE(a.Frame.Position.X == Approx(b.Frame.Position.X));
		REQUIRE(a.Frame.Position.Z == Approx(b.Frame.Position.Z));
	}
}

TEST_CASE("a tick with a job pool matches one without", "[demo]") {
	Session pooled;
	Session serial;

	engine::parallel::Jobs::Start(4);
	pooled.Tick(30);
	engine::parallel::Jobs::Stop();

	serial.Tick(30);

	for (size_t index = 0; index < serial.Drawn().size(); index += 37) {
		REQUIRE(pooled.Drawn()[index].Frame.Position.X == Approx(serial.Drawn()[index].Frame.Position.X));
	}
}

TEST_CASE("a tick reports itself to the frame graph and the metrics sink", "[demo]") {
	Metrics::Clear();

	Session session;

	FrameGraph::SetEnabled(true);
	FrameGraph::BeginFrame();
	session.Tick(1);
	FrameGraph::EndFrame();

	const auto spans = FrameGraph::Spans();
	FrameGraph::SetEnabled(false);

	// Every registered system is a span, with no instrumentation inside the
	// systems themselves. That is what makes the F5 overlay useful on code
	// nobody remembered to annotate.
	const auto named = [&spans](std::string_view name) {
		return std::any_of(spans.begin(), spans.end(), [name](const auto &span) {
			return span.Name == name;
		});
	};

	REQUIRE(named("ecs.systems"));
	REQUIRE(named("orbit"));
	REQUIRE(named("spin"));
	REQUIRE(named("move-camera"));
	REQUIRE(named("collect-instances"));

	// A span per phase between the scheduler and its systems, so "which part of
	// the tick" is answerable before "which system" — and every one of them is
	// ECS time, because every engine and game system runs through the ECS.
	REQUIRE(named("simulation"));
	REQUIRE(named("pre-render"));

	const auto categorised = [&spans](std::string_view name) {
		const auto found =
			std::find_if(spans.begin(), spans.end(), [name](const auto &span) { return span.Name == name; });
		return found != spans.end() && found->Category == engine::core::ProfileCategory::ECS;
	};
	REQUIRE(categorised("orbit"));
	REQUIRE(categorised("collect-instances"));

	const auto counters = Metrics::Drain();
	const auto instances = std::find_if(counters.begin(), counters.end(), [](const auto &counter) {
		return counter.Name == engine::core::Name("render.instances");
	});
	REQUIRE(instances != counters.end());
	REQUIRE(instances->Value == Approx(static_cast<double>(ENTITIES)));
}

TEST_CASE("the panels render a real tick's data", "[demo]") {
	Session session;

	FrameGraph::SetEnabled(true);
	FrameGraph::BeginFrame();
	session.Tick(1);
	FrameGraph::EndFrame();

	engine::render::FrameStatistics statistics;
	statistics.Record(0.0, STEP);

	std::vector<engine::render::SystemTiming> timings;
	for (const auto &timing : session.Systems.Timings()) {
		timings.push_back({timing.Name, timing.Milliseconds});
	}
	// capture-previous, orbit, spin, move-camera, collect-instances.
	REQUIRE(timings.size() == 5);

	engine::render::OverlayImage image;
	image.Resize(1280, 720);

	engine::render::DebugPanelData panels;
	panels.ShowStatistics = true;
	panels.ShowFrameGraph = true;
	panels.Statistics = &statistics;
	panels.Spans = FrameGraph::Spans();
	panels.FrameMilliseconds = FrameGraph::FrameMilliseconds();
	panels.Systems = timings;
	panels.Entities = ENTITIES;

	engine::render::DrawDebugPanels(image, panels);
	FrameGraph::SetEnabled(false);

	REQUIRE(image.IsDirty());
}
