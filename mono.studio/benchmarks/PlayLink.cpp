// What a frame of Play costs the editor between the tick and the picture.
//
// **This is the `play link` bar.** A capture of the studio running a scene shows
// `simulation` split into the world's tick and a `play link` span beside it, and
// inside that span `Authority::Publish` and then `Authority::Survey`,
// `FindBearing` and `Resign`. That is the editor hosting a server for one client
// and replicating the whole scene to it every tick, in the same thread that then
// has to draw. It is the one part of the editor whose cost is set by how big the
// scene is rather than by what the author is doing, so a scene that grows makes
// the editor slower with nobody touching the editor.
//
// `mono.studio/tests/PlayLink.cpp` explains why this half of Play is reachable
// without a window at all: `PlayLink` is an authority over one world and a
// replica in another, and neither needs a device. So this measures the real
// thing rather than a stand-in - unlike `Widgets.cpp` beside it, which has to
// rebuild the panels' shape because the panels themselves need an `Editor`.
//
// ## Why the rows vary how much moved
//
// **A tick of replication has a fixed half and a variable half, and only one of
// them is anybody's fault.** The variable half is the delta: what changed, sent
// to whoever can see it, which is work in proportion to the movement and is
// what a network programmer expects to pay. The fixed half is `Survey` - resolve
// the replicated component names, build the list of entities carrying any of
// them, re-hash every signed slot - and it runs in full on a scene where nothing
// has moved at all.
//
// So `nothing moved` is the floor: whatever that row says is what an idle Play
// session costs per part per tick, for a picture identical to the last one. The
// gap up to `everything moved` is the delta, and the ratio between the two is
// the honest answer to "is this replication or is this bookkeeping". A change
// that makes `Survey` walk per entity instead of per archetype table moves the
// floor and leaves the ceiling alone, which no other suite would notice.
//
// The fixture builds no physics pipeline, so `Universe::Tick` here is close to
// empty and the figure is dominated by the link. That is deliberate: what
// physics costs is `bench_physics`, and adding it here would only make this
// number harder to read.

#include <engine/core/Log.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Bench.hpp>
#include <engine/world/Universe.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <studio/PlayLink.hpp>
#include <utility>
#include <vector>

TEST_SUITE_ID("studio.bench.playlink")

using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::scene::Bounds;
using engine::scene::Transform;
using engine::scene::Visual;
using engine::world::Universe;
using engine::world::WorldId;
using engine::world::WorldSettings;
using studio::PlayLink;

namespace playlink_bench {

	constexpr double TICK_RATE = 60.0;
	constexpr float FRAME_SECONDS = 1.0f / static_cast<float>(TICK_RATE);

	// How many ticks the fixture runs before anything is measured.
	//
	// **A join is deliberately spread over ticks rather than sent at once**, so
	// the first frames of a link are a snapshot streaming out in chunks and are
	// nothing like the steady state this suite is about. The test suite settles
	// in 32; twice that leaves room for the acknowledgement round trip to empty
	// the outstanding sets as well.
	constexpr int SETTLE_TICKS = 64;

	struct Pool {
		Pool() {
			engine::parallel::Jobs::Start(0);
			engine::scene::RegisterSceneComponents();
		}
		~Pool() {
			engine::parallel::Jobs::Stop();
		}
	};
	const Pool Workers;

	// A universe with one authored world, a play link into it, and a settled
	// client.
	struct Fixture {
		Universe Worlds;
		WorldId Authority;
		PlayLink Link;
		std::vector<Entity> Parts;

		// How far along each part has been nudged, so that a moved row writes a
		// value that actually differs from the last one. A write of the same
		// bytes is still a dirty bit, but a signed component would not call it a
		// change - and half the point of these rows is which of the two paths a
		// scene is on.
		float Phase = 0.0f;
	};

	// A fixture of `parts` drawable entities, built once per size.
	//
	// Lazily and inside the body, because a universe binds its driver thread on
	// construction and a store binds its owning thread - neither of which is the
	// thread that runs a namespace static.
	Fixture &FixtureOf(size_t parts) {
		static std::vector<std::pair<size_t, std::unique_ptr<Fixture>>> built;
		for (auto &[size, fixture] : built) {
			if (size == parts) {
				return *fixture;
			}
		}

		auto fixture = std::make_unique<Fixture>();

		WorldSettings settings;
		settings.Name = Name("engine.bench.studio.Play" + std::to_string(parts));
		settings.TickRate = TICK_RATE;
		fixture->Authority = fixture->Worlds.Create(settings);

		// **The dirty bits a delta is built from.** In a real editor world
		// `physics::PreparePhysicsWorld` states this and `Editor::BuildWorld`
		// calls it for every world; this fixture builds no physics, so it stands
		// in for the pipeline it does not install. Without it the `Observed`
		// half of the replication table finds nothing, silently, and every row
		// below would report an idle link.
		fixture->Worlds.Enter(fixture->Authority, [](Store &store) {
			store.Observe<Transform>();
			store.Observe<engine::scene::Motion>();
		});

		fixture->Parts.reserve(parts);
		fixture->Worlds.Enter(fixture->Authority, [&fixture, parts](Store &store) {
			for (size_t index = 0; index < parts; index++) {
				const Entity entity = store.Create();

				// Spread along a line rather than stacked at the origin, so the
				// priority ordering has distinct distances to sort by - a scene
				// where every score ties measures a sort that never swaps.
				const float x = static_cast<float>(index % 256) * 4.0f;
				const float z = static_cast<float>(index / 256) * 4.0f;
				store.Set<Transform>(entity, Transform{CFrame(Vector3{x, 0.0f, z})});
				store.Set<Bounds>(entity, Bounds{Vector3{0.5f, 0.5f, 0.5f}});
				store.Set<Visual>(entity, Visual{});

				fixture->Parts.push_back(entity);
			}
		});

		std::string error;
		if (!fixture->Link.Start(fixture->Worlds, fixture->Authority, TICK_RATE, error)) {
			// Nothing to measure, and a row of zeroes is a worse answer than a
			// loud one. The benchmark binary has no assertions, so this is the
			// only way to say so.
			ENGINE_ERROR("studio bench: the play link would not start: {}", error);
		}

		for (int tick = 0; tick < SETTLE_TICKS; tick++) {
			fixture->Link.Step(fixture->Worlds);
			fixture->Worlds.Tick(FRAME_SECONDS);
		}

		built.emplace_back(parts, std::move(fixture));
		return *built.back().second;
	}

	// Writes a new transform to one part in `every`, or to none when `every` is
	// zero.
	void Move(Fixture &fixture, size_t every) {
		if (every == 0) {
			return;
		}

		fixture.Phase += 0.01f;
		const float phase = fixture.Phase;

		fixture.Worlds.Enter(fixture.Authority, [&fixture, every, phase](Store &store) {
			for (size_t index = 0; index < fixture.Parts.size(); index += every) {
				const float x = static_cast<float>(index % 256) * 4.0f + phase;
				const float z = static_cast<float>(index / 256) * 4.0f;
				store.Set<Transform>(fixture.Parts[index], Transform{CFrame(Vector3{x, 0.0f, z})});
			}
		});
	}

	// One frame of Play: the author's writes, the link, then the tick.
	//
	// **That order is `Editor::Simulate`'s and it is load bearing.** A world
	// clears its change bits at the *start* of a tick, so publishing first means
	// a value written between two ticks is still marked when it is read.
	// Publishing after the tick instead loses every write, because nothing in an
	// editor happens inside a system.
	void Frame(size_t parts, size_t every) {
		Fixture &fixture = FixtureOf(parts);
		Move(fixture, every);
		fixture.Link.Step(fixture.Worlds);
		fixture.Worlds.Tick(FRAME_SECONDS);
	}
}

using namespace playlink_bench;

// --- the floor: a scene nobody is touching ---------------------------------------
//
// Nothing has moved, so there is no delta to build and no bytes to send. What is
// left is `Survey` - the component resolution, `FindBearing` and `Resign` - plus
// one empty pass per client. **This row is the cost of having Play open.**

BENCH_PER_ITEM("Play frame · 2k parts · nothing moved", 2000) {
	Frame(2000, 0);
}

BENCH_PER_ITEM("Play frame · 8k parts · nothing moved", 8000) {
	Frame(8000, 0);
}

// --- the delta, by how much of the scene moved ------------------------------------
//
// The same scenes with a share of the parts written every frame. The gap from
// the floor is the replication proper; the floor itself does not move.

BENCH_PER_ITEM("Play frame · 2k parts · one part in ten moved", 2000) {
	Frame(2000, 10);
}

BENCH_PER_ITEM("Play frame · 2k parts · every part moved", 2000) {
	Frame(2000, 1);
}

// **There is no moving row at eight thousand parts, and the omission is
// deliberate.** A scene that size produces more changed bytes a tick than the
// per-client budget carries, whatever share of it moves, so the link never
// catches up: the client is declared adrift and the whole world is
// re-snapshotted every `ResnapshotAfterTicks`. What such a row reports is
// whether a re-snapshot happened to land inside the sample - measured at a
// spread a hundred times its own minimum, which detects nothing. The floor rows
// above cover that scene at the size where the numbers mean something, and what
// a backlogged link costs is `Protocol.cpp`'s hundred-client row.
