// What a script can ask about who carries a tag.
//
// **The failure this closes is a second list.** `Instance:AddTag` has existed
// since v0.9 and answers one direction only, so a scene that wanted "every
// door" kept its own table of them beside the tags - and that table is wrong
// the first time a door is destroyed, in a way nothing reports.
//
// What is pinned here is that there is **one** mechanism: a tag set from the
// service is the tag the instance reports and the other way round, both
// spellings reaching `scene::Tagging`. Plus the two answers this surface owes
// and Roblox does not state - a deterministic order for `GetTagged`, and an
// empty list rather than an error for a tag nothing has added yet.

#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Tagging.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scripthost.collectionservice")
TEST_DEPENDS("engine.scene.tagging")

using engine::ecs::Store;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::script::Runtime;

namespace {
	// **Registered before the store exists**, for `MeshCatalogue`'s reason: a
	// resource id minted before the explicit registration lands takes the
	// compiler's spelling of the type and aborts when the real one arrives.
	Store Fresh(const char *name) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		return Store(name);
	}

	void MustRun(Runtime &runtime, const char *source) {
		INFO(source);
		const bool ok = runtime.Run(source);
		INFO(runtime.LastError());
		REQUIRE(ok);
	}
}

TEST_CASE("the service and the instance tag one thing", "[scripting][collection]") {
	// **The whole point of the service being a view rather than a store.** Two
	// mechanisms would agree until the first time one of them was fixed, and a
	// scene mixing the spellings - which every real one does, because the
	// instance form is shorter when the instance is in hand - would see half
	// its doors.
	Store store = Fresh("collection_one_mechanism");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		local part = Instance.new('Part')

		assert(CollectionService:AddTag(part, 'door'), 'a part can carry a tag')
		assert(part:HasTag('door'), 'the instance sees what the service set')

		part:AddTag('hinge')
		assert(CollectionService:HasTag(part, 'hinge'), 'and the service sees what the instance set')
		assert(#CollectionService:GetTagged('hinge') == 1, 'from either side it is one tag')

		-- Sorted by text, so the answer does not depend on which was added
		-- first - 'hinge' was registered second and sorts second regardless.
		local tags = CollectionService:GetTags(part)
		assert(#tags == 2, 'both tags, got ' .. #tags)
		assert(tags[1] == 'door' and tags[2] == 'hinge', 'sorted by name')

		-- **`Tags` is a `BasePart` component**, so anything else answers false
		-- rather than raising. That is `Instance:AddTag`'s answer too, and the
		-- two must not differ: which classes can be tagged is `scene`'s
		-- decision and this service does not get a vote.
		local folder = Instance.new('Instance')
		assert(CollectionService:AddTag(folder, 'door') == false, 'a container carries no tags')
		assert(folder:AddTag('door') == false, 'and says so from the other side too')
		assert(#CollectionService:GetTags(folder) == 0, 'so it lists none')
	)");
}

TEST_CASE("GetTagged answers in world order", "[scripting][collection]") {
	// **The ordering rule, and the case that discriminates it.** `Anchored` is
	// a structural property - an anchored part carries no `RigidBody`, so it
	// sits in a different archetype - and the parts here alternate, so the
	// order `Store::Each` hands rows over is by table and interleaves. A scene
	// laying its doors out from this list would arrange itself differently the
	// moment somebody anchored one, which is the non-determinism sorting is
	// here to prevent.
	//
	// They are also tagged in reverse, so "the order they were tagged" is
	// ruled out as well.
	Store store = Fresh("collection_order");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		local made = {}
		for index = 1, 6 do
			local part = Instance.new('Part')
			part.Name = 'p' .. index
			part.Anchored = index % 2 == 0
			part.Parent = workspace
			made[index] = part
		end

		for index = 4, 1, -1 do
			assert(CollectionService:AddTag(made[index], 'door'))
		end

		local doors = CollectionService:GetTagged('door')
		assert(#doors == 4, 'exactly the tagged ones, got ' .. #doors)
		for index = 1, 4 do
			assert(doors[index] == made[index], 'expected p' .. index .. ', got ' .. doors[index].Name)
		end

		-- The two that were never tagged are not in it, and the world holding
		-- them is not what decides.
		assert(not made[5]:HasTag('door') and not made[6]:HasTag('door'))
	)");
}

TEST_CASE("a tag nothing carries is an empty list", "[scripting][collection]") {
	// **Empty rather than an error, which departs from the ECS surface's rule
	// that a query naming an undeclared component raises.** A component is
	// declared by C++ and a typo can never become valid; a tag is created by
	// whichever `AddTag` names it first, so "nobody has added this yet" is an
	// ordinary state a script polling on `Heartbeat` sits in until another
	// script has run.
	//
	// And asking must not register it: a service that took the question as a
	// declaration would spend one of the world's thirty-two bits per typo.
	Store store = Fresh("collection_unknown");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		assert(#CollectionService:GetAllTags() == 0, 'a fresh world has registered nothing')

		local part = Instance.new('Part')
		assert(#CollectionService:GetTagged('nobody') == 0, 'nothing carries it')
		assert(CollectionService:HasTag(part, 'nobody') == false, 'and this does not either')
		assert(#CollectionService:GetTags(part) == 0, 'an untagged part lists nothing')

		assert(#CollectionService:GetAllTags() == 0, 'and asking registered no tag')
	)");

	// The C++ side of the same claim: asking did not even give the world a tag
	// table, which `TagsOf` would have done.
	CHECK(store.Resource<engine::scene::TagTable>() == nullptr);
}

TEST_CASE("a removed tag leaves the list and stays in the world", "[scripting][collection]") {
	// **Two answers that look inconsistent and are not.** `GetTagged` drops the
	// instance immediately, because that is a mask bit. `GetAllTags` keeps the
	// name forever, because `scene::RemoveTag` never frees a bit - freeing one
	// would renumber nothing while changing what a mask already stored on
	// another row means, which is the alias failure `Tagging.hpp` exists to
	// prevent.
	Store store = Fresh("collection_remove");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		local kept = Instance.new('Part')
		local dropped = Instance.new('Part')

		-- Registered in this order so the sort has something to do: 'zebra'
		-- first, 'apple' second, and `GetAllTags` must still answer apple first.
		assert(CollectionService:AddTag(kept, 'zebra'))
		assert(CollectionService:AddTag(dropped, 'zebra'))
		assert(CollectionService:AddTag(kept, 'apple'))

		assert(CollectionService:RemoveTag(dropped, 'zebra'))
		assert(dropped:HasTag('zebra') == false, 'the instance agrees')

		local zebras = CollectionService:GetTagged('zebra')
		assert(#zebras == 1 and zebras[1] == kept, 'only the one still carrying it')

		local all = CollectionService:GetAllTags()
		assert(#all == 2, 'the name outlives the last carrier, got ' .. #all)
		assert(all[1] == 'apple' and all[2] == 'zebra', 'sorted by text, not by registration order')
	)");
}

TEST_CASE("a destroyed instance leaves the tag list", "[scripting][collection]") {
	// **The bug the scene-side list had.** A game holding its own table of
	// doors keeps a dead handle in it, and the failure surfaces later as a
	// property read on nothing. There is nothing to go stale here because the
	// answer is the rows themselves.
	Store store = Fresh("collection_destroy");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		local doomed = Instance.new('Part')
		local survivor = Instance.new('Part')
		assert(CollectionService:AddTag(doomed, 'door'))
		assert(CollectionService:AddTag(survivor, 'door'))
		assert(#CollectionService:GetTagged('door') == 2)

		doomed:Destroy()

		local doors = CollectionService:GetTagged('door')
		assert(#doors == 1, 'the destroyed one is gone, got ' .. #doors)
		assert(doors[1] == survivor, 'and it is the other one')
	)");
}
