#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/content/MaterialLayers.hpp>
#include <engine/scene/content/MaterialLayout.hpp>
#include <engine/scene/content/Materials.hpp>
#include <engine/scene/content/Shaders.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.materiallayers")

using namespace engine;
using namespace engine::scene;

namespace {
	struct World {
		ecs::Store Store{"layers"};
		ecs::Entity Part;
		World() {
			RegisterSceneComponents();
			RegisterSceneClasses();
			Part = Store.CreateInstance(ecs::Classes::Find(core::Name("MeshPart")), "body");
		}
		void Module(const char *name, const char *code) {
			const auto module = Store.CreateInstance(ShaderModuleClass(), name);
			REQUIRE(SetShaderSource(Store, module, code));
		}
		ecs::Entity Layer(const char *shader, int32_t order) {
			const auto layer = Store.CreateInstance(MaterialLayerClass(), shader);
			auto *held = Store.GetMutable<MaterialLayer>(layer);
			held->Shader = core::Name(shader);
			held->Order = order;
			REQUIRE(Store.SetParent(layer, Part));
			return layer;
		}
		core::Name Shader() {
			(void)ResolveMaterials(Store);
			return Store.Get<SurfaceAppearance>(Part)->Shader;
		}
		std::string Source() {
			return ShaderTextOf(Store, Shader()).Code;
		}
	};
	const char *const MUD = "#pragma parameter float dirt = 0.5\n#pragma texture masks\n"
							"void Layer(SurfaceInput i, inout SurfaceOutput o) { o.Roughness = dirt; }\n";
	const char *const PAINT =
		"void Layer(SurfaceInput i, inout SurfaceOutput o) { o.Albedo = vec3(1, 0, 0); }\n";
}

TEST_CASE("layers compose in order and removing them restores the part", "[scene][material]") {
	World world;
	world.Module("mud", MUD);
	world.Module("paint", PAINT);
	CHECK_FALSE(world.Shader().IsValid());
	const auto mud = world.Layer("mud", 5);
	const auto paint = world.Layer("paint", 1);
	const core::Name stacked = world.Shader();
	REQUIRE(stacked.IsValid());
	const std::string first = world.Source();
	CHECK(first.find("#include \"paint\"") < first.find("#include \"mud\""));
	world.Store.GetMutable<MaterialLayer>(paint)->Order = 9;
	const std::string second = world.Source();
	CHECK(second.find("#include \"mud\"") < second.find("#include \"paint\""));

	// A demanded shader, and one a save does not hold.
	std::vector<core::Name> demanded;
	DemandedShaders(world.Store, demanded);
	CHECK(std::find(demanded.begin(), demanded.end(), world.Shader()) != demanded.end());
	core::ByteWriter image;
	CHECK(world.Store.Save(image));

	world.Store.Destroy(mud);
	world.Store.Destroy(paint);
	CHECK_FALSE(world.Shader().IsValid());
	DemandedShaders(world.Store, demanded);
	CHECK(demanded.empty());
}

TEST_CASE("opacity moves a parameter and not the shader", "[scene][material]") {
	World world;
	world.Module("paint", PAINT);
	const auto layer = world.Layer("paint", 0);
	const core::Name before = world.Shader();
	const auto values = [&] {
		return FormatMaterialParameters(*world.Store.Get<SurfaceAppearance>(world.Part));
	};
	CHECK(values() == "0=1");
	world.Store.GetMutable<MaterialLayer>(layer)->Opacity = 0.25f;
	CHECK(world.Shader() == before);
	CHECK(values() == "0=0.25");
}

TEST_CASE("equal stacks share a shader and an edited module makes a new one", "[scene][material]") {
	World world;
	world.Module("paint", PAINT);
	(void)world.Layer("paint", 0);
	const core::Name first = world.Shader();
	const auto twin = world.Store.CreateInstance(ecs::Classes::Find(core::Name("MeshPart")), "twin");
	const auto layer = world.Store.CreateInstance(MaterialLayerClass(), "paint");
	world.Store.GetMutable<MaterialLayer>(layer)->Shader = core::Name("paint");
	REQUIRE(world.Store.SetParent(layer, twin));
	CHECK(world.Shader() == first);
	CHECK(world.Store.Get<SurfaceAppearance>(twin)->Shader == first);
	// Blend and channels are part of the text.
	world.Store.GetMutable<MaterialLayer>(layer)->Blend = LayerBlend::Multiply;
	(void)world.Shader();
	CHECK(world.Store.Get<SurfaceAppearance>(twin)->Shader != first);
}

TEST_CASE("a layer's declarations follow the base's and masks must be declared", "[scene][material]") {
	const LayerSource mud{"mud", MUD, "masks.g"};
	const auto composed = ComposeLayers(
		"#pragma parameter vec3 tint = 1, 1, 1\nvoid Surface(SurfaceInput i, inout SurfaceOutput o) {}\n",
		std::span(&mud, 1)
	);
	REQUIRE(composed.Error.empty());
	const auto layout = ParseMaterialLayout(composed.Source);
	REQUIRE(layout.Error.empty());
	CHECK(layout.Find("tint")->Offset == 0);
	CHECK(layout.Find("dirt")->Offset == 3);
	CHECK(layout.FindTexture("masks") == 0);
	for (const char *mask : {"nope.r", "masks.x", "masks.rg"}) {
		const LayerSource bad{"mud", MUD, mask};
		CHECK_FALSE(ComposeLayers("", std::span(&bad, 1)).Error.empty());
	}
	// No float left: the opacity is written in.
	const LayerSource plain{"paint", PAINT, "", 0.5f};
	const auto full = ComposeLayers(
		"#pragma parameter vec4 a\n#pragma parameter vec4 b\n#pragma parameter vec4 c\n#pragma parameter "
		"vec4 d\n"
		"void Surface(SurfaceInput i, inout SurfaceOutput o) {}\n",
		std::span(&plain, 1)
	);
	REQUIRE(full.Error.empty());
	CHECK(full.Opacities.front().empty());
	CHECK(full.Source.find("5e-01") != std::string::npos);
}

TEST_CASE("a base that arrives under layers moves their opacities out of its floats", "[scene][material]") {
	World world;
	world.Module("paint", PAINT);
	(void)world.Layer("paint", 0);
	(void)world.Shader();
	const auto script = world.Store.CreateInstance(ShaderScriptClass(), "base");
	REQUIRE(SetShaderSource(
		world.Store,
		script,
		"#pragma parameter vec3 tint = 1, 1, 1\nvoid Surface(SurfaceInput i, inout SurfaceOutput o) {}\n"
	));
	const auto material = world.Store.CreateInstance(MaterialClass(), "Material");
	world.Store.GetMutable<MaterialRef>(material)->Shader = core::Name("base");
	REQUIRE(world.Store.SetParent(material, world.Part));
	// A value a script set before the stack was composed again stays.
	auto *appearance = world.Store.GetMutable<SurfaceAppearance>(world.Part);
	const float tint[]{0.2f, 0.3f, 0.4f};
	REQUIRE(SetMaterialParameter(
		*appearance, *ParseMaterialLayout("#pragma parameter vec3 tint").Find("tint"), tint, 3
	));
	(void)world.Shader();
	CHECK(world.Store.Get<SurfaceAppearance>(world.Part)->MaterialParameters[0] == 0.2f);
	REQUIRE(SetMaterialParameter(
		*world.Store.GetMutable<SurfaceAppearance>(world.Part),
		*ParseMaterialLayout("#pragma parameter vec3 tint").Find("tint"),
		nullptr,
		0
	));
	// Float 0 is the base's now and unset; the opacity is after it.
	CHECK(FormatMaterialParameters(*world.Store.Get<SurfaceAppearance>(world.Part)) == "3=1");
}

TEST_CASE("a material's keywords make a variant and only declared ones", "[scene][material]") {
	World world;
	const auto script = world.Store.CreateInstance(ShaderScriptClass(), "base");
	REQUIRE(SetShaderSource(
		world.Store,
		script,
		"#pragma keyword COAT\n#pragma keyword FUZZ\nvoid Surface(SurfaceInput i, inout SurfaceOutput o) {}\n"
	));
	const auto material = world.Store.CreateInstance(MaterialClass(), "Material");
	world.Store.GetMutable<MaterialRef>(material)->Shader = core::Name("base");
	REQUIRE(world.Store.SetParent(material, world.Part));
	CHECK(world.Shader() == core::Name("base"));
	world.Store.GetMutable<MaterialRef>(material)->Keywords = core::Name("COAT, FUZZ");
	CHECK(world.Source().find("#define COAT 1\n#define FUZZ 1\n") != std::string::npos);
	world.Store.GetMutable<MaterialRef>(material)->Keywords = core::Name("NOPE");
	CHECK(world.Source().find("#error") != std::string::npos);
	world.Store.GetMutable<MaterialRef>(material)->Keywords = core::Name{};
	CHECK(world.Shader() == core::Name("base"));
}

TEST_CASE("masks read the coordinates they name", "[scene][material]") {
	const auto composed = [](const char *mask) {
		const LayerSource layer{"mud", MUD, mask};
		return ComposeLayers("", std::span(&layer, 1));
	};
	CHECK(composed("masks.g@mesh").Source.find("i.MeshUV).g") != std::string::npos);
	CHECK(composed("masks.b@attribute2").Source.find("i.Attributes[2].xy).b") != std::string::npos);
	CHECK_FALSE(composed("masks.g@attribute4").Error.empty());
	CHECK_FALSE(composed("masks.g@nowhere").Error.empty());
}
