// Measures runtime SPIR-V to MSL translation on the module's four-resource fixture.

#include "../tests/fixtures/Fragment.hpp"

#include <engine/msl/Translate.hpp>
#include <engine/testing/Bench.hpp>

#include <stdexcept>

TEST_SUITE_ID("engine.msl.bench.translate")

BENCH("SPIRV-Cross translation of four-resource fragment", 16) {
	const engine::msl::Translation translated = engine::msl::Translate(engine::msl::testdata::FRAGMENT);
	if (translated.Failed) {
		throw std::runtime_error("MSL benchmark fixture refused: " + translated.Error);
	}
	engine::testing::Consume(translated.Source.size());
}
