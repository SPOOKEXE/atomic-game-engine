// Tags: a name outside the process, a bit inside it.
//
// The two properties worth pinning are the ones that fail silently. A bit that
// gets reused means one tag's objects turning up in another's pass, which
// nobody notices until they see the wrong thing reflected in a mirror; and a
// mask restored without its table means a filter that matches nothing, with no
// error anywhere.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Tagging.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.scene.tagging")

using engine::core::Name;
using engine::ecs::Classes;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::scene::AddTag;
using engine::scene::HasTag;
using engine::scene::MakePart;
using engine::scene::MatchesTags;
using engine::scene::PartDesc;
using engine::scene::RemoveTag;
using engine::scene::SurfaceCamera;
using engine::scene::Tags;
using engine::scene::TagsOf;
using engine::scene::TagTable;

namespace {
	Entity MadePart(Store &store) {
		engine::scene::RegisterSceneClasses();
		return MakePart(store, PartDesc{});
	}
}

TEST_CASE("a registered tag gets a bit and keeps it", "[scene][tagging]") {
	TagTable table;

	const uint32_t first = table.Register(Name("tagging_test.Mirror"));
	const uint32_t second = table.Register(Name("tagging_test.Glass"));

	CHECK(first == 1u);
	CHECK(second == 2u);

	// Registering again is a lookup, not a second bit.
	CHECK(table.Register(Name("tagging_test.Mirror")) == first);
	CHECK(table.Find(Name("tagging_test.Glass")) == second);
	CHECK(table.Names.size() == 2);
}

TEST_CASE("an unregistered name has no bit", "[scene][tagging]") {
	TagTable table;
	table.Register(Name("tagging_test.Mirror"));

	// Zero rather than a guess, so `MatchesTags` reads it as "no filter"
	// rather than as "the first tag".
	CHECK(table.Find(Name("tagging_test.Never")) == 0);
	CHECK(table.Find(Name()) == 0);
	CHECK(table.Register(Name()) == 0);
}

TEST_CASE("the thirty-third tag is refused rather than aliased", "[scene][tagging]") {
	TagTable table;
	for (size_t index = 0; index < TagTable::MAXIMUM; index++) {
		REQUIRE(table.Register(Name("tagging_test.Full" + std::to_string(index))) != 0);
	}

	// **The failure this ceiling exists to prevent is not "too many tags".** It
	// is one tag quietly sharing another's bit, which puts one group's objects
	// in another group's pass - invisible until somebody notices the wrong
	// thing reflected.
	CHECK(table.Register(Name("tagging_test.OneTooMany")) == 0);
	CHECK(table.Names.size() == TagTable::MAXIMUM);

	// And the last real one still works, so the ceiling is thirty-two rather
	// than thirty-one.
	CHECK(table.Find(Name("tagging_test.Full31")) == (1u << 31));
}

TEST_CASE("a mask becomes names again", "[scene][tagging]") {
	TagTable table;
	table.Register(Name("tagging_test.A"));
	table.Register(Name("tagging_test.B"));
	table.Register(Name("tagging_test.C"));

	const auto named = table.Describe(0b101);
	REQUIRE(named.size() == 2);
	CHECK(named[0] == Name("tagging_test.A"));
	CHECK(named[1] == Name("tagging_test.C"));

	CHECK(table.Describe(0).empty());

	// A bit past what the table names is ignored rather than read as a name
	// that is not there.
	CHECK(table.Describe(0xFFFFFFFF).size() == 3);
}

TEST_CASE("a part can be tagged and untagged", "[scene][tagging]") {
	Store store("tagging_test");
	engine::scene::RegisterSceneComponents();
	const Entity part = MadePart(store);
	REQUIRE(part != engine::ecs::NULL_ENTITY);

	CHECK_FALSE(HasTag(store, part, Name("tagging_test.Reflective")));

	REQUIRE(AddTag(store, part, Name("tagging_test.Reflective")));
	CHECK(HasTag(store, part, Name("tagging_test.Reflective")));

	// Every `BasePart` has a `Tags`, which is what makes this work without the
	// caller adding a component first.
	REQUIRE(store.Get<Tags>(part) != nullptr);
	CHECK(store.Get<Tags>(part)->Mask == 1u);

	REQUIRE(RemoveTag(store, part, Name("tagging_test.Reflective")));
	CHECK_FALSE(HasTag(store, part, Name("tagging_test.Reflective")));

	// **The name stays in the table.** Freeing the bit would renumber nothing
	// and leave every other row's mask meaning something new.
	CHECK(TagsOf(store).Find(Name("tagging_test.Reflective")) == 1u);
}

TEST_CASE("removing a tag the world never heard of registers nothing", "[scene][tagging]") {
	Store store("tagging_test");
	engine::scene::RegisterSceneComponents();
	const Entity part = MadePart(store);

	REQUIRE(RemoveTag(store, part, Name("tagging_test.Unknown")));
	CHECK(TagsOf(store).Names.empty());
}

TEST_CASE("tagging something with no tags component fails rather than aborting", "[scene][tagging]") {
	Store store("tagging_test");
	engine::scene::RegisterSceneComponents();

	const Entity bare = store.Create();
	CHECK_FALSE(AddTag(store, bare, Name("tagging_test.Anything")));
	CHECK_FALSE(RemoveTag(store, bare, Name("tagging_test.Anything")));
	CHECK_FALSE(HasTag(store, bare, Name("tagging_test.Anything")));
}

TEST_CASE("an empty filter matches everything", "[scene][tagging]") {
	// What makes tag filtering free for every scene that does not use it.
	CHECK(MatchesTags(0, 0));
	CHECK(MatchesTags(0b1010, 0));

	CHECK(MatchesTags(0b1010, 0b0010));
	CHECK(MatchesTags(0b1010, 0b1111));
	CHECK_FALSE(MatchesTags(0b1010, 0b0101));
	CHECK_FALSE(MatchesTags(0, 0b0001));
}

TEST_CASE("several tags live on one part at once", "[scene][tagging]") {
	Store store("tagging_test");
	engine::scene::RegisterSceneComponents();
	const Entity part = MadePart(store);

	REQUIRE(AddTag(store, part, Name("tagging_test.Reflective")));
	REQUIRE(AddTag(store, part, Name("tagging_test.Interactive")));

	CHECK(HasTag(store, part, Name("tagging_test.Reflective")));
	CHECK(HasTag(store, part, Name("tagging_test.Interactive")));
	CHECK(store.Get<Tags>(part)->Mask == 0b11);

	RemoveTag(store, part, Name("tagging_test.Reflective"));
	CHECK_FALSE(HasTag(store, part, Name("tagging_test.Reflective")));
	CHECK(HasTag(store, part, Name("tagging_test.Interactive")));
}

TEST_CASE("a surface camera's filter is authored by name and stored as a bit", "[scene][tagging]") {
	Store store("tag_filter_test");
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	const Entity camera = store.CreateInstance(Classes::Find(Name("SurfaceCamera")), "Redirect");
	REQUIRE(camera != engine::ecs::NULL_ENTITY);

	// A camera with no filter draws the world, which is what every mirror is.
	CHECK(store.Get<SurfaceCamera>(camera)->TagFilter == 0);

	// **The declared type is checked, not just the round-trip.** Writing raw
	// bytes through `SetProperty` succeeds whatever the descriptor claims the
	// type is, so a property declared `String` over a `core::Name` payload
	// passes this test and fails at the first script that assigns to it. The
	// type *is* the contract with the binding, so it is what to assert.
	bool declared = false;
	for (const engine::ecs::PropertyDescriptor &property : store.PropertiesOf(camera)) {
		if (property.Name == Name("TagFilter")) {
			declared = true;
			CHECK(property.Type == engine::ecs::PropertyType::Name);
		}
	}
	CHECK(declared);

	const Name reflective("tag_filter_test.Reflective");
	REQUIRE(store.SetProperty(camera, Name("TagFilter"), &reflective, sizeof(Name)));

	// **The property is a name and the storage is a bit.** The registration
	// happens where the name is written, so the draw loop never sees a string.
	const uint32_t bit = TagsOf(store).Find(reflective);
	CHECK(bit != 0);
	CHECK(store.Get<SurfaceCamera>(camera)->TagFilter == bit);

	Name read;
	REQUIRE(store.GetProperty(camera, Name("TagFilter"), &read, sizeof(Name)));
	CHECK(read == reflective);

	// Cleared back to "everything", because that is what an empty filter means
	// and what clearing the property has to do.
	const Name none;
	REQUIRE(store.SetProperty(camera, Name("TagFilter"), &none, sizeof(Name)));
	CHECK(store.Get<SurfaceCamera>(camera)->TagFilter == 0);
}

TEST_CASE("a filtered surface draws its group and an unfiltered one the world", "[scene][tagging]") {
	// The predicate the renderer applies per instance. It is checked here
	// rather than in `render` for the reason every other piece of draw
	// arithmetic is: a renderer is the one module a headless suite cannot
	// exercise, so the part that is pure logic over a `shared` type belongs
	// where a test can reach it.
	constexpr uint32_t REFLECTIVE = 0b01;
	constexpr uint32_t HIDDEN = 0b10;

	// An unfiltered camera - every mirror - draws everything.
	CHECK(MatchesTags(0, 0));
	CHECK(MatchesTags(REFLECTIVE, 0));
	CHECK(MatchesTags(HIDDEN, 0));

	// A filtered one draws its group and nothing else, which is what makes a
	// second pipeline *redirected* rather than a second copy of the scene.
	CHECK(MatchesTags(REFLECTIVE, REFLECTIVE));
	CHECK(MatchesTags(REFLECTIVE | HIDDEN, REFLECTIVE));
	CHECK_FALSE(MatchesTags(HIDDEN, REFLECTIVE));
	CHECK_FALSE(MatchesTags(0, REFLECTIVE));
}

TEST_CASE("a filter can name several tags at once", "[scene][tagging]") {
	Store store("tag_set_test");
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	const Entity camera = store.CreateInstance(Classes::Find(Name("SurfaceCamera")), "Redirect");

	// **A list in a string, and that is the whole syntax.** A mask holds
	// thirty-two and a property holds one value, so the alternative was a list
	// type in `ecs::PropertyType` that exactly one property would use.
	const Name several("tag_set_test.Imported, tag_set_test.Reflective");
	REQUIRE(store.SetProperty(camera, Name("TagFilter"), &several, sizeof(Name)));

	const uint32_t imported = TagsOf(store).Find(Name("tag_set_test.Imported"));
	const uint32_t reflective = TagsOf(store).Find(Name("tag_set_test.Reflective"));
	REQUIRE(imported != 0);
	REQUIRE(reflective != 0);
	CHECK(store.Get<SurfaceCamera>(camera)->TagFilter == (imported | reflective));

	// An instance carrying either is drawn; one carrying neither is not.
	CHECK(MatchesTags(imported, store.Get<SurfaceCamera>(camera)->TagFilter));
	CHECK(MatchesTags(reflective, store.Get<SurfaceCamera>(camera)->TagFilter));
	CHECK_FALSE(MatchesTags(0, store.Get<SurfaceCamera>(camera)->TagFilter));

	// It round-trips, in bit order and joined the same way it was written.
	Name read;
	REQUIRE(store.GetProperty(camera, Name("TagFilter"), &read, sizeof(Name)));
	CHECK(read == several);
}

TEST_CASE("spacing and empty entries in a filter are forgiving", "[scene][tagging]") {
	Store store("tag_spacing_test");
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	const Entity camera = store.CreateInstance(Classes::Find(Name("SurfaceCamera")), "Redirect");

	// The separator reads the way somebody would type it, and a stray comma
	// does not register a blank tag.
	const Name messy("  tag_spacing_test.A ,, tag_spacing_test.B ,  ");
	REQUIRE(store.SetProperty(camera, Name("TagFilter"), &messy, sizeof(Name)));

	CHECK(TagsOf(store).Names.size() == 2);
	CHECK(TagsOf(store).Find(Name("tag_spacing_test.A")) != 0);
	CHECK(TagsOf(store).Find(Name("tag_spacing_test.B")) != 0);
}

TEST_CASE("a filter that overflows the table applies none of itself", "[scene][tagging]") {
	Store store("tag_overflow_test");
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	// One slot left.
	for (size_t index = 0; index + 1 < TagTable::MAXIMUM; index++) {
		REQUIRE(TagsOf(store).Register(Name("tag_overflow_test.Fill" + std::to_string(index))) != 0);
	}

	const Entity camera = store.CreateInstance(Classes::Find(Name("SurfaceCamera")), "Redirect");
	const Name two("tag_overflow_test.First, tag_overflow_test.Second");

	// **Refused whole rather than applied part way.** A camera left with half
	// its filter is a redirected pass drawing some of its group, which is
	// harder to notice than one drawing none of it.
	CHECK_FALSE(store.SetProperty(camera, Name("TagFilter"), &two, sizeof(Name)));
	CHECK(store.Get<SurfaceCamera>(camera)->TagFilter == 0);
}
