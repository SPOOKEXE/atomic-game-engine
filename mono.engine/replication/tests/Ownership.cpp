// What a client is allowed to write, and what happens to the rest.
//
// **The trust boundary, so these are adversarial rather than illustrative.**
// Every case here is a client saying something it should not be able to say:
// state for an entity it does not own, a component the server never sends it,
// last tick's position arriving after this tick's, and - the one that motivated
// the whole shape of `WriteComponents` - a delta whose owned entity is second in
// a packed value stream, where a filter that skipped bytes instead of reading
// them would land the wrong client's position on it.
//
// The policy those enforce is `Authority::SetOwnership`'s: the engine decides
// *who*, the host decides *what*, and neither pretends to do the other's half.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Submission.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

TEST_SUITE_ID("engine.replication.ownership")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::replication::ApplyStatus;
using engine::replication::Authority;
using engine::replication::BuildSubmission;
using engine::replication::ClientId;
using engine::replication::Delta;

namespace ownership_test {
	struct Spot {
		float X = 0.0f;
	};

	// Replicated by nobody here, which is the point: owning an entity does not
	// grant a name.
	struct Hidden {
		int Value = 0;
	};

	void RegisterTypes() {
		static bool once = [] {
			engine::ecs::Components::Register<Spot>("ownership_test.Spot");
			engine::ecs::Components::Register<Hidden>("ownership_test.Hidden");
			return true;
		}();
		(void)once;
	}

	// The bytes a client would put on the wire.
	std::vector<std::byte> Encode(const Delta &delta) {
		engine::core::ByteWriter writer;
		WriteMessage(writer, delta);
		return {writer.Bytes().begin(), writer.Bytes().end()};
	}

	// A server with one admitted client and a world to write into.
	struct Server {
		Server() : World("ownership_server") {
			RegisterTypes();
			Authority_.Replicate(Name("ownership_test.Spot"));
		}

		// Hands `entity` to the client, and nothing else.
		void Owns(Entity entity) {
			Authority_.SetOwnership([entity](ClientId, Entity subject, const Store &) {
				return subject == entity;
			});
		}

		bool Receive(const Delta &delta) {
			return Authority_.Receive(Handle, Encode(delta));
		}

		ApplyStatus Apply() {
			return Authority_.ApplySubmitted(Handle, World);
		}

		Store World;
		Authority Authority_;
		ClientId Handle = Authority_.Admit();
	};

	// A delta naming `entities` with one `Spot` each, values in order.
	Delta SpotsFor(std::span<const Entity> entities, std::span<const float> values, uint64_t tick) {
		Store scratch("ownership_client");
		for (size_t index = 0; index < entities.size(); index++) {
			scratch.CreateAt(entities[index]);
			scratch.Set<Spot>(entities[index], Spot{values[index]});
		}

		const std::array<Name, 1> components{Name("ownership_test.Spot")};
		return BuildSubmission(scratch, tick, entities, components);
	}
}

using namespace ownership_test;

TEST_CASE("an authority told nothing accepts nothing", "[replication][ownership]") {
	// **The default is refusal, and that is the decision.** Accepting until
	// somebody remembers to restrict it would make the insecure state the one a
	// host gets by forgetting - which is how most of the bugs this module's
	// AGENTS.md is about begin.
	Server server;

	const Entity mine = server.World.Create();
	server.World.Set<Spot>(mine, Spot{1.0f});

	const std::array<Entity, 1> entities{mine};
	const std::array<float, 1> values{99.0f};

	CHECK_FALSE(server.Receive(SpotsFor(entities, values, 1)));
	CHECK(server.Apply() == ApplyStatus::Ok);
	CHECK(server.World.Get<Spot>(mine)->X == 1.0f);
}

TEST_CASE("a client writes what it owns and nothing else", "[replication][ownership]") {
	Server server;

	const Entity mine = server.World.Create();
	const Entity theirs = server.World.Create();
	server.World.Set<Spot>(mine, Spot{1.0f});
	server.World.Set<Spot>(theirs, Spot{2.0f});
	server.Owns(mine);

	const std::array<Entity, 2> entities{mine, theirs};
	const std::array<float, 2> values{10.0f, 20.0f};

	REQUIRE(server.Receive(SpotsFor(entities, values, 1)));
	REQUIRE(server.Apply() == ApplyStatus::Ok);

	CHECK(server.World.Get<Spot>(mine)->X == 10.0f);
	CHECK(server.World.Get<Spot>(theirs)->X == 2.0f);

	// Counted rather than only dropped. A steady zero is a game where nobody is
	// trying; anything else is a client submitting state for something it was
	// not handed.
	CHECK(server.Authority_.Stats().Unowned == 1);
}

TEST_CASE("a refused value does not shift the ones after it", "[replication][ownership]") {
	// **The case the whole design is arranged around.** A component's values
	// are one packed stream in entity order, so a filter that dropped an entity
	// *before* the write - by stripping it from the entity list - would leave
	// its bytes in the stream and put them on the next entity along. Here the
	// owned entity is second, so that mistake lands the unowned client's value
	// on the owned row and every check above still passes.
	Server server;

	const Entity theirs = server.World.Create();
	const Entity mine = server.World.Create();
	server.World.Set<Spot>(theirs, Spot{2.0f});
	server.World.Set<Spot>(mine, Spot{1.0f});
	server.Owns(mine);

	const std::array<Entity, 2> entities{theirs, mine};
	const std::array<float, 2> values{700.0f, 42.0f};

	REQUIRE(server.Receive(SpotsFor(entities, values, 1)));
	REQUIRE(server.Apply() == ApplyStatus::Ok);

	// 42, which is its own value. 700 would be the shift.
	CHECK(server.World.Get<Spot>(mine)->X == 42.0f);
	CHECK(server.World.Get<Spot>(theirs)->X == 2.0f);
}

TEST_CASE("owning an entity does not grant a component", "[replication][ownership]") {
	// A component the authority does not replicate is one the client was never
	// sent and has no business setting - server-side AI state, a pending bus
	// request. Ownership answers *which entity*, not *which fields of it*.
	Server server;

	const Entity mine = server.World.Create();
	server.World.Set<Hidden>(mine, Hidden{7});
	server.Owns(mine);

	Store scratch("ownership_client");
	scratch.CreateAt(mine);
	scratch.Set<Hidden>(mine, Hidden{999});

	const std::array<Entity, 1> entities{mine};
	const std::array<Name, 1> components{Name("ownership_test.Hidden")};

	REQUIRE(server.Receive(BuildSubmission(scratch, 1, entities, components)));
	REQUIRE(server.Apply() == ApplyStatus::Ok);

	CHECK(server.World.Get<Hidden>(mine)->Value == 7);
	CHECK(server.Authority_.Stats().Unowned == 1);
}

TEST_CASE("a submission older than the last one is refused", "[replication][ownership]") {
	// Not merged, because a submission is the client's whole answer for what it
	// owns: last tick's position arriving after this tick's would drag the
	// entity backwards on every machine watching it.
	Server server;

	const Entity mine = server.World.Create();
	server.World.Set<Spot>(mine, Spot{0.0f});
	server.Owns(mine);

	const std::array<Entity, 1> entities{mine};
	const std::array<float, 1> late{5.0f};
	const std::array<float, 1> later{9.0f};

	REQUIRE(server.Receive(SpotsFor(entities, later, 10)));
	CHECK_FALSE(server.Receive(SpotsFor(entities, late, 9)));

	// And equal is refused too: a duplicate says nothing new, and a second
	// answer for one tick is a client contradicting itself.
	CHECK_FALSE(server.Receive(SpotsFor(entities, late, 10)));

	REQUIRE(server.Apply() == ApplyStatus::Ok);
	CHECK(server.World.Get<Spot>(mine)->X == 9.0f);
}

TEST_CASE("a submission with nothing in it is refused", "[replication][ownership]") {
	Server server;
	server.Owns(server.World.Create());

	Delta empty;
	empty.Tick = 1;
	empty.Final = true;

	CHECK_FALSE(server.Receive(empty));
}

TEST_CASE("a client cannot tell the server what exists", "[replication][ownership]") {
	// **The asymmetry that makes this safe to have at all.** A delta is a client
	// saying where the thing it owns is; a snapshot or a structure message is a
	// client saying what the world contains, which is the one thing an authority
	// may never be told. Opening the delta path did not open those.
	Server server;
	server.Owns(server.World.Create());

	engine::replication::Structure structure;
	structure.Tick = 1;
	structure.Created.push_back(Entity{99});

	engine::core::ByteWriter writer;
	WriteMessage(writer, structure);
	const std::vector<std::byte> message(writer.Bytes().begin(), writer.Bytes().end());

	CHECK_FALSE(server.Authority_.Receive(server.Handle, message));
}
