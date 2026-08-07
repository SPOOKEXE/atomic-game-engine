// What a script can ask about the content this world holds.
//
// **The failure this closes was invisible in exactly the way that matters.** A
// scene named a mesh with a string literal; an unregistered name draws a cube,
// and so does a mesh that has not streamed in yet — so a demo written against a
// store nobody else had looked identical to one that was still loading.
// `MeshGrid.luau` carried nine of those literals.
//
// What is pinned here is that the answer comes from the world's own catalogues
// and never from a guess: an empty list when nothing has been registered, the
// names when it has, and sorted order, which is what lets a scene lay content
// out the same way on every run.

#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/PublishedCatalogue.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/TextureCatalogue.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.script.contentservice")
TEST_DEPENDS("engine.scene.texturecatalogue")
TEST_DEPENDS("engine.scene.meshcatalogue")

using engine::core::Name;
using engine::ecs::Store;
using engine::scene::FlipbookFacts;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::script::Runtime;

namespace {
	// **Registered before the store exists**, for `MeshCatalogue`'s reason: a
	// resource id minted before the explicit registration lands takes the
	// compiler's spelling of the type and aborts the process when the real
	// registration arrives.
	Store Fresh(const char *name) {
		(void)engine::scene::PartClass();
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

TEST_CASE("a world with no content answers an empty list", "[scripting][content]") {
	// **The honest answer on a headless server and before a fetch lands**, and
	// they are the same answer on purpose: neither has anything to draw. A
	// service that read a manifest instead would name assets this process may
	// never fetch and the renderer cannot draw.
	Store store = Fresh("content_empty");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		local meshes = ContentService:GetMeshes()
		assert(type(meshes) == "table", "GetMeshes returns a table")
		assert(#meshes == 0, "a fresh world holds no meshes")
		assert(#ContentService:GetTextures() == 0, "and no textures")
	)");
}

TEST_CASE("registered meshes are reported by name", "[scripting][content]") {
	Store store = Fresh("content_meshes");

	REQUIRE(engine::scene::RecordMesh(store, Name("props/fox.amesh"), 1200));
	REQUIRE(engine::scene::RecordMesh(store, Name("props/dragon.amesh"), 98000));

	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		local meshes = ContentService:GetMeshes()
		assert(#meshes == 2, "both meshes")

		-- **Sorted, which is what makes a layout reproducible.** The catalogue
		-- is a hash map, so a scene that placed parts in iteration order would
		-- arrange itself differently on every run.
		assert(meshes[1] == "props/dragon.amesh", "dragon sorts first, got " .. meshes[1])
		assert(meshes[2] == "props/fox.amesh", "fox sorts second, got " .. meshes[2])

		assert(ContentService:GetTriangleCount("props/dragon.amesh") == 98000)
		assert(ContentService:GetTriangleCount("props/fox.amesh") == 1200)

		-- A mesh this world has never been told about is zero rather than an
		-- error: that is `TrianglesOf`'s own answer and a script asking about a
		-- name it read from this same list cannot hit it.
		assert(ContentService:GetTriangleCount("nothing/here.amesh") == 0)
	)");
}

TEST_CASE("a texture's flipbook facts reach a script", "[scripting][content]") {
	// **The number a scene would otherwise hardcode.** A GIF states a delay per
	// frame and how many of its grid's cells hold one; a script that wrote `24`
	// in is wrong the moment somebody re-exports with a frame added — which is
	// exactly what happened with `fox_dance.gif`, whose 48 frames were being
	// played as 24.
	Store store = Fresh("content_flipbook");

	REQUIRE(
		engine::scene::RecordTexture(
			store, Name("effects/fox_dance.atex"), FlipbookFacts{.Side = 8, .Frames = 48, .FrameRate = 24.0f}
		)
	);

	// A still image, which is the case that must read as "nothing to play".
	REQUIRE(engine::scene::RecordTexture(store, Name("props/bark.atex"), FlipbookFacts{}));

	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		local textures = ContentService:GetTextures()
		assert(#textures == 2, "both textures are listed, still or not")

		local fox = ContentService:GetFlipbook("effects/fox_dance.atex")
		assert(fox ~= nil, "the fox is a flipbook")
		assert(fox.Side == 8, "an 8x8 grid")
		assert(fox.Frames == 48, "forty-eight of its sixty-four cells hold a frame")
		assert(math.abs(fox.FrameRate - 24) < 0.001, "24fps")

		-- **`nil` for a still image and for an unknown name alike.** Neither is
		-- something to play, and a consumer that had to tell them apart would
		-- be asking a question with no use.
		assert(ContentService:GetFlipbook("props/bark.atex") == nil, "a still is not a flipbook")
		assert(ContentService:GetFlipbook("nothing/here.atex") == nil, "nor is a stranger")
	)");
}

TEST_CASE("a mesh registered after the runtime started is still seen", "[scripting][content]") {
	// **The case the demos actually live in.** Delivery spans frames, so the
	// catalogue is empty when a scene chunk runs and fills as bundles verify —
	// which is why `MeshGrid.luau` polls on `Heartbeat` instead of reading once.
	// A service that cached its answer would make that poll useless.
	Store store = Fresh("content_late");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		assert(#ContentService:GetMeshes() == 0, "nothing yet")
	)");

	REQUIRE(engine::scene::RecordMesh(store, Name("props/late.amesh"), 42));

	MustRun(*runtime, R"(
		local meshes = ContentService:GetMeshes()
		assert(#meshes == 1, "the one that arrived")
		assert(meshes[1] == "props/late.amesh")
	)");
}

TEST_CASE("a script can ask what the store published", "[scripting][content]") {
	// **The two lists answer different questions and the difference is the
	// whole reason this one exists.** `GetMeshes` is what arrived; this is what
	// there is. They were the same set until v0.10, because content was fetched
	// by kind — so a scene reading the first saw the store whether it had asked
	// for any of it or not. Nothing is fetched by kind now, and the consequence
	// is that a scene reading only the first can never discover anything: it
	// sees what it has already named, which on a fresh place is nothing.
	Store store = Fresh("content_published");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	// A world nobody has told answers empty rather than failing, which is what a
	// headless server and a client with no `--cdn` both are.
	MustRun(*runtime, R"(
		assert(#ContentService:GetPublishedMeshes() == 0, "nothing offered yet")
	)");

	REQUIRE(
		engine::scene::RecordPublishedMeshes(
			store, {Name("characters/xiao.amesh"), Name("characters/barbara.amesh")}
		) == 2
	);

	MustRun(*runtime, R"(
		local published = ContentService:GetPublishedMeshes()
		assert(#published == 2, "both offered")

		-- Sorted, like every list this service returns, so a scene laying content
		-- out arranges itself the same way on every run.
		assert(published[1] == "characters/barbara.amesh", published[1])
		assert(published[2] == "characters/xiao.amesh", published[2])

		-- And still nothing has *arrived*. This is the distinction: naming one of
		-- these is what fetches it, so before a scene does, the loaded catalogue
		-- is empty.
		assert(#ContentService:GetMeshes() == 0, "published is not loaded")
	)");
}

TEST_CASE("a republish replaces the offered list", "[scripting][content]") {
	// **Replaces rather than appends.** A manifest is a whole answer, and a name
	// left behind from a previous one is a mesh a scene would set, fetch and
	// never resolve — which draws the fallback cube with nothing to say why.
	Store store = Fresh("content_republish");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	REQUIRE(engine::scene::RecordPublishedMeshes(store, {Name("gone.amesh")}) == 1);
	REQUIRE(engine::scene::RecordPublishedMeshes(store, {Name("kept.amesh")}) == 1);

	MustRun(*runtime, R"(
		local published = ContentService:GetPublishedMeshes()
		assert(#published == 1, "one, not two")
		assert(published[1] == "kept.amesh", published[1])
	)");
}

TEST_CASE("an invalid published name is dropped", "[scripting][content]") {
	// A name a scene cannot use is a row it would try to fetch and never
	// resolve, so it is refused where the list is recorded rather than surfacing
	// as an empty string in a table somebody iterates.
	Store store = Fresh("content_invalid");
	CHECK(engine::scene::RecordPublishedMeshes(store, {Name(), Name("real.amesh"), Name()}) == 1);

	std::vector<Name> read;
	CHECK(engine::scene::PublishedMeshes(store, read) == 1);
	REQUIRE(read.size() == 1);
	CHECK(read.front() == Name("real.amesh"));
}
