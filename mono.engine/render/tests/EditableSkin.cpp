// A script-built skinned mesh: the render conversion of its skin, and the
// palette the draw path builds from a `Bone` tree, applied on the CPU the way
// `opaque.vert`'s `ApplySkin` applies it on the GPU.

#include <engine/assets/Mesh.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/EditableMeshes.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.render.editableskin")
TEST_DEPENDS("engine.scene.editablemeshskin")

using namespace engine;

namespace {
	// The skinned position of one vertex of `mesh` under a palette.
	core::Vector3 Skinned(const assets::MeshVertex &vertex, const std::vector<core::CFrame> &palette) {
		const core::Vector3 position{vertex.Position[0], vertex.Position[1], vertex.Position[2]};
		core::Vector3 total{};
		for (size_t slot = 0; slot < 4; slot++) {
			if (vertex.Weights[slot] == 0) continue;
			const float weight = float(vertex.Weights[slot]) / 65535.0f;
			total = total + palette[vertex.Joints[slot]].PointToWorldSpace(position) * weight;
		}
		return total;
	}

	struct Rig {
		ecs::Store Store{"editable-skin"};
		ecs::Entity Part;
		ecs::Entity Mesh;
		std::array<ecs::Entity, 2> Bones;

		Rig() {
			scene::RegisterSceneClasses();
			render::RegisterPresentationComponents();
			Part = Store.CreateInstance(ecs::Classes::Find(core::Name("SkinnedMeshPart")), "Body");
			Mesh = Store.CreateInstance(scene::EditableMeshClass(), "BodyMesh");
			REQUIRE(Store.SetParent(Mesh, Part));
			Bones[0] = Store.CreateInstance(scene::BoneClass(), "Root");
			Bones[1] = Store.CreateInstance(scene::BoneClass(), "Arm");
			REQUIRE(Store.SetParent(Bones[0], Part));
			REQUIRE(Store.SetParent(Bones[1], Bones[0]));
			REQUIRE(scene::BindSkeletonBones(Store, Part, Bones).empty());
			Store.GetMutable<scene::Bone>(Bones[1])->Rest.Position = {0, 1, 0};
			Store.GetMutable<scene::Bone>(Bones[1])->InverseBind.Position = {0, -1, 0};

			for (const core::Vector3 position : {core::Vector3{0, 0, 0}, {0, 2, 0}, {1, 2, 0}})
				REQUIRE(scene::AddVertex(Store, Mesh, position));
			REQUIRE(scene::AddTriangle(Store, Mesh, 0, 1, 2));
			const std::array<uint16_t, 1> root{0};
			const std::array<uint16_t, 2> both{0, 1};
			const std::array<float, 1> whole{1};
			const std::array<float, 2> split{1, 3};
			REQUIRE(scene::SetVertexSkin(Store, Mesh, 0, root, whole));
			REQUIRE(scene::SetVertexSkin(Store, Mesh, 1, std::span(both.data() + 1, 1), whole));
			REQUIRE(scene::SetVertexSkin(Store, Mesh, 2, both, split));
		}

		// What one presented frame computes: resolve, then the palette.
		std::vector<core::CFrame> Palette() {
			scene::ResolveBones(Store);
			render::DrawList draw;
			scene::DrawInstance instance;
			instance.Source = Part.Id;
			instance.Frame = Store.Get<scene::Transform>(Part)->Frame;
			draw.Instances.push_back(instance);
			render::CollectSkinPalettes(Store, draw);
			return draw.JointFrames;
		}
	};
}

TEST_CASE("an editable mesh skin converts to normalised render weights", "[render][editable-skin]") {
	Rig rig;
	const assets::MeshData built = render::BuildMeshData(*rig.Store.Get<scene::EditableMesh>(rig.Mesh));
	REQUIRE(built.IsValid());
	CHECK(built.JointCount == 2);
	CHECK(built.Vertices[1].Joints[0] == 1);
	CHECK(built.Vertices[1].Weights[0] == 65535);
	CHECK(built.Vertices[2].Weights[0] + built.Vertices[2].Weights[1] == 65535);
	CHECK(built.Vertices[2].Weights[1] == Catch::Approx(65535 * 0.75).margin(1));
}

TEST_CASE(
	"an inverse bind or rest edit changes the skinned result the same frame", "[render][editable-skin]"
) {
	Rig rig;
	const auto mesh = [&] { return render::BuildMeshData(*rig.Store.Get<scene::EditableMesh>(rig.Mesh)); };
	const std::vector<core::CFrame> bind = rig.Palette();
	REQUIRE(bind.size() == 2);
	// At bind pose every vertex is where it was authored.
	const assets::MeshData atRest = mesh();
	CHECK((Skinned(atRest.Vertices[1], bind) - core::Vector3{0, 2, 0}).Magnitude() < 1e-5f);

	// A rebuilt bind pose: the arm now binds one unit lower, through the
	// script-facing property rather than the row.
	const core::CFrame lowered(core::Vector3{0, -2, 0});
	REQUIRE(rig.Store.SetProperty(rig.Bones[1], core::Name("InverseBindCFrame"), &lowered, sizeof(lowered)));
	const std::vector<core::CFrame> rebound = rig.Palette();
	CHECK((Skinned(atRest.Vertices[1], rebound) - core::Vector3{0, 1, 0}).Magnitude() < 1e-5f);

	// New rest positions under the same palette.
	REQUIRE(scene::SetVertexPosition(rig.Store, rig.Mesh, 1, {0, 3, 0}));
	CHECK((Skinned(mesh().Vertices[1], rebound) - core::Vector3{0, 2, 0}).Magnitude() < 1e-5f);

	// An animated joint moves the bound vertices.
	rig.Store.GetMutable<scene::Bone>(rig.Bones[1])->Transform.Position = {1, 0, 0};
	CHECK((Skinned(mesh().Vertices[1], rig.Palette()) - core::Vector3{1, 2, 0}).Magnitude() < 1e-5f);
}
