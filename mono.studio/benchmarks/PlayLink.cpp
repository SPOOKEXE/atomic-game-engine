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
// **One item is one part in one frame**, so a row divides straight into a
// per-part per-frame cost whatever its scene size or its batch length is.
//
// So `nothing moved` is the floor: whatever that row says is what an idle Play
// session costs per part per tick, for a picture identical to the last one. The
// gap up to `everything moved` is the delta, and the ratio between the two is
// the honest answer to "is this replication or is this bookkeeping". A change
// that makes `Survey` walk per entity instead of per archetype table moves the
// floor and leaves the ceiling alone, which no other suite would notice.
//
// **A Play frame is a tick of the world plus a step of the link, and the tick is
// the larger half.** That is why the first rows below run the identical scene
// with no link at all: the number this suite is about is the *difference*, and a
// row read on its own is mostly the world advancing. The fixture builds no
// physics pipeline - what physics costs is `bench_physics` and adding it here
// would only widen the part of the figure that is not replication.

#include <engine/core/Log.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
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
using engine::scene::PartDesc;
using engine::scene::Transform;
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
	// nothing like the steady state this suite is about. `AuthoritySettings`
	// carries eight kilobytes of snapshot a tick by default, so a scene of eight
	// thousand parts is some forty ticks of streaming before the first delta -
	// and a fixture that stopped settling at sixty-four was measuring the tail of
	// its own join, which reads as a big scene being cheaper per part than a
	// small one.
	//
	// Four times that, so the streaming, the acknowledgement round trip and the
	// first deltas are all behind the measurement. Settling costs a fraction of a
	// second per fixture and it happens once.
	constexpr int SETTLE_TICKS = 256;

	// How many frames one sample averages over.
	//
	// **One frame is too short a sample to survive a busy machine.** A frame here
	// is a couple of hundred microseconds, so a single scheduler preemption is
	// the whole measurement - the rows swung sevenfold between runs of the same
	// binary while a build was running beside them, on the *minimum*, which is
	// supposed to be the figure a busy machine cannot move. A batch of frames per
	// sample averages that out without averaging away a real regression.
	//
	// It also spreads the periodic costs a link really has - the re-snapshot a
	// client that has fallen behind triggers every `ResnapshotAfterTicks` - over
	// the samples instead of leaving them to land in one and be discarded as the
	// maximum.
	constexpr size_t FRAMES = 64;

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
	//
	// The world holds `parts` drawable parts and as many plain instances beside
	// them, which is roughly the mix of an authored place: geometry, and the
	// folders, scripts and values holding it together.
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

	// A fixture of `parts` drawable entities, built once per row.
	//
	// **Keyed by what the row does to it and not only by its size, because a
	// link carries state between frames.** Two rows sharing one fixture is two
	// rows sharing an outstanding set and a snapshot backlog: whichever ran
	// second inherited whatever the first left behind, so the numbers depended
	// on declaration order and the moving rows all converged on one figure. That
	// is a fixture measuring the row above it.
	//
	// Lazily and inside the body, because a universe binds its driver thread on
	// construction and a store binds its owning thread - neither of which is the
	// thread that runs a namespace static.
	Fixture &FixtureOf(size_t parts, size_t every, bool linked) {
		struct Key {
			size_t Parts;
			size_t Every;
			bool Linked;

			bool operator==(const Key &other) const {
				return Parts == other.Parts && Every == other.Every && Linked == other.Linked;
			}
		};

		static std::vector<std::pair<Key, std::unique_ptr<Fixture>>> built;
		const Key key{parts, every, linked};
		for (auto &[made, fixture] : built) {
			if (made == key) {
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
				// **`MakePart` and not a hand-assembled entity, and the
				// difference decides what this suite measures.** A part is an
				// *instance*: it carries `ecs.Hierarchy`, `ecs.InstanceName` and
				// `ecs.InstanceClass` as well as its transform and its look, and
				// those three are replicated with signature detection - so every
				// instance in the world is hashed three times a tick whether or
				// not anything touched it. That is most of what `Resign` does in
				// a real editor. A fixture of bare entities carries none of them
				// and reported the same figure before and after `Resign` was
				// made four times faster, which is a benchmark agreeing with
				// itself rather than with the program.
				PartDesc desc;

				// Spread along a grid rather than stacked at the origin, so the
				// priority ordering has distinct distances to sort by - a scene
				// where every score ties measures a sort that never swaps.
				desc.Frame = CFrame(
					Vector3{
						static_cast<float>(index % 256) * 4.0f, 0.0f, static_cast<float>(index / 256) * 4.0f
					}
				);

				fixture->Parts.push_back(engine::scene::MakePart(store, desc));
			}

			// **The furniture, which is most of an authored place and none of
			// its geometry.** Folders, scripts, values and services are
			// instances with no `scene.` component on them at all, so they carry
			// the three instance components and nothing else - and the
			// replication table declares every registered `scene.` and `gui.`
			// component ahead of those three. A survey that asks each entity in
			// turn walks that whole list before it finds the one component a
			// folder has. One per part is conservative for a real place.
			const engine::ecs::ClassId root = engine::ecs::Classes::RegisterInstanceRoot();
			for (size_t index = 0; index < parts; index++) {
				store.CreateInstance(root);
			}
		});

		if (linked) {
			std::string error;
			if (!fixture->Link.Start(fixture->Worlds, fixture->Authority, TICK_RATE, error)) {
				// Nothing to measure, and a row of zeroes is a worse answer than
				// a loud one. The benchmark binary has no assertions, so this is
				// the only way to say so.
				ENGINE_ERROR("studio bench: the play link would not start: {}", error);
			}
		}

		// **Settled the way the row will drive it.** A link settled idle and then
		// handed a moving scene spends its first samples draining a backlog that
		// the steady state does not have, which is the first sample being the
		// slowest and the minimum being taken from the last.
		for (int tick = 0; tick < SETTLE_TICKS; tick++) {
			Move(*fixture, every);
			if (linked) {
				fixture->Link.Step(fixture->Worlds);
			}
			fixture->Worlds.Tick(FRAME_SECONDS);
		}

		built.emplace_back(key, std::move(fixture));
		return *built.back().second;
	}

	// `FRAMES` frames of Play: the author's writes, the link, then the tick.
	//
	// **That order is `Editor::Simulate`'s and it is load bearing.** A world
	// clears its change bits at the *start* of a tick, so publishing first means
	// a value written between two ticks is still marked when it is read.
	// Publishing after the tick instead loses every write, because nothing in an
	// editor happens inside a system.
	void Frames(size_t parts, size_t every, bool linked) {
		Fixture &fixture = FixtureOf(parts, every, linked);
		for (size_t frame = 0; frame < FRAMES; frame++) {
			Move(fixture, every);
			if (linked) {
				fixture.Link.Step(fixture.Worlds);
			}
			fixture.Worlds.Tick(FRAME_SECONDS);
		}
	}
}

using namespace playlink_bench;

// --- the control: the same frame with Play closed -------------------------------
//
// **Read every row below against this one.** A Play frame is a tick of the world
// *and* a step of the link, and the tick is not small: it advances two worlds,
// re-derives what is drawable, and runs whatever the place scripts. Timing the
// pair and calling the figure "replication" would have credited the link with
// most of a cost it does not own - the first version of this suite did exactly
// that, and reported no change across a rewrite that made `Survey` four times
// faster.
//
// So this row runs the identical scene with no link started at all. **The
// difference between a row below and the row beside it here is what hosting one
// client costs**, and that difference is the number a replication change moves.

BENCH_PER_ITEM("Play frame · 2k parts, 2k plain instances · control, no link", 2000 * FRAMES) {
	Frames(2000, 0, false);
}

BENCH_PER_ITEM("Play frame · 8k parts, 8k plain instances · control, no link", 8000 * FRAMES) {
	Frames(8000, 0, false);
}

// --- the floor: a scene nobody is touching ---------------------------------------
//
// Nothing has moved, so there is no delta to build and no bytes to send. What is
// left over the control is `Survey` - the component resolution, `FindBearing`
// and `Resign` - plus one empty pass per client. **That difference is the cost
// of having Play open.**

BENCH_PER_ITEM("Play frame · 2k parts, 2k plain instances · nothing moved", 2000 * FRAMES) {
	Frames(2000, 0, true);
}

BENCH_PER_ITEM("Play frame · 8k parts, 8k plain instances · nothing moved", 8000 * FRAMES) {
	Frames(8000, 0, true);
}

// --- the delta, by how much of the scene moved ------------------------------------
//
// The same scenes with a share of the parts written every frame. The gap from the
// floor is the replication proper; the floor itself does not move.

BENCH_PER_ITEM("Play frame · 2k parts, 2k plain instances · one part in ten moved", 2000 * FRAMES) {
	Frames(2000, 10, true);
}

BENCH_PER_ITEM("Play frame · 8k parts, 8k plain instances · one part in ten moved", 8000 * FRAMES) {
	Frames(8000, 10, true);
}

// **No row moves the whole scene, and the omission is deliberate.** Two thousand
// transforms a tick is already more changed bytes than one client's byte budget
// carries, so the link never catches up: the outstanding set grows, the client is
// eventually declared adrift, and the whole world is re-snapshotted every
// `ResnapshotAfterTicks`. What such a row reports is whether a re-snapshot
// happened to land inside the sample, which detects nothing. What a link that
// cannot keep up costs is a real question and `Protocol.cpp`'s hundred-client row
// is where it is asked.
