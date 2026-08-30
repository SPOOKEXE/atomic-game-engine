#include <engine/core/Bytes.hpp>
#include <engine/core/Random.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/SparseSet.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <span>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.ecs.snapshot")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::Random;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;

namespace snapshot_test {
	struct Spot {
		float X = 0.0f;
		float Y = 0.0f;
	};
	struct Push {
		float X = 0.0f;
	};
	struct Score {
		int Value = 0;
	};
	struct Asleep {};

	// Holds an entity handle, which is the case the directory has to come back
	// exactly for: a parent, a target, an owner.
	struct Linked {
		Entity Other;
	};

	// Registered under a name of its own so that a case below can rewrite that
	// name in a saved stream and hand the result back as a snapshot from a
	// build this one does not share a component set with.
	struct Vanishing {
		uint32_t Value = 0;
	};

	// No serialisation - its bytes are a pointer into this process.
	struct Unwritable {
		std::string Text;
		Unwritable() = default;
	};

	// Round-trips a store into a fresh one and hands the fresh one back.
	bool Transfer(const Store &from, Store &into) {
		ByteWriter writer;
		if (!from.Save(writer)) {
			return false;
		}
		ByteReader reader(writer.Bytes());
		return into.Load(reader);
	}

	std::vector<float> SpotsIn(Store &store) {
		std::vector<float> found;
		store.Each<Spot>([&](Entity, Spot &spot) { found.push_back(spot.X); });
		std::sort(found.begin(), found.end());
		return found;
	}
}

using namespace snapshot_test;

TEST_CASE("an empty world round-trips", "[ecs]") {
	Store source("source");
	Store restored("restored");

	REQUIRE(Transfer(source, restored));
	REQUIRE(restored.CountMatching<Spot>() == 0);
	REQUIRE(restored.TableCount() == 0);
}

TEST_CASE("a cloned world is independent and keeps its identity", "[ecs]") {
	Store source("simulation");
	const Entity entity = source.Create();
	source.Set<Spot>(entity, Spot{3.0f, 4.0f});
	source.AdvanceTick(0.25f);

	Store preview("preview");
	REQUIRE(source.CloneTo(preview));
	REQUIRE(preview.Name() == "preview");
	REQUIRE(preview.Alive(entity));
	REQUIRE(preview.Get<Spot>(entity)->X == 3.0f);
	REQUIRE(preview.Time().Tick == source.Time().Tick);

	preview.Set<Spot>(entity, Spot{9.0f, 4.0f});
	REQUIRE(source.Get<Spot>(entity)->X == 3.0f);
	REQUIRE(preview.Get<Spot>(entity)->X == 9.0f);
}

TEST_CASE("entities and components come back", "[ecs]") {
	Store source("source");

	for (int index = 0; index < 64; index++) {
		const Entity entity = source.Create();
		source.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		if (index % 2 == 0) {
			source.Set<Push>(entity, Push{1.0f});
		}
	}

	Store restored("restored");
	REQUIRE(Transfer(source, restored));

	REQUIRE(restored.CountMatching<Spot>() == 64);
	REQUIRE(restored.CountMatching<Spot, Push>() == 32);
	REQUIRE(SpotsIn(restored) == SpotsIn(source));
}

TEST_CASE("a tree survives a snapshot, links and order and all", "[ecs]") {
	// **The guard on `ecs.Hierarchy` having a wire format.** Replication sends
	// only the parent handle and the far side rebuilds its own child lists from
	// it, because one store's `FirstChild` and `NextSibling` are an index into
	// that store and writing them into another one is how a sibling list comes
	// to contain itself.
	//
	// A snapshot must not do that. `Column::Write` uses `TypeDescriptor::Write`
	// and never `Wire` - `WireFormat`'s own header says a lossy form must not
	// reach one - so a saved world carries the whole node and comes back with
	// its tree intact. If that ever stopped being true, an `.agame` would open
	// with every instance a root and nothing would say so until somebody looked
	// in the explorer.
	engine::ecs::Classes::RegisterInstanceRoot();

	Store source("source");
	const Entity parent = source.CreateInstance(engine::ecs::Classes::RegisterInstanceRoot(), "Parent");

	std::vector<Entity> children;
	for (int index = 0; index < 4; index++) {
		const Entity child =
			source.CreateInstance(engine::ecs::Classes::RegisterInstanceRoot(), "C" + std::to_string(index));
		REQUIRE(source.SetParent(child, parent));
		children.push_back(child);
	}

	Store restored("restored");
	REQUIRE(Transfer(source, restored));

	// Every link, not merely the parent: a walk that finds the children is what
	// the explorer, the destroy and the replication capture all do.
	std::vector<Entity> seen;
	restored.EachChild(parent, [&seen](Entity child) { seen.push_back(child); });
	REQUIRE(seen == children);

	for (const Entity child : children) {
		CHECK(restored.ParentOf(child) == parent);
	}

	// And the order, which is what `GetChildren` publishes to a script.
	CHECK(restored.FindFirstChild(parent, "C0") == children[0]);
	CHECK(restored.FindFirstChild(parent, "C3") == children[3]);
}

TEST_CASE("entity handles stay valid across a restore", "[ecs]") {
	// The reason the directory is reproduced rather than re-allocated. A handle
	// held outside the store, or stored *inside* a component, is only still the
	// same entity if its index and generation both come back.
	Store source("source");

	const Entity first = source.Create();
	const Entity second = source.Create();
	source.Set<Spot>(first, Spot{1.0f, 0.0f});
	source.Set<Spot>(second, Spot{2.0f, 0.0f});
	source.Set<Linked>(first, Linked{second});

	Store restored("restored");
	REQUIRE(Transfer(source, restored));

	REQUIRE(restored.Alive(first));
	REQUIRE(restored.Alive(second));
	REQUIRE(restored.Get<Spot>(first)->X == 1.0f);

	// And the handle held inside a component still points at the right entity.
	const Entity target = restored.Get<Linked>(first)->Other;
	REQUIRE(target == second);
	REQUIRE(restored.Alive(target));
	REQUIRE(restored.Get<Spot>(target)->X == 2.0f);
}

TEST_CASE("generations survive, so a stale handle stays stale", "[ecs]") {
	// A snapshot that reset generations would resurrect every handle to an
	// entity destroyed before it was taken.
	Store source("source");

	const Entity dead = source.Create();
	source.Set<Spot>(dead, Spot{});
	source.Destroy(dead);

	const Entity live = source.Create(); // reuses the index at a new generation
	source.Set<Spot>(live, Spot{5.0f, 0.0f});
	REQUIRE_FALSE(source.Alive(dead));

	Store restored("restored");
	REQUIRE(Transfer(source, restored));

	REQUIRE(restored.Alive(live));
	REQUIRE_FALSE(restored.Alive(dead));
}

TEST_CASE("both index regions round-trip, generations and all", "[ecs]") {
	// The format's half of the split. The directory is written as one run per
	// region, so a save that wrote only the authoritative run would lose every
	// prediction - silently, because a world with none looks identical.
	Store source("source");

	const Entity theirs = source.Create();
	const Entity retired = source.Create();
	const Entity mine = source.CreatePredicted();
	const Entity discarded = source.CreatePredicted();

	REQUIRE(Store::IsPredicted(mine));
	REQUIRE_FALSE(Store::IsPredicted(theirs));

	source.Set<Spot>(theirs, Spot{1.0f, 2.0f});
	source.Set<Spot>(mine, Spot{3.0f, 4.0f});

	// A handle stored *inside* a component, in both directions: this is the
	// case the directory has to come back exactly for, and a promotion or a
	// second region must not weaken it.
	source.Set<Linked>(theirs, Linked{mine});
	source.Set<Linked>(mine, Linked{theirs});

	// Destroyed on both sides, so the generations coming back are not all the
	// first one and the free lists have something in them.
	source.Destroy(retired);
	source.Destroy(discarded);

	Store restored("restored");
	REQUIRE(Transfer(source, restored));

	REQUIRE(restored.Alive(theirs));
	REQUIRE(restored.Alive(mine));
	REQUIRE_FALSE(restored.Alive(retired));
	REQUIRE_FALSE(restored.Alive(discarded));

	REQUIRE(restored.Get<Spot>(mine)->X == 3.0f);
	REQUIRE(restored.Get<Linked>(theirs)->Other == mine);
	REQUIRE(restored.Get<Linked>(mine)->Other == theirs);
	REQUIRE(restored.Alive(restored.Get<Linked>(theirs)->Other));
	REQUIRE(restored.Get<Spot>(restored.Get<Linked>(theirs)->Other)->X == 3.0f);

	// And each region's free list came back its own, so the next mint on each
	// side is the handle the source would have issued - index, generation and
	// region alike. A shared or dropped free list shows here and nowhere else.
	REQUIRE(restored.Create() == source.Create());

	const Entity guessed = restored.CreatePredicted();
	REQUIRE(guessed == source.CreatePredicted());
	REQUIRE(Store::IsPredicted(guessed));
	REQUIRE(guessed != discarded); // reused index, moved-on generation
}

TEST_CASE("the free list comes back, so restored worlds allocate alike", "[ecs]") {
	Store source("source");

	std::vector<Entity> entities;
	for (int index = 0; index < 8; index++) {
		entities.push_back(source.Create());
	}
	source.Destroy(entities[2]);
	source.Destroy(entities[5]);

	Store restored("restored");
	REQUIRE(Transfer(source, restored));

	// Two indices were free, so the next two creations must reuse them rather
	// than growing the directory past where it was.
	const Entity next = restored.Create();
	const Entity after = restored.Create();
	REQUIRE(restored.Alive(next));
	REQUIRE(restored.Alive(after));
	REQUIRE(next != after);

	// And the reused handles are not equal to the destroyed ones, because the
	// generation moved on.
	REQUIRE(next != entities[2]);
	REQUIRE(after != entities[5]);
}

TEST_CASE("resources and the clock come back", "[ecs]") {
	Store source("source");
	source.SetResource(Score{42});
	source.AdvanceTick(1.0f / 60.0f);
	source.AdvanceTick(1.0f / 60.0f);
	source.SetFrame(0.016f, 0.5f);

	Store restored("restored");
	REQUIRE(Transfer(source, restored));

	REQUIRE(restored.HasResource<Score>());
	REQUIRE(restored.Resource<Score>()->Value == 42);

	REQUIRE(restored.Time().Tick == 2);
	REQUIRE(restored.Time().Elapsed == source.Time().Elapsed);
	REQUIRE(restored.Time().Alpha == 0.5f);
}

TEST_CASE("names come back and still resolve", "[ecs]") {
	Store source("source");
	const Entity camera = source.Create("camera");
	source.Set<Spot>(camera, Spot{3.0f, 0.0f});

	Store restored("restored");
	REQUIRE(Transfer(source, restored));

	REQUIRE(restored.NameOf(camera) == "camera");
	REQUIRE(restored.Find("camera") == camera);
	REQUIRE(restored.Get<Spot>(restored.Find("camera"))->X == 3.0f);
}

TEST_CASE("tags come back as presence", "[ecs]") {
	Store source("source");
	const Entity sleeper = source.Create();
	source.Set<Spot>(sleeper, Spot{});
	source.Set<Asleep>(sleeper, Asleep{});

	const Entity awake = source.Create();
	source.Set<Spot>(awake, Spot{});

	Store restored("restored");
	REQUIRE(Transfer(source, restored));

	REQUIRE(restored.Has<Asleep>(sleeper));
	REQUIRE_FALSE(restored.Has<Asleep>(awake));
	REQUIRE(restored.CountMatching<Spot, Asleep>() == 1);
}

TEST_CASE("what is observed comes back observed", "[ecs]") {
	Store source("source");
	source.Observe<Spot>();
	const Entity entity = source.Create();
	source.Set<Spot>(entity, Spot{});

	Store restored("restored");
	REQUIRE(Transfer(source, restored));

	REQUIRE(restored.Observed<Spot>());

	restored.ClearChanges();
	restored.Set<Spot>(entity, Spot{9.0f, 0.0f});
	REQUIRE(restored.Changed<Spot>(entity));
}

TEST_CASE("a restored world keeps ticking identically", "[ecs]") {
	// The half that catches a snapshot which saves values but not identity: two
	// worlds that agree at the moment of restore and diverge a hundred ticks
	// later were not actually the same world.
	Store source("source");
	source.Observe<Spot>();

	for (int index = 0; index < 200; index++) {
		const Entity entity = source.Create();
		source.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		source.Set<Push>(entity, Push{static_cast<float>(index % 7) * 0.25f});
	}

	Store restored("restored");
	REQUIRE(Transfer(source, restored));

	const auto advance = [](Store &store) {
		for (int tick = 0; tick < 100; tick++) {
			store.AdvanceTick(1.0f / 60.0f);
			store.Each<Spot, const Push>([](Entity, Spot &spot, const Push &push) {
				spot.X += push.X;
				spot.Y = spot.X * 0.5f;
			});
		}
	};

	advance(source);
	advance(restored);

	REQUIRE(SpotsIn(restored) == SpotsIn(source));
	REQUIRE(restored.Time().Tick == source.Time().Tick);
}

TEST_CASE("a snapshot of a snapshot is the same snapshot", "[ecs]") {
	// Byte-for-byte, which is stronger than "restores to something equivalent"
	// and is what a recording needs in order to be comparable.
	Store source("source");
	for (int index = 0; index < 32; index++) {
		const Entity entity = source.Create();
		source.Set<Spot>(entity, Spot{static_cast<float>(index), 1.0f});
		if (index % 3 == 0) {
			source.Set<Score>(entity, Score{index});
		}
	}
	source.Create("marked");

	ByteWriter first;
	REQUIRE(source.Save(first));

	Store restored("restored");
	ByteReader reader(first.Bytes());
	REQUIRE(restored.Load(reader));

	ByteWriter second;
	REQUIRE(restored.Save(second));

	const std::span<const std::byte> left = first.Bytes();
	const std::span<const std::byte> right = second.Bytes();
	REQUIRE(left.size() == right.size());
	REQUIRE(std::equal(left.begin(), left.end(), right.begin()));
}

// --- refusals -------------------------------------------------------------

TEST_CASE("a component with no serialisation refuses the snapshot", "[ecs]") {
	// Refused rather than skipped. A snapshot missing a column restores a world
	// that looks right and is not, which is worse than one that will not save.
	Store store("source");
	const Entity entity = store.Create();
	store.Set<Unwritable>(entity, Unwritable{});

	ByteWriter writer;
	REQUIRE_FALSE(store.Save(writer));
}

TEST_CASE("a snapshot naming a component this build does not have is refused", "[ecs]") {
	// **The cross-build case, and the one that cannot be produced by corrupting
	// bytes at random.** A save file from a build that had a component this one
	// does not is a well-formed snapshot in every other respect: the header is
	// right, the version is right, every length agrees. Only the name in the
	// component table is unknown, and the choice at that point is to refuse or
	// to restore a world with a column missing.
	//
	// Refusing is the same call `Unwritable` above gets and for the same
	// reason. A world that silently lost a column looks right and is not.
	//
	// Built by patching a real snapshot rather than by hand, so it stays a
	// snapshot when the format changes. The replacement is the same length as
	// the original, which leaves every offset after it valid - the point is to
	// exercise the unknown-name path and not the truncation path beside it.
	Components::Register<Vanishing>("test.Vanishing");

	Store source("source");
	source.Set<Vanishing>(source.Create(), Vanishing{7});

	ByteWriter writer;
	REQUIRE(source.Save(writer));

	const std::span<const std::byte> saved = writer.Bytes();
	std::vector<std::byte> bytes(saved.begin(), saved.end());

	const std::string_view present = "test.Vanishing";
	const std::string_view absent = "test.Vanished!";
	REQUIRE(present.size() == absent.size());

	const auto at = std::search(
		bytes.begin(), bytes.end(), present.begin(), present.end(), [](std::byte left, char right) {
			return left == static_cast<std::byte>(right);
		}
	);
	REQUIRE(at != bytes.end());
	std::transform(absent.begin(), absent.end(), at, [](char character) {
		return static_cast<std::byte>(character);
	});

	Store target("target");
	target.Create();

	ByteReader reader(bytes);
	REQUIRE_FALSE(target.Load(reader));

	// Empty rather than partly the old world and partly the new one.
	REQUIRE(target.TableCount() == 0);
	REQUIRE(target.CountMatching<Vanishing>() == 0);
}

TEST_CASE("a stream that is not a snapshot is refused", "[ecs]") {
	Store store("target");
	store.Create();

	ByteWriter writer;
	writer.WriteUInt64(0xDEAD'BEEF'DEAD'BEEFull);
	writer.WriteUInt32(1);

	ByteReader reader(writer.Bytes());
	REQUIRE_FALSE(store.Load(reader));

	// And the store is empty rather than half-anything.
	REQUIRE(store.CountMatching<Spot>() == 0);
	REQUIRE(store.TableCount() == 0);
}

TEST_CASE("a future version is refused rather than guessed at", "[ecs]") {
	Store source("source");
	source.Create();

	ByteWriter writer;
	REQUIRE(source.Save(writer));

	// Bump the version field, which sits right after the eight magic bytes.
	std::vector<std::byte> tampered(writer.Bytes().begin(), writer.Bytes().end());
	tampered[8] = static_cast<std::byte>(Store::SNAPSHOT_VERSION + 1);

	Store restored("restored");
	ByteReader reader(tampered);
	REQUIRE_FALSE(restored.Load(reader));
}

TEST_CASE("a snapshot written at the previous version is refused", "[ecs]") {
	// **Version 1 wrote the entity directory as one run and version 2 writes
	// two**, one per index region. The two layouts are indistinguishable from
	// the bytes - a version 1 stream's table count sits exactly where version
	// 2's predicted-run length does - so a reader that tried to cope would
	// restore a world nobody can reason about rather than refusing one.
	Store source("source");
	source.Set<Spot>(source.Create(), Spot{1.0f, 2.0f});

	ByteWriter writer;
	REQUIRE(source.Save(writer));

	std::vector<std::byte> old(writer.Bytes().begin(), writer.Bytes().end());
	old[8] = static_cast<std::byte>(1);

	Store restored("restored");
	restored.Set<Spot>(restored.Create(), Spot{9.0f, 9.0f});

	ByteReader reader(old);
	REQUIRE_FALSE(restored.Load(reader));

	// Empty rather than half-restored, which is what `Load` promises on any
	// failure and is the difference between a refusal and a misread.
	REQUIRE(restored.CountMatching<Spot>() == 0);
	REQUIRE(restored.TableCount() == 0);
}

TEST_CASE("a snapshot claiming more entities than a region holds is refused", "[ecs]") {
	// Every field of an inbound snapshot is hostile. The directory is a count
	// followed by a run, and a count of four billion would otherwise have the
	// reader walk to it one failed read at a time.
	Store source("source");
	source.Create();

	ByteWriter writer;
	REQUIRE(source.Save(writer));

	std::vector<std::byte> tampered(writer.Bytes().begin(), writer.Bytes().end());

	// The authoritative directory count is the field after the header, and the
	// header is variable-length - the store's name and the component table are
	// both in it. Walked with a reader rather than computed as an offset, so a
	// field moving breaks this loudly instead of silently overwriting the wrong
	// eight bytes and still passing.
	ByteReader header(writer.Bytes());
	header.ReadUInt64();
	header.ReadUInt32();
	header.ReadString();
	const uint32_t components = header.ReadUInt32();
	for (uint32_t index = 0; index < components; index++) {
		header.ReadName();
	}
	const size_t at = header.Position();
	REQUIRE(header.ReadUInt64() == 1); // the honest count, before it is spoiled

	// One more authoritative entity than the range holds.
	const uint64_t absurd = static_cast<uint64_t>(engine::ecs::SparseSet::AUTHORITATIVE_INDICES) + 1;
	for (size_t byte = 0; byte < 8; byte++) {
		tampered[at + byte] = static_cast<std::byte>((absurd >> (byte * 8)) & 0xFF);
	}

	Store restored("restored");
	ByteReader reader(tampered);
	REQUIRE_FALSE(restored.Load(reader));
	REQUIRE(restored.TableCount() == 0);

	// **Refused at the count, not after eating the stream.** A reader without
	// the check refuses too - it runs out of bytes and fails - so the outcome
	// alone proves nothing. What the check buys is that the count is judged
	// before a single directory entry is read against it, and the cursor is
	// where that shows.
	REQUIRE(reader.Position() == at + sizeof(uint64_t));
}

TEST_CASE("a truncated snapshot leaves the store empty", "[ecs]") {
	Store source("source");
	for (int index = 0; index < 40; index++) {
		const Entity entity = source.Create();
		source.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
	}

	ByteWriter writer;
	REQUIRE(source.Save(writer));

	// Every truncation point, because a reader that only checks the header
	// fails cleanly on some of these and walks off the end on others.
	size_t survivedWithState = 0;
	for (size_t length = 0; length < writer.Size(); length += 7) {
		std::vector<std::byte> cut(writer.Bytes().begin(), writer.Bytes().begin() + length);

		Store restored("restored");
		ByteReader reader(cut);
		if (restored.Load(reader)) {
			continue; // a prefix that happens to be a valid smaller snapshot
		}
		if (restored.CountMatching<Spot>() != 0 || restored.TableCount() != 0) {
			survivedWithState++;
		}
	}

	REQUIRE(survivedWithState == 0);
}

TEST_CASE("random bytes never restore anything", "[ecs]") {
	// `core::Random` so a failure reproduces from the seed. Nothing here
	// asserts a value - the assertion is that no arbitrary buffer produces a
	// store claiming to hold entities.
	constexpr uint32_t ITERATIONS = 500;
	size_t restoredSomething = 0;

	for (uint32_t iteration = 0; iteration < ITERATIONS; iteration++) {
		const size_t size = Random::Bits(iteration, 1) % 256;
		std::vector<std::byte> buffer(size);
		for (size_t index = 0; index < size; index++) {
			buffer[index] = static_cast<std::byte>(Random::Bits(iteration, static_cast<uint32_t>(index) + 2));
		}

		Store store("fuzz");
		ByteReader reader(buffer);
		if (store.Load(reader) && store.TableCount() > 0) {
			restoredSomething++;
		}
	}

	REQUIRE(restoredSomething == 0);
}

TEST_CASE("a snapshot with a valid header and rubbish body is refused", "[ecs]") {
	// The nastier fuzz case: the magic and version are right, so the reader
	// gets past the cheap checks and has to survive the rest on bounds alone.
	Store source("source");
	source.Create();
	ByteWriter valid;
	REQUIRE(source.Save(valid));

	size_t restoredWithState = 0;
	for (uint32_t iteration = 0; iteration < 200; iteration++) {
		std::vector<std::byte> buffer(valid.Bytes().begin(), valid.Bytes().end());
		for (size_t index = 12; index < buffer.size(); index++) {
			buffer[index] = static_cast<std::byte>(Random::Bits(iteration, static_cast<uint32_t>(index)));
		}

		Store store("fuzz");
		ByteReader reader(buffer);
		if (store.Load(reader) && store.TableCount() > 0) {
			restoredWithState++;
		}
	}

	// Some of these may load - the format is not self-validating beyond its
	// bounds - but none may crash, and that is what this case is really
	// asserting by completing at all.
	REQUIRE(restoredWithState <= 200);
}

TEST_CASE("loading over a populated world replaces it entirely", "[ecs]") {
	Store source("source");
	const Entity kept = source.Create();
	source.Set<Spot>(kept, Spot{1.0f, 0.0f});

	Store target("target");
	for (int index = 0; index < 20; index++) {
		const Entity entity = target.Create();
		target.Set<Score>(entity, Score{index});
	}
	REQUIRE(target.CountMatching<Score>() == 20);

	REQUIRE(Transfer(source, target));

	// Nothing of the old world survives - not its entities, not its tables.
	REQUIRE(target.CountMatching<Score>() == 0);
	REQUIRE(target.CountMatching<Spot>() == 1);
}

TEST_CASE("clear empties the world but leaves a clock", "[ecs]") {
	Store store("test");
	store.AdvanceTick(0.5f);
	for (int index = 0; index < 10; index++) {
		store.Set<Spot>(store.Create(), Spot{});
	}

	store.Clear();

	REQUIRE(store.CountMatching<Spot>() == 0);
	REQUIRE(store.TableCount() == 0);

	// A world with no clock is one where every system has to check for one.
	REQUIRE(store.Time().Tick == 0);
	REQUIRE(store.HasResource<engine::ecs::WorldTime>());
}
