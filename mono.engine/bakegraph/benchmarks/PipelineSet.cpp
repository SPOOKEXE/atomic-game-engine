// The steady-state cost of finding a pipeline in editor documents of each size.
//
// A world usually has only a few pipelines, but the studio can hold many while
// an author experiments. The set stays vector-backed for its stable serialized
// order, so these rows locate the point where a text binary search beats the
// integer equality walk it replaces.

#include <engine/bakegraph/Document.hpp>
#include <engine/core/Name.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.bakegraph.bench.pipeline-set")

using engine::bake::Document;
using engine::bake::PipelineSet;
using engine::core::Name;
using engine::testing::Consume;

namespace pipeline_set_bench {
	struct Fixture {
		std::vector<Name> Names;
		PipelineSet Pipelines;
	};

	Fixture BuildFixture(size_t count) {
		Fixture fixture;
		fixture.Names.reserve(count);
		for (size_t index = 0; index < count; ++index) {
			fixture.Names.emplace_back("engine.bench.pipeline." + std::to_string(index));
		}

		// Reverse insertion makes every fixture take the same sorted-insert path.
		for (size_t index = fixture.Names.size(); index > 0; --index) {
			fixture.Pipelines.Set(fixture.Names[index - 1], Document());
		}
		return fixture;
	}

	template <size_t count> const Fixture &FixtureOf() {
		static const Fixture fixture = BuildFixture(count);
		return fixture;
	}

	void FindAll(const Fixture &fixture) {
		for (size_t index = 0; index < 100'000; ++index) {
			Consume(fixture.Pipelines.Find(fixture.Names[index % fixture.Names.size()]));
		}
	}

	// The former lookup body, kept as the control for choosing a crossover.
	const Name *FindLinear(const PipelineSet &set, Name wanted) {
		for (const Name &held : set.Names()) {
			if (held == wanted) {
				return &held;
			}
		}
		return nullptr;
	}

	void FindAllLinear(const Fixture &fixture) {
		for (size_t index = 0; index < 100'000; ++index) {
			Consume(FindLinear(fixture.Pipelines, fixture.Names[index % fixture.Names.size()]));
		}
	}

	const Name *FindBinary(const PipelineSet &set, Name wanted) {
		const auto found =
			std::lower_bound(set.Names().begin(), set.Names().end(), wanted, [](Name left, Name right) {
				return left.Text() < right.Text();
			});
		return found != set.Names().end() && *found == wanted ? &*found : nullptr;
	}

	void FindAllBinary(const Fixture &fixture) {
		for (size_t index = 0; index < 100'000; ++index) {
			Consume(FindBinary(fixture.Pipelines, fixture.Names[index % fixture.Names.size()]));
		}
	}
}

using namespace pipeline_set_bench;

// Fixtures are initialized on a warm-up sample, outside every retained timing.
BENCH("control · linear Find · 4 pipelines", 100'000) {
	FindAllLinear(FixtureOf<4>());
}

BENCH("control · text binary Find · 4 pipelines", 100'000) {
	FindAllBinary(FixtureOf<4>());
}

BENCH("PipelineSet::Find · 4 pipelines", 100'000) {
	FindAll(FixtureOf<4>());
}

BENCH("control · linear Find · 16 pipelines", 100'000) {
	FindAllLinear(FixtureOf<16>());
}

BENCH("control · text binary Find · 16 pipelines", 100'000) {
	FindAllBinary(FixtureOf<16>());
}

BENCH("PipelineSet::Find · 16 pipelines", 100'000) {
	FindAll(FixtureOf<16>());
}

BENCH("control · linear Find · 64 pipelines", 100'000) {
	FindAllLinear(FixtureOf<64>());
}

BENCH("control · text binary Find · 64 pipelines", 100'000) {
	FindAllBinary(FixtureOf<64>());
}

BENCH("PipelineSet::Find · 64 pipelines", 100'000) {
	FindAll(FixtureOf<64>());
}

BENCH("control · linear Find · 256 pipelines", 100'000) {
	FindAllLinear(FixtureOf<256>());
}

BENCH("control · text binary Find · 256 pipelines", 100'000) {
	FindAllBinary(FixtureOf<256>());
}

BENCH("PipelineSet::Find · 256 pipelines", 100'000) {
	FindAll(FixtureOf<256>());
}

BENCH("control · linear Find · 512 pipelines", 100'000) {
	FindAllLinear(FixtureOf<512>());
}

BENCH("control · text binary Find · 512 pipelines", 100'000) {
	FindAllBinary(FixtureOf<512>());
}

BENCH("PipelineSet::Find · 512 pipelines", 100'000) {
	FindAll(FixtureOf<512>());
}

BENCH("control · linear Find · 1024 pipelines", 100'000) {
	FindAllLinear(FixtureOf<1024>());
}

BENCH("control · text binary Find · 1024 pipelines", 100'000) {
	FindAllBinary(FixtureOf<1024>());
}

BENCH("PipelineSet::Find · 1024 pipelines", 100'000) {
	FindAll(FixtureOf<1024>());
}

BENCH("control · linear Find · 2048 pipelines", 100'000) {
	FindAllLinear(FixtureOf<2048>());
}

BENCH("control · text binary Find · 2048 pipelines", 100'000) {
	FindAllBinary(FixtureOf<2048>());
}

BENCH("PipelineSet::Find · 2048 pipelines", 100'000) {
	FindAll(FixtureOf<2048>());
}

BENCH("control · linear Find · 4096 pipelines", 100'000) {
	FindAllLinear(FixtureOf<4096>());
}

BENCH("control · text binary Find · 4096 pipelines", 100'000) {
	FindAllBinary(FixtureOf<4096>());
}

BENCH("PipelineSet::Find · 4096 pipelines", 100'000) {
	FindAll(FixtureOf<4096>());
}
