#include "resources/MaterialSamplerAdmission.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/resources/ShaderAssembly.hpp>
#include <engine/render/resources/ShaderCompiler.hpp>
#include <engine/render/resources/ShaderLibrary.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/content/MaterialLayers.hpp>
#include <engine/scene/content/Materials.hpp>
#include <engine/scene/content/Shaders.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <map>

TEST_SUITE_ID("engine.render.shaderassembly")

using namespace engine;
using namespace engine::render;

namespace {
	const char *const PLAIN =
		"void Surface(SurfaceInput i, inout SurfaceOutput o) { o.Albedo = vec3(i.UV, 0.0); }\n";
	const char *const LIT =
		"void Surface(SurfaceInput i, inout SurfaceOutput o) { DefaultSurface(i, o); }\n"
		"vec3 Light(SurfaceOutput s, LightSample l) {\n"
		"	return s.Albedo * l.Colour * l.Shadow * step(0.3, dot(s.Normal, l.Direction));\n"
		"}\n";

	ShaderModuleResolver From(std::map<std::string, std::string> held) {
		return [held = std::move(held)](std::string_view name) -> std::optional<std::string> {
			const auto found = held.find(std::string(name));
			return found == held.end() ? std::nullopt : std::optional(found->second);
		};
	}

	void Compiles(const ShaderAssembly &assembled) {
		REQUIRE(assembled.Error.empty());
		ShaderCompiler compiler;
		compiler.SetOptimise(true);
		for (const std::string *text : {&assembled.Forward, &assembled.GBuffer}) {
			const auto compiled = compiler.Compile(*text, ShaderStage::Fragment, "surface");
			INFO(compiled.Error);
			REQUIRE_FALSE(compiled.Failed);
			const auto refused = AdmitMaterialSamplers(compiled.SpirV);
			INFO(refused.value_or(""));
			CHECK_FALSE(refused.has_value());
		}
	}
}

TEST_CASE("a surface compiles for both passes with and without a Light", "[render][shader]") {
	const auto plain = AssembleShader("plain", PLAIN, {});
	CHECK(plain.Surface);
	CHECK_FALSE(plain.Lit);
	Compiles(plain);
	const auto lit = AssembleShader("lit", LIT, {});
	CHECK(lit.Lit);
	Compiles(lit);
}

TEST_CASE("every engine lobe is callable from a Light", "[render][shader]") {
	Compiles(AssembleShader(
		"lobes",
		"#include \"engine/brdf\"\n"
		"void Surface(SurfaceInput i, inout SurfaceOutput o) { DefaultSurface(i, o); o.Custom[0].x = "
		"SurfaceCurvature(); }\n"
		"vec3 Light(SurfaceOutput s, LightSample l) {\n"
		"	vec3 view = vec3(0, 0, 1);\n"
		"	float cosine = dot(s.Normal, l.Direction);\n"
		"	vec3 total = BrdfPreIntegrated(cosine, vec3(s.Custom[0].x)) + BrdfWrap(cosine, 0.5);\n"
		"	total += BrdfFresnel(view, l.Direction, vec3(0.04)) * BrdfGgx(s.Normal, view, l.Direction, "
		"0.3);\n"
		"	total += BrdfTransmission(s.Normal, l.Direction, 0.01, vec3(0.02));\n"
		"	total += BrdfSheen(s.Normal, view, l.Direction, 0.4) + BrdfStrand(s.Normal, view, l.Direction, "
		"40.0);\n"
		"	return total * l.Colour;\n"
		"}\n",
		{}
	));
}

TEST_CASE("includes resolve world modules once and refuse cycles and strangers", "[render][shader]") {
	const auto modules = From(
		{{"a", "#include \"b\"\nfloat A() { return B(); }\n"},
		 {"b", "float B() { return 0.5; }\n"},
		 {"loop", "#include \"loop\"\n"}}
	);
	const std::string head = "#include \"b\"\n#include \"a\"\n";
	const auto good = AssembleShader(
		"s", head + "void Surface(SurfaceInput i, inout SurfaceOutput o) { o.Roughness = A(); }\n", modules
	);
	Compiles(good);
	const auto refused = [&](std::string source) {
		return AssembleShader("s", source + PLAIN, modules).Error;
	};
	CHECK(refused("#include \"loop\"\n").find("includes itself") != std::string::npos);
	CHECK(refused("\n#include \"gone\"\n").starts_with("s:2: "));
	CHECK(refused("#include <b>\n").find("quoted") != std::string::npos);
	// A commented include is not one.
	CHECK(refused("// #include \"gone\"\n").empty());
	CHECK_FALSE(
		AssembleShader("s", "#version 450\n#include \"engine/brdf\"\nvoid main() {}\n", modules).Error.empty()
	);
}

TEST_CASE("a whole program is untouched without includes and expands them with", "[render][shader]") {
	const std::string program =
		"#version 450\nlayout(location = 0) out vec4 c;\nvoid main() { c = vec4(1); }\n";
	const auto same = AssembleShader("p", program, {});
	CHECK(same.Forward == program);
	CHECK(same.GBuffer.empty());
	const auto expanded = AssembleShader(
		"p",
		"#version 450\n#include \"b\"\nlayout(location = 0) out vec4 c;\nvoid main() { c = vec4(B()); }\n",
		From({{"b", "float B() { return 0.5; }\n"}})
	);
	REQUIRE(expanded.Error.empty());
	ShaderCompiler compiler;
	const auto compiled = compiler.Compile(expanded.Forward, ShaderStage::Fragment, "p");
	INFO(compiled.Error);
	CHECK_FALSE(compiled.Failed);
}

TEST_CASE("compile errors name the author's file and line", "[render][shader]") {
	ShaderCompiler compiler;
	const auto own = AssembleShader(
		"mine", "\n\nvoid Surface(SurfaceInput i, inout SurfaceOutput o) { o.Albedo = nope; }\n", {}
	);
	const auto first = compiler.Compile(own.Forward, ShaderStage::Fragment, "mine");
	REQUIRE(first.Failed);
	INFO(first.Error);
	CHECK(first.Error.find("mine:3:") != std::string::npos);
	const auto other = AssembleShader(
		"mine", std::string("#include \"bad\"\n") + PLAIN, From({{"bad", "\nfloat Bad() { return nope; }\n"}})
	);
	const auto second = compiler.Compile(other.Forward, ShaderStage::Fragment, "mine");
	REQUIRE(second.Failed);
	INFO(second.Error);
	CHECK(second.Error.find("bad:2:") != std::string::npos);
}

TEST_CASE("editing a module recompiles the scripts that include it and no others", "[render][shader]") {
	scene::RegisterSceneComponents();
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	ecs::Store store("modules");
	const auto select = [&](const char *name) {
		const auto material = store.CreateInstance(scene::MaterialClass(), "Material");
		store.GetMutable<scene::MaterialRef>(material)->Shader = core::Name(name);
	};
	const auto user = store.CreateInstance(scene::ShaderScriptClass(), "user");
	const auto alone = store.CreateInstance(scene::ShaderScriptClass(), "alone");
	const auto module = store.CreateInstance(scene::ShaderModuleClass(), "lib");
	REQUIRE(scene::SetShaderSource(store, module, "float Rough() { return 0.2; }\n"));
	REQUIRE(
		scene::SetShaderSource(
			store,
			user,
			"#include \"lib\"\nvoid Surface(SurfaceInput i, inout SurfaceOutput o) { o.Roughness = Rough(); "
			"}\n"
		)
	);
	REQUIRE(scene::SetShaderSource(store, alone, PLAIN));
	select("user");
	select("alone");
	ShaderLibrary library;
	CHECK(library.Refresh(store) == 2);
	REQUIRE(library.Find(core::Name("user")) != nullptr);
	CHECK_FALSE(library.Find(core::Name("user"))->GBufferSpirV.empty());
	CHECK(library.Refresh(store) == 0);

	REQUIRE(scene::SetShaderSource(store, module, "float Rough() { return 0.7; }\n"));
	CHECK(library.Refresh(store) == 1);
	CHECK(library.Changed().front() == core::Name("user"));

	// A broken module keeps the last good words and reports on the script.
	REQUIRE(scene::SetShaderSource(store, module, "float Rough() { return nope; }\n"));
	CHECK(library.Refresh(store) == 0);
	CHECK_FALSE(library.Find(core::Name("user"))->SpirV.empty());
	CHECK(scene::ShaderErrorOf(store, user).find("lib:1") != std::string::npos);
	CHECK(scene::ShaderErrorOf(store, alone).empty());
	// A world with a broken shader still saves.
	core::ByteWriter image;
	CHECK(store.Save(image));
	REQUIRE(scene::SetShaderSource(store, module, "float Rough() { return 0.4; }\n"));
	CHECK(library.Refresh(store) == 1);
	CHECK(scene::ShaderErrorOf(store, user).empty());
}

TEST_CASE("declared values compile and bad declarations name their line", "[render][shader]") {
	Compiles(AssembleShader(
		"declared",
		"#pragma parameter float a = 0.5 // half\n#pragma parameter vec4 b = 1, 2, 3, -4e-2\n"
		"#pragma parameter vec3 c\n#pragma texture one\n#pragma texture two\n"
		"void Surface(SurfaceInput i, inout SurfaceOutput o) {\n"
		"	o.Albedo = c * a + b.rgb + texture(one, i.UV).rgb * texture(two, i.UV).rgb;\n}\n",
		{}
	));
	const auto refused = [](const char *line) {
		return AssembleShader("s", std::string("\n") + line + "\n" + PLAIN, {}).Error;
	};
	CHECK(refused("#pragma parameter vec5 a").starts_with("s:2: "));
	CHECK(refused("#pragma parameter vec2 a = 1").starts_with("s:2: "));
	CHECK(refused("#pragma parameter float a = 1, 2").starts_with("s:2: "));
	CHECK(refused("#pragma texture").starts_with("s:2: "));
	CHECK(refused("#pragma texture a\n#pragma texture b\n#pragma texture c").starts_with("s:4: "));
	CHECK(refused("#pragma optimize(off)").empty());
}

TEST_CASE("a composed layer stack compiles over maps, a Surface and a Light", "[render][shader]") {
	const std::string mud = "#pragma parameter float dirt = 0.5\n#pragma texture masks\n"
							"float Helper() { return dirt; }\n"
							"void Layer(SurfaceInput i, inout SurfaceOutput o) { o.Roughness = Helper(); }\n";
	const std::string paint =
		"void Layer(SurfaceInput i, inout SurfaceOutput o) { o.Albedo = vec3(1, 0, 0); }\n";
	const auto modules = From({{"mud", mud}, {"paint", paint}});
	std::vector<scene::LayerSource> layers{
		{"mud", mud, "masks.g"},
		{"paint", paint, "", 1.0f, scene::LayerBlend::Multiply, scene::LAYER_ALBEDO},
		{"mud", mud, "masks.b", 1.0f, scene::LayerBlend::Screen},
		{"paint", paint, "", 1.0f, scene::LayerBlend::Add},
		{"mud", mud, "masks.r@attribute1", 1.0f, scene::LayerBlend::Height},
	};
	for (const std::string base : {std::string(), std::string(PLAIN), std::string(LIT)}) {
		const auto composed = scene::ComposeLayers(base, layers);
		REQUIRE(composed.Error.empty());
		const auto assembled = AssembleShader("layered", composed.Source, modules);
		CHECK(assembled.Surface);
		CHECK(assembled.Lit == (base == LIT));
		Compiles(assembled);
	}
}
