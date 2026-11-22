// What a script can ask about the content this world holds.
//
// **The failure this closes was invisible in exactly the way that matters.** A
// scene named a mesh with a string literal; an unregistered name draws a cube,
// and so does a mesh that has not streamed in yet - so a demo written against a
// store nobody else had looked identical to one that was still loading.
// `MeshGrid.luau` carried nine of those literals.
//
// What is pinned here is that the answer comes from the world's own catalogues
// and never from a guess: an empty list when nothing has been registered, the
// names when it has, and sorted order, which is what lets a scene lay content
// out the same way on every run.

#include <engine/assets/Mesh.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/PublishedCatalogue.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/scene/TextureCatalogue.hpp>
#include <engine/script/MeshSource.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.scripthost.contentservice")
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
	// in is wrong the moment somebody re-exports with a frame added - which is
	// exactly what happened with `fox_dance.gif`, whose 48 frames were being
	// played as 24.
	Store store = Fresh("content_flipbook");

	REQUIRE(
		engine::scene::RecordTexture(
			store,
			Name("effects/fox_dance.atex"),
			FlipbookFacts{
				.Side = 8, .Frames = 48, .FrameRate = 24.0f, .FrameDurations = {}, .CumulativeEnds = {}
			}
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
	// catalogue is empty when a scene chunk runs and fills as bundles verify -
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
	// by kind - so a scene reading the first saw the store whether it had asked
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
	// never resolve - which draws the fallback cube with nothing to say why.
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

TEST_CASE("a script can read what a mesh is wearing", "[scripting][content]") {
	// **The handle an outfit swap needs.** A `MeshPart` naming no `TextureID`
	// shows whatever each submesh recorded at bake time, and those names live
	// inside the mesh file - so without this a script has no way to learn the
	// current sheet and no name to put back.
	Store store = Fresh("content_mesh_textures");

	const std::array<Name, 3> worn{Name("skins/body.atex"), Name("skins/eyes.atex"), Name("skins/body.atex")};
	REQUIRE(
		engine::scene::RecordMesh(
			store, Name("props/hero.amesh"), 4200, worn, {}, engine::core::Vector3{2.0f, 6.0f, 1.0f}
		)
	);

	// A submesh that names nothing is an ordinary thing - a model with one
	// untextured run - and it crosses as an empty string rather than as a hole,
	// so the index keeps meaning "submesh number".
	const std::array<Name, 2> partly{Name("skins/only.atex"), Name()};
	REQUIRE(engine::scene::RecordMesh(store, Name("props/half.amesh"), 12, partly));

	// A built-in names none at all.
	REQUIRE(engine::scene::RecordMesh(store, Name("engine.Cube"), 12));

	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		local size = ContentService:GetMeshSize("props/hero.amesh")
		assert(size.X == 2 and size.Y == 6 and size.Z == 1, "authored mesh bounds arrive")
		local worn = ContentService:GetMeshTextures("props/hero.amesh")
		assert(#worn == 3, "one entry per submesh, got " .. #worn)

		-- **Not sorted and not deduplicated**, unlike every other list this
		-- service hands out. Which run wears which is a fact, and a character
		-- with twenty submeshes sharing four sheets is four names repeated.
		assert(worn[1] == "skins/body.atex", "submesh order, got " .. worn[1])
		assert(worn[2] == "skins/eyes.atex", "submesh order, got " .. worn[2])
		assert(worn[3] == "skins/body.atex", "the duplicate survives, got " .. worn[3])

		-- An untextured run is an empty string in its own slot. Reported rather
		-- than skipped, because skipping it would shift every index after it.
		local half = ContentService:GetMeshTextures("props/half.amesh")
		assert(#half == 2, "the untextured run keeps its slot, got " .. #half)
		assert(half[1] == "skins/only.atex")
		assert(half[2] == "", "an untextured submesh is empty, got '" .. half[2] .. "'")

		-- A built-in wears nothing, and a mesh nothing has recorded answers the
		-- same way. Those two are one answer for `TrianglesOf`'s reason: this
		-- world cannot tell you, and a caller can act on neither.
		assert(#ContentService:GetMeshTextures("engine.Cube") == 0, "a built-in wears nothing")
		assert(#ContentService:GetMeshTextures("props/never.amesh") == 0, "an unknown mesh is empty")
		assert(#ContentService:GetMeshTextures("") == 0, "and so is no mesh at all")
	)");
}

namespace {
	// A three-vertex mesh skinned to a two-joint chain, through the mesh
	// format the way the content pump receives one.
	std::shared_ptr<const engine::scene::MeshSource> RiggedSource() {
		engine::assets::MeshData mesh;
		mesh.JointCount = 2;
		mesh.Vertices.resize(3);
		const float positions[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 2, 0}};
		for (size_t index = 0; index < 3; index++) {
			std::memcpy(mesh.Vertices[index].Position, positions[index], sizeof(positions[index]));
			mesh.Vertices[index].Normal[1] = 1;
			mesh.Vertices[index].Joints[0] = index == 2 ? 1 : 0;
			mesh.Vertices[index].Weights[0] = 65535;
		}
		mesh.Indices = {0, 1, 2};
		mesh.Submeshes.push_back({0, 3, {}, {}, {1, 1, 1, 1}});
		mesh.Skeleton = {
			{"Hips",
			 engine::assets::MeshJoint::NO_PARENT,
			 engine::core::CFrame({0, 1, 0}),
			 engine::core::CFrame({0, -1, 0})},
			{"Spine", 0, engine::core::CFrame({0, 0.5f, 0}), engine::core::CFrame({0, -1.5f, 0})},
		};
		mesh.ComputeBounds();
		engine::core::ByteWriter writer;
		REQUIRE(engine::assets::Mesh::Write(writer, mesh));
		engine::core::ByteReader reader(writer.Bytes());
		engine::assets::MeshData read;
		REQUIRE(engine::assets::Mesh::Read(reader, read));
		return engine::script::BuildMeshSource(read);
	}

	void Beat(Store &store, Runtime &runtime) {
		store.ClearChanges();
		store.AdvanceTick(store.Time().Delta);
		runtime.Heartbeat(store.Time().Delta);
		store.FlushSignals();
	}
}

TEST_CASE("ReadMesh returns buffers and a skeleton a script can rebuild a rig from", "[scripting][content]") {
	engine::scene::EditableMeshClass();
	Store store = Fresh("content_readmesh");
	REQUIRE(engine::scene::RecordMeshSource(store, Name("body.amesh"), RiggedSource()));
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		assert(ContentService:ReadMesh("missing.amesh") == nil, "an unreceived mesh reads nil")
		local read = ContentService:ReadMesh("body.amesh")
		assert(read.VertexCount == 3 and read.IndexCount == 3 and read.JointCount == 2)
		assert(buffer.len(read.Indices) == 12)
		assert(buffer.readf32(read.Vertices, read.Layout.Position.Offset + 2 * 12 + 4) == 2)
		assert(buffer.readu16(read.Vertices, read.Layout.Joints.Offset + 2 * 8) == 1)
		assert(#read.Submeshes == 1 and read.Submeshes[1].IndexCount == 3)
		assert(#read.Skeleton == 2)
		assert(read.Skeleton[1].Name == "Hips" and read.Skeleton[1].Parent == -1)
		assert(read.Skeleton[2].Name == "Spine" and read.Skeleton[2].Parent == 0)
		assert(math.abs(read.Skeleton[2].InverseBind.Position.Y + 1.5) < 1e-6)

		local rig = Instance.new("SkinnedMeshPart")
		rig.Name = "Rig"
		rig.Parent = workspace
		local bones = {}
		for index, joint in read.Skeleton do
			local bone = Instance.new("Bone")
			bone.Name = joint.Name
			bone.RestCFrame = joint.Rest
			bone.InverseBindCFrame = joint.InverseBind
			bone.Parent = if joint.Parent >= 0 then bones[joint.Parent + 1] else rig
			bones[index] = bone
		end
		assert(not pcall(rig.SetBones, rig, { bones[2], bones[1] }), "a child before its parent is refused")
		rig:SetBones(bones)
		assert(rig.JointCount == 2)

		local mesh = Instance.new("EditableMesh")
		mesh.Name = "Body"
		mesh.Parent = rig
		local ok = pcall(mesh.SetGeometryFromBuffer, mesh, { VertexCount = 3 }, read.Vertices, read.Indices)
		assert(not ok, "a layout without a Position is refused")
		ok = pcall(mesh.SetGeometryFromBuffer, mesh, { VertexCount = 99, Position = 0 }, read.Vertices, read.Indices)
		assert(not ok, "a field past the end of the buffer is refused")
		assert(mesh:SetGeometryFromBuffer(read.Layout, read.Vertices, read.Indices))
		rig.MeshId = mesh.ContentId
		mesh.Name = "BodyBuilt"
	)");
	Beat(store, *runtime);
	INFO(runtime->LastError());

	const auto rig = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Rig");
	REQUIRE(rig != engine::ecs::NULL_ENTITY);
	const auto mesh = store.FindFirstChild(rig, "BodyBuilt");
	REQUIRE(mesh != engine::ecs::NULL_ENTITY);
	const auto *built = store.Get<engine::scene::EditableMesh>(mesh);
	REQUIRE(built->Positions.size() == 3);
	CHECK(built->Positions[2] == engine::core::Vector3{0, 2, 0});
	REQUIRE(built->Joints.size() == 3);
	CHECK(built->Joints[2][0] == 1);
	CHECK(built->Weights[2][0] == 1.0f);
	CHECK(store.Get<engine::scene::Visual>(rig)->Mesh == engine::scene::EditableMeshContentName(store, mesh));
	const auto spine = store.FindFirstChild(store.FindFirstChild(rig, "Hips"), "Spine");
	REQUIRE(spine != engine::ecs::NULL_ENTITY);
	CHECK(store.Get<engine::scene::Bone>(spine)->Joint == 1);
	CHECK(store.Get<engine::scene::Bone>(spine)->ParentJoint == 0);
}

TEST_CASE("ReadMesh and typed array buffers work from JavaScript", "[scripting][content][js]") {
	engine::scene::EditableMeshClass();
	Store store = Fresh("content_readmesh_js");
	REQUIRE(engine::scene::RecordMeshSource(store, Name("body.amesh"), RiggedSource()));
	const auto runtime = MakeRuntime(store, Language::JavaScript);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		const read = ContentService.ReadMesh('body.amesh');
		if (read.VertexCount !== 3 || read.Skeleton[1].Name !== 'Spine' || read.Skeleton[1].Parent !== 0)
			throw new Error('unexpected readout');
		if (!(read.Vertices instanceof ArrayBuffer) || read.Indices.byteLength !== 12)
			throw new Error('buffers are ArrayBuffers');
		const mesh = Instance.new('EditableMesh');
		mesh.Name = 'JsPending';
		const positions = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]);
		const indices = new Uint16Array([0, 2, 1]);
		(async () => {
			const accepted = await mesh.SetGeometryFromBuffer(
				{ VertexCount: 3, IndexFormat: 'u16', Position: 0 }, positions, indices);
			if (!accepted) throw new Error('refused');
			mesh.Name = 'JsBuilt';
		})();
	)");
	Beat(store, *runtime);
	const auto mesh = store.FindFirstRoot("JsBuilt");
	REQUIRE(mesh != engine::ecs::NULL_ENTITY);
	CHECK(store.Get<engine::scene::EditableMesh>(mesh)->Indices == std::vector<uint32_t>{0, 2, 1});
}
