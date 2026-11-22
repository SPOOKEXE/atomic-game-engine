// `MeshKernels` from both languages.
//
// The arithmetic is asserted in `engine.script.meshkernels`; this suite asks
// whether the two bindings hand it the same bytes and answer the same way: a
// Luau `buffer` and a JavaScript `Float32Array` over one kernel, a refusal that
// leaves `out` alone, and a wrong argument type that raises in both.
//
// The answer crosses as `workspace.Name`, for `ScriptCall.cpp`'s reason.

#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.scripthost.meshkernels")
TEST_DEPENDS("engine.script.meshkernels")

using engine::ecs::Store;
using engine::script::Language;
using engine::script::MakeRuntime;

namespace {
	// Runs `source` and hands back `workspace.Name`, or the error text.
	std::string Answer(Language language, const std::string &source) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		Store store("meshkernels");
		const auto runtime = MakeRuntime(store, language);
		REQUIRE(runtime != nullptr);
		const auto workspace = engine::scene::WorkspaceOf(store);
		INFO(source);
		if (!runtime->Run(source.c_str())) return "error: " + std::string(runtime->LastError());
		return std::string(store.InstanceNameOf(workspace).Text());
	}
}

TEST_CASE("both languages run a kernel over their own byte stream", "[scripting][meshkernels]") {
	const std::string luau = Answer(Language::Luau, R"(
		local out, x = buffer.create(12), buffer.create(12)
		for i = 0, 2 do
			buffer.writef32(out, i * 4, i + 1)
			buffer.writef32(x, i * 4, (i + 1) * 10)
		end
		local ok = MeshKernels.Axpy(out, 0.5, x, 3)
		local refused = MeshKernels.Axpy(out, 1, x, 4)
		workspace.Name = `{ok} {refused} {buffer.readf32(out, 0)} {buffer.readf32(out, 4)} {buffer.readf32(out, 8)}`
	)");
	const std::string javascript = Answer(Language::JavaScript, R"(
		const out = new Float32Array([1, 2, 3]);
		const x = new Float32Array([10, 20, 30]);
		const ok = MeshKernels.Axpy(out, 0.5, x, 3);
		const refused = MeshKernels.Axpy(out.buffer, 1, x, 4);
		workspace.Name = `${ok} ${refused} ${out[0]} ${out[1]} ${out[2]}`;
	)");
	CHECK(luau == "true false 6 12 18");
	CHECK(javascript == luau);
}

TEST_CASE("both languages take CSR adjacency as a pair", "[scripting][meshkernels]") {
	const std::string luau = Answer(Language::Luau, R"(
		local function f32(values)
			local b = buffer.create(#values * 4)
			for i, v in values do buffer.writef32(b, (i - 1) * 4, v) end
			return b
		end
		local function u32(values)
			local b = buffer.create(#values * 4)
			for i, v in values do buffer.writeu32(b, (i - 1) * 4, v) end
			return b
		end
		local positions = f32({0, 0, 0, 1, 1, 0, 2, 0, 0})
		local adjacency = {u32({0, 1, 3, 4}), u32({1, 0, 2, 1})}
		local named = {offsets = adjacency[1], neighbours = adjacency[2]}
		local a = MeshKernels.Laplacian(positions, positions, adjacency, nil, 1, 0.5)
		local b = MeshKernels.Laplacian(positions, positions, named, f32({0, 0, 0}), 1, 0.5)
		workspace.Name = `{a} {b} {buffer.readf32(positions, 4)}`
	)");
	const std::string javascript = Answer(Language::JavaScript, R"(
		const positions = new Float32Array([0, 0, 0, 1, 1, 0, 2, 0, 0]);
		const adjacency = [new Uint32Array([0, 1, 3, 4]), new Uint32Array([1, 0, 2, 1])];
		const named = { offsets: adjacency[0], neighbours: adjacency[1] };
		const a = MeshKernels.Laplacian(positions, positions, adjacency, null, 1, 0.5);
		const b = MeshKernels.Laplacian(positions, positions, named, new Float32Array(3), 1, 0.5);
		workspace.Name = `${a} ${b} ${positions[1 * 3 + 1]}`;
	)");
	CHECK(luau == "true true 0.5");
	CHECK(javascript == luau);
}

TEST_CASE("a stream that is not one raises in both languages", "[scripting][meshkernels]") {
	CHECK(Answer(Language::Luau, "MeshKernels.Gather({}, buffer.create(4), buffer.create(4), 1)")
			  .starts_with("error"));
	CHECK(Answer(Language::JavaScript, "MeshKernels.Gather({}, new ArrayBuffer(4), new ArrayBuffer(4), 1);")
			  .starts_with("error"));
	CHECK(Answer(Language::Luau, "MeshKernels.Axpy(buffer.create(4), 1, buffer.create(4), -1)")
			  .starts_with("error"));
}
