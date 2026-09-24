#include "../src/ArrayOps.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.array_ops")

using engine::imagegraph::ArrayItem;
using engine::imagegraph::Status;
using engine::imagegraph::detail::ArrayProcessMode;
using engine::imagegraph::detail::BuildArraySchedule;
using engine::imagegraph::detail::CollectArray;

TEST_CASE("spread appends one array level and leaves nested values intact", "[imagegraph][array]") {
	using Item = ArrayItem<unsigned>;
	const Item first{1u};
	const Item nested{std::vector<Item>{Item{2u}, Item{std::vector<Item>{Item{3u}}}}};
	const std::array<Item, 2> inputs{first, nested};
	std::vector<Item> output;
	REQUIRE(CollectArray<unsigned>(inputs, false, 8, output) == Status::Ok);
	CHECK(output == std::vector<Item>{first, nested});
	REQUIRE(CollectArray<unsigned>(inputs, true, 8, output) == Status::Ok);
	CHECK(output == std::vector<Item>{first, Item{2u}, Item{std::vector<Item>{Item{3u}}}});
}

TEST_CASE("spread checks the full nested shape before replacing output", "[imagegraph][array]") {
	using Item = ArrayItem<unsigned>;
	const Item nested{std::vector<Item>{Item{1u}, Item{2u}}};
	const std::array<Item, 1> inputs{nested};
	std::vector<Item> output{Item{99u}};
	CHECK(CollectArray<unsigned>(inputs, false, 2, output) == Status::LimitExceeded);
	CHECK(output == std::vector<Item>{Item{99u}});
	REQUIRE(CollectArray<unsigned>(inputs, true, 2, output) == Status::Ok);
	CHECK(output == std::vector<Item>{Item{1u}, Item{2u}});
	Item deep{1u};
	for (size_t depth = 0; depth < 16; depth++)
		deep = Item{std::vector<Item>{std::move(deep)}};
	CHECK(CollectArray<unsigned>(std::array<Item, 1>{deep}, false, 32, output) == Status::LimitExceeded);
	CHECK(output == std::vector<Item>{Item{1u}, Item{2u}});
}

TEST_CASE("array processor modes select ordered unequal-length elements", "[imagegraph][array]") {
	const std::array<size_t, 2> lengths{2, 3};
	std::vector<std::vector<size_t>> output;
	REQUIRE(BuildArraySchedule(lengths, ArrayProcessMode::Loop, 8, output) == Status::Ok);
	CHECK(output == std::vector<std::vector<size_t>>{{0, 0}, {1, 1}, {0, 2}});
	REQUIRE(BuildArraySchedule(lengths, ArrayProcessMode::Hold, 8, output) == Status::Ok);
	CHECK(output == std::vector<std::vector<size_t>>{{0, 0}, {1, 1}, {1, 2}});
	REQUIRE(BuildArraySchedule(lengths, ArrayProcessMode::Expand, 8, output) == Status::Ok);
	CHECK(output == std::vector<std::vector<size_t>>{{0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 1}, {1, 2}});
	REQUIRE(BuildArraySchedule(lengths, ArrayProcessMode::ExpandInverse, 8, output) == Status::Ok);
	CHECK(output == std::vector<std::vector<size_t>>{{0, 0}, {1, 0}, {0, 1}, {1, 1}, {0, 2}, {1, 2}});
}

TEST_CASE("array processor refuses empty inputs and product overflow", "[imagegraph][array]") {
	std::vector<std::vector<size_t>> output{{99}};
	CHECK(
		BuildArraySchedule(std::array<size_t, 2>{0, 2}, ArrayProcessMode::Loop, 8, output) ==
		Status::InvalidValue
	);
	CHECK(output == std::vector<std::vector<size_t>>{{99}});
	CHECK(
		BuildArraySchedule(std::array<size_t, 2>{64, 65}, ArrayProcessMode::Expand, 4096, output) ==
		Status::LimitExceeded
	);
	CHECK(output == std::vector<std::vector<size_t>>{{99}});
	CHECK(
		BuildArraySchedule(std::array<size_t, 2>{2, 3}, ArrayProcessMode::Expand, 5, output) ==
		Status::LimitExceeded
	);
	CHECK(output == std::vector<std::vector<size_t>>{{99}});
	CHECK(
		BuildArraySchedule(std::array<size_t, 2>{2, 3}, static_cast<ArrayProcessMode>(9), 8, output) ==
		Status::InvalidValue
	);
	const std::array<size_t, 8> longLengths{4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096};
	REQUIRE(BuildArraySchedule(longLengths, ArrayProcessMode::Loop, 4096, output) == Status::Ok);
	CHECK(output.size() == 4096);
	CHECK(output.back() == std::vector<size_t>(longLengths.size(), 4095));
}
