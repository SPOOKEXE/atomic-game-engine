// A plugin's skinning loop over 25,600 vertices, interpreted and native.
//
// The same chunk runs in a plugin-origin runtime with and without `--!native`.
// The native row is meant to be at least three times faster; on a machine
// `luau_codegen_supported` refuses both rows measure the interpreter.
// Setup (buffer fill, compile) is inside the row and is small next to the
// eight passes the chunk makes.

#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/scriptluau/Runtime.hpp>
#include <engine/testing/Bench.hpp>

#include <string>
#include <string_view>

TEST_SUITE_ID("engine.scriptluau.bench.native")

namespace {
	constexpr std::string_view SKIN = R"(
local count = 25600
local input, out = buffer.create(count * 12), buffer.create(count * 12)
for i = 0, count * 3 - 1 do buffer.writef32(input, i * 4, (i % 97) * 0.013 - 0.5) end
local function skin(m: {number}, input: buffer, out: buffer)
	local m1, m2, m3, m4 = m[1], m[2], m[3], m[4]
	local m5, m6, m7, m8 = m[5], m[6], m[7], m[8]
	local m9, m10, m11, m12 = m[9], m[10], m[11], m[12]
	for v = 0, count - 1 do
		local o = v * 12
		local x, y, z = buffer.readf32(input, o), buffer.readf32(input, o + 4), buffer.readf32(input, o + 8)
		buffer.writef32(out, o, m1 * x + m2 * y + m3 * z + m4)
		buffer.writef32(out, o + 4, m5 * x + m6 * y + m7 * z + m8)
		buffer.writef32(out, o + 8, m9 * x + m10 * y + m11 * z + m12)
	end
end
local m = {0.9, 0.1, 0, 0.2, -0.1, 0.95, 0.05, 0, 0, 0, 1.1, -0.3}
for pass = 1, 8 do skin(m, input, out) end
)";

	bool RunSkin(bool native) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		engine::ecs::Store store("bench.luau.native");
		engine::scene::InstallServices(store);
		const auto runtime =
			engine::script::MakeLuauRuntime(store, {.Origin = engine::script::ScriptOrigin::Plugin});
		const std::string source = std::string(native ? "--!native\n" : "") + std::string(SKIN);
		return runtime != nullptr && runtime->Run(source);
	}
}

BENCH("plugin skin loop · 8 passes x 25600 vertices · interpreted", 1) {
	engine::testing::Consume(RunSkin(false) ? 1 : 0);
}

BENCH("plugin skin loop · 8 passes x 25600 vertices · --!native", 1) {
	engine::testing::Consume(RunSkin(true) ? 1 : 0);
}
