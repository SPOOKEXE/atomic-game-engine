// The attachment resolve pass, and the signal it owes.
//
// **Written because the pass had no test of its own and one half of it did not
// work.** `ResolveAttachments` wrote `WorldFrame` straight through the
// reference `Store::Each` hands out - a direct memory write, which the store
// does not report - so `Attachment.WorldCFrame` and `WorldPosition` could never
// fire `.Changed`. The value read correctly the whole time, which is why
// nothing noticed: a script polling saw the truth and a script waiting on the
// signal waited for ever.
//
// The other half is why the fix is a compare and not a `MarkAllChanged`.
// Reporting every attachment every frame advances the world's change counter
// for ever, and `physics`'s static broadphase and `gui`'s compile are both
// gates on that counter standing still - `Store::GetUnobserved` carries the
// argument.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_SUITE_ID("engine.scene.attachments")
TEST_DEPENDS("engine.ecs.signals")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::Attachment;
using engine::scene::ResolveAttachment;
using engine::scene::ResolveAttachments;
using engine::scene::Transform;

namespace {
	Store Fresh(const char *name) {
		engine::scene::RegisterSceneComponents();
		engine::scene::RegisterSceneClasses();
		return Store(name);
	}

	// A part at `where` with an attachment `offset` from it, and the
	// attachment's entity.
	Entity Rig(Store &store, const Vector3 &where, const Vector3 &offset, Entity &part) {
		part = store.CreateInstance(engine::scene::PartClass(), "Post");
		REQUIRE(part != NULL_ENTITY);
		store.GetMutable<Transform>(part)->Frame = CFrame(where);

		const Entity point = store.CreateInstance(engine::scene::AttachmentClass(), "Top");
		REQUIRE(point != NULL_ENTITY);
		REQUIRE(store.SetParent(point, part));
		store.GetMutable<Attachment>(point)->Frame = CFrame(offset);

		return point;
	}
}

TEST_CASE("an attachment resolves against its parent's frame", "[scene][attachments]") {
	Store store = Fresh("attachments.resolve");

	Entity part = NULL_ENTITY;
	const Entity point = Rig(store, Vector3{10.0f, 0.0f, -4.0f}, Vector3{0.0f, 6.0f, 0.0f}, part);

	CHECK(ResolveAttachments(store) == 1);

	const Attachment *resolved = store.Get<Attachment>(point);
	REQUIRE(resolved != nullptr);
	CHECK(resolved->WorldFrame.Position.X == Approx(10.0f));
	CHECK(resolved->WorldFrame.Position.Y == Approx(6.0f));
	CHECK(resolved->WorldFrame.Position.Z == Approx(-4.0f));

	// The on-the-spot reader and the pass have to agree, or a script reading
	// `WorldCFrame` and an emitter reading the cache would place the same point
	// in two places.
	CHECK(ResolveAttachment(store, point).Position.Y == Approx(6.0f));
}

TEST_CASE("an attachment on nothing keeps its local frame", "[scene][attachments]") {
	Store store = Fresh("attachments.orphan");

	const Entity point = store.CreateInstance(engine::scene::AttachmentClass(), "Loose");
	store.GetMutable<Attachment>(point)->Frame = CFrame(Vector3{1.0f, 2.0f, 3.0f});

	CHECK(ResolveAttachments(store) == 1);

	// The useful state rather than an error - it is what makes an attachment
	// usable as a bare point in space, which is what a beam between two world
	// positions needs.
	CHECK(store.Get<Attachment>(point)->WorldFrame.Position.Y == Approx(2.0f));
}

// --- the signal ---------------------------------------------------------------

TEST_CASE("resolving reports the attachments that moved", "[scene][attachments]") {
	Store store = Fresh("attachments.signal");

	Entity part = NULL_ENTITY;
	const Entity point = Rig(store, Vector3{0.0f, 0.0f, 0.0f}, Vector3{0.0f, 6.0f, 0.0f}, part);

	std::vector<Entity> heard;
	store.OnChanged<Attachment>([&heard](Store &, Entity entity, const Attachment &) {
		heard.push_back(entity);
	});

	// The first pass moves it off the identity, so it reports.
	REQUIRE(ResolveAttachments(store) == 1);
	REQUIRE(store.FlushSignals() == 1);
	REQUIRE(heard == std::vector<Entity>{point});

	// **`ClearChanges` is what a tick does at a phase boundary**, and a test
	// that skipped it would fire again off the previous pass's bits and prove
	// nothing about the second one.
	store.ClearChanges();
	heard.clear();

	// **And the part moving reports again**, which is the case the property
	// surface cannot express on its own: nothing wrote the attachment, and its
	// world position changed. `docs/DEFERRED.md` D00043 is this.
	store.GetMutable<Transform>(part)->Frame = CFrame(Vector3{20.0f, 0.0f, 0.0f});

	REQUIRE(ResolveAttachments(store) == 1);
	REQUIRE(store.FlushSignals() == 1);
	CHECK(heard == std::vector<Entity>{point});
	CHECK(store.Get<Attachment>(point)->WorldFrame.Position.X == Approx(20.0f));
}

TEST_CASE("resolving a world that did not move reports nothing", "[scene][attachments]") {
	Store store = Fresh("attachments.quiet");

	Entity part = NULL_ENTITY;
	(void)Rig(store, Vector3{3.0f, 0.0f, 0.0f}, Vector3{0.0f, 1.0f, 0.0f}, part);

	std::vector<Entity> heard;
	store.OnChanged<Attachment>([&heard](Store &, Entity entity, const Attachment &) {
		heard.push_back(entity);
	});

	REQUIRE(ResolveAttachments(store) == 1);
	REQUIRE(store.FlushSignals() == 1);

	// The phase boundary, as above: the bits from the pass that *did* move it
	// are cleared, so what follows measures only the second pass.
	store.ClearChanges();
	heard.clear();

	// **The half that makes the compare necessary rather than tidy.** This pass
	// runs every frame in every phase it is registered in, and a version of it
	// that reported unconditionally would advance the change counter for ever -
	// falsifying `physics`'s static broadphase gate and `gui`'s compile gate,
	// both of which are built on that counter standing still when nothing was
	// authored.
	const uint64_t before = store.ChangeVersion();

	REQUIRE(ResolveAttachments(store) == 1);
	CHECK(store.FlushSignals() == 0);
	CHECK(heard.empty());
	CHECK(store.ChangeVersion() == before);
}
