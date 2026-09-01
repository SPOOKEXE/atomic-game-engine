// The whole Roblox place boundary, from foreign bytes to the engine-facing tree.
//
// Real place imports hold tens of millions of properties. The row below keeps
// that cost visible without checking a large external fixture into the tree. It
// uses thirty-two properties per instance because property storage, not the
// shallow instance shell, was the measured memory and time hot path.

#include <engine/bake/RobloxModel.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>

TEST_SUITE_ID("engine.bake.bench.robloxfile")

namespace {
	const std::string &RobloxPlace() {
		static const std::string place = [] {
			constexpr size_t INSTANCES = 20'000;
			std::string made;
			made.reserve(INSTANCES * 1'600);
			made += "<roblox version=\"4\">";
			for (size_t instance = 0; instance < INSTANCES; instance++) {
				made += "<Item class=\"Part\" referent=\"RBX";
				made += std::to_string(instance);
				made += "\"><Properties><string name=\"Name\">Part";
				made += std::to_string(instance);
				made += "</string>";
				for (size_t property = 0; property < 32; property++) {
					made += "<string name=\"Value";
					made += std::to_string(property);
					made += "\">text</string>";
				}
				made += "</Properties></Item>";
			}
			made += "</roblox>";
			return made;
		}();
		return place;
	}

	std::span<const std::byte> Bytes(std::string_view text) {
		return std::as_bytes(std::span(text));
	}
}

BENCH_PER_ITEM("ReadRobloxFile · 20k instances with 32 properties", 20'000) {
	engine::bake::RobloxModel model;
	std::string failure;
	if (!engine::bake::ReadRobloxFile(Bytes(RobloxPlace()), model, failure)) {
		throw std::runtime_error(failure);
	}
	engine::testing::Consume(model.Roots.size());
}
