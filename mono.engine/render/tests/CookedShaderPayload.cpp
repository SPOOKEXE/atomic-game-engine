#include "CookedShaderPayload.hpp"

#include <engine/msl/Translate.hpp>
#include <engine/render/ShaderCompiler.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>

TEST_SUITE_ID("engine.render.cookedshaderpayload")
TEST_DEPENDS("engine.render.shadercompiler")

namespace {
	engine::assets::ShaderData Payloads() {
		engine::render::ShaderCompiler compiler;
		const auto compiled = compiler.Compile(
			"#version 450\nlayout(location=0)out vec4 colour;void main(){colour=vec4(1);}",
			engine::render::ShaderStage::Fragment,
			"cooked-selection.frag"
		);
		INFO(compiled.Error);
		REQUIRE_FALSE(compiled.Failed);
		const auto translated = engine::msl::Translate(compiled.SpirV);
		INFO(translated.Error);
		REQUIRE_FALSE(translated.Failed);
		engine::assets::ShaderData shader;
		shader.CompilerVersion = "shaderc-test-provenance";
		shader.OptimizerVersion = "none";
		shader.TranslatorVersion = "spirv-cross-test-provenance";
		shader.ShaderAbi = "atomic.material.v1";
		shader.TargetEnvironment = "vulkan1.0";
		engine::assets::ShaderVariant variant;
		variant.Name = "material";
		variant.Stage = "fragment";
		engine::assets::ShaderPayload msl;
		msl.Backend = "msl";
		msl.EntryPoint = engine::msl::ENTRY_POINT;
		msl.Target = "msl2.0-macos";
		const auto text = std::as_bytes(std::span(translated.Source));
		msl.Bytes.assign(text.begin(), text.end());
		variant.Payloads.push_back(std::move(msl));
		engine::assets::ShaderPayload spirv;
		spirv.Backend = "spirv";
		spirv.EntryPoint = "main";
		spirv.Target = "vulkan1.0";
		for (const auto word : compiled.SpirV)
			for (uint32_t byte = 0; byte < 4; ++byte)
				spirv.Bytes.push_back(std::byte((word >> (byte * 8)) & 255));
		variant.Payloads.push_back(std::move(spirv));
		shader.Variants.push_back(std::move(variant));
		REQUIRE(shader.IsValid());
		return shader;
	}
}

TEST_CASE("cooked selection requires explicit ABI variant stage and backend", "[render][cooked-payload]") {
	const auto shader = Payloads();
	engine::render::CookedShaderSelection selected;
	REQUIRE_FALSE(
		engine::render::SelectCookedPayload(
			shader, "atomic.material.v1", "material", "fragment", "msl", selected
		)
	);
	REQUIRE(selected.Payload != nullptr);
	CHECK(selected.Payload->EntryPoint == "main0");
	CHECK(selected.Payload->Backend == "msl");
	const auto accepted = selected;
	CHECK(
		engine::render::SelectCookedPayload(
			shader, "atomic.composer-hlsl.v1", "material", "fragment", "msl", selected
		)
			.has_value()
	);
	CHECK(selected.Payload == accepted.Payload);
	CHECK(
		engine::render::SelectCookedPayload(
			shader, "atomic.material.v1", "material", "vertex", "msl", selected
		)
			.has_value()
	);
	CHECK(
		engine::render::SelectCookedPayload(
			shader, "atomic.material.v1", "missing", "fragment", "msl", selected
		)
			.has_value()
	);
	CHECK(
		engine::render::SelectCookedPayload(
			shader, "atomic.material.v1", "material", "fragment", "metallib", selected
		)
			.has_value()
	);
}

TEST_CASE(
	"retained cooked backing accounts for both forms before replacing the accepted stage",
	"[render][cooked-payload]"
) {
	auto shader = Payloads();
	const auto &forms = shader.Variants[0].Payloads;
	const auto backingBytes = forms[0].Bytes.size() + forms[1].Bytes.size();
	engine::render::OwnedCookedStage retained;
	REQUIRE_FALSE(
		engine::render::RetainCookedStage(
			shader, "atomic.material.v1", "material", "fragment", backingBytes, retained
		)
	);
	REQUIRE(retained.Msl.has_value());
	CHECK(retained.Msl->Bytes == forms[0].Bytes);
	CHECK(retained.SpirV.Bytes == forms[1].Bytes);
	const auto accepted = retained;
	const auto error = engine::render::RetainCookedStage(
		shader, "atomic.material.v1", "material", "fragment", backingBytes - 1, retained
	);
	REQUIRE(error.has_value());
	CHECK(error->find("backing byte budget") != std::string::npos);
	CHECK(retained == accepted);
	shader.Variants.clear();
	CHECK(retained == accepted);
	CHECK(
		engine::render::RetainCookedStage(
			shader, "atomic.material.v1", "material", "fragment", backingBytes, retained
		)
			.has_value()
	);
	CHECK(retained == accepted);
}

TEST_CASE("cooked payload identity includes MSL bytes target and entry point", "[render][cooked-payload]") {
	const auto shader = Payloads();
	const auto &msl = shader.Variants[0].Payloads[0];
	auto changed = msl;
	changed.Bytes.push_back(std::byte{' '});
	CHECK_FALSE(engine::render::SameCookedPayload(msl, changed));
	changed = msl;
	changed.EntryPoint = "alternate";
	CHECK_FALSE(engine::render::SameCookedPayload(msl, changed));
	changed = msl;
	changed.Target = "msl2.1-macos";
	CHECK_FALSE(engine::render::SameCookedPayload(msl, changed));
	CHECK(engine::render::SameCookedPayload(msl, msl));
}
