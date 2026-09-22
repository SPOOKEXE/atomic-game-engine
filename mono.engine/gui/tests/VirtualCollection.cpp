#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/VirtualCollection.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <stdexcept>
#include <string>

TEST_SUITE_ID("engine.gui.virtual_collection")

using namespace engine;
using namespace engine::gui;

namespace {
	ecs::AttributeValue StringValue(std::string value) {
		ecs::AttributeValue result;
		result.Type = ecs::PropertyType::String;
		result.String = std::move(value);
		return result;
	}

	ecs::AttributeValue IntegerValue(int32_t value) {
		ecs::AttributeValue result;
		result.Type = ecs::PropertyType::Int32;
		result.Int32 = value;
		return result;
	}

	VirtualCollection Collection() {
		VirtualCollection collection;
		collection.ItemCount = 1000000;
		collection.Page.First = 400;
		collection.Page.ExtentBefore = 7200.0f;
		collection.FixedExtent = 18.0f;
		collection.Overscan = 5;
		collection.Revision = 41;
		collection.Page.Records = {
			VirtualRecord{
				.Key = "player-\xF0\x9F\x9A\x80",
				.Fields =
					{
						VirtualField{.Name = "Name", .Value = StringValue("Ada")},
						VirtualField{.Name = "Score", .Value = IntegerValue(73)},
					},
				.MeasuredExtent = 30.0f,
			},
		};
		return collection;
	}

	VirtualCollection BoundaryCollection() {
		VirtualCollection collection;
		collection.ItemCount = 999;
		collection.Page.Records.reserve(collection.ItemCount);

		// 46 bytes of prelude, then 999 records whose fixed fields total 26
		// bytes each. One 604-byte value and 998 1,024-byte values fill the
		// 1 MiB page exactly.
		for (uint32_t index = 0; index < collection.ItemCount; index++) {
			std::string key = "r";
			const std::string indexText = std::to_string(index);
			key.append(3 - indexText.size(), '0');
			key.append(indexText);

			VirtualField field;
			field.Name = "v";
			field.Value =
				StringValue(std::string(index == 0 ? 604 : VirtualCollection::MAXIMUM_TEXT_BYTES, 'x'));
			collection.Page.Records.push_back(
				VirtualRecord{
					.Key = std::move(key),
					.Fields = {std::move(field)},
				}
			);
		}
		return collection;
	}
}

TEST_CASE("a virtual collection retains stable typed source records", "[gui][virtual]") {
	const VirtualCollection collection = Collection();
	REQUIRE(ValidateVirtualCollection(collection));

	const VirtualRecord *record = FindVirtualRecord(collection, "player-\xF0\x9F\x9A\x80");
	REQUIRE(record != nullptr);
	const ecs::AttributeValue *score = FindVirtualField(*record, "Score");
	REQUIRE(score != nullptr);
	CHECK(score->Type == ecs::PropertyType::Int32);
	CHECK(score->Int32 == 73);
	CHECK(FindVirtualRecord(collection, "missing") == nullptr);
	CHECK(FindVirtualField(*record, "missing") == nullptr);
	CHECK(VirtualRecordAt(collection, 399) == nullptr);
	CHECK(VirtualRecordAt(collection, 400) == record);
	CHECK(VirtualRecordAt(collection, 401) == nullptr);
	CHECK(VirtualExtentOf(collection, *record) == Catch::Approx(18.0f));
	CHECK(VirtualEstimatedTotalExtent(collection) == Catch::Approx(18000000.0f));

	VirtualCollection measured = collection;
	measured.ExtentPolicy = VirtualExtentPolicy::Measured;
	CHECK(ValidateVirtualCollection(measured));
	const VirtualRecord *measuredRecord = FindVirtualRecord(measured, "player-\xF0\x9F\x9A\x80");
	REQUIRE(measuredRecord != nullptr);
	CHECK(VirtualExtentOf(measured, *measuredRecord) == Catch::Approx(30.0f));
	CHECK(VirtualEstimatedTotalExtent(measured) == Catch::Approx(18000012.0f));
}

TEST_CASE("a virtual collection maps grid records into bounded row-major cells", "[gui][virtual]") {
	VirtualCollection grid = Collection();
	grid.ItemCount = 7;
	grid.Page.First = 0;
	grid.Page.Records.clear();
	for (uint32_t index = 0; index < grid.ItemCount; index++) {
		grid.Page.Records.push_back(VirtualRecord{.Key = std::to_string(index), .Fields = {}});
	}
	grid.LayoutPolicy = VirtualLayoutPolicy::Grid;
	grid.GridColumns = 3;
	grid.GridCellStride = {40.0f, 25.0f};
	REQUIRE(ValidateVirtualCollection(grid));
	CHECK(VirtualCellPosition(grid, 0) == engine::core::Vector2{0.0f, 0.0f});
	CHECK(VirtualCellPosition(grid, 5) == engine::core::Vector2{80.0f, 25.0f});
	CHECK(VirtualEstimatedTotalExtent(grid) == Catch::Approx(75.0f));
	CHECK(VirtualCanvasExtent(grid) == engine::core::Vector2{120.0f, 75.0f});

	grid.ExtentPolicy = VirtualExtentPolicy::Measured;
	CHECK_FALSE(ValidateVirtualCollection(grid));
	grid.ExtentPolicy = VirtualExtentPolicy::Fixed;
	grid.GridColumns = 0;
	CHECK_FALSE(ValidateVirtualCollection(grid));
}

TEST_CASE("virtual collection validation refuses ambiguous and malformed source data", "[gui][virtual]") {
	VirtualCollection collection = Collection();
	collection.Page.Records.push_back(collection.Page.Records.front());
	CHECK_FALSE(ValidateVirtualCollection(collection));

	collection = Collection();
	collection.Page.Records.front().Fields.push_back(collection.Page.Records.front().Fields.front());
	CHECK_FALSE(ValidateVirtualCollection(collection));

	collection = Collection();
	collection.Page.Records.front().Key = "\xC3";
	CHECK_FALSE(ValidateVirtualCollection(collection));

	collection = Collection();
	collection.Page.Records.front().Fields.front().Value.String = "\xC3";
	CHECK_FALSE(ValidateVirtualCollection(collection));

	collection = Collection();
	collection.ExtentPolicy = VirtualExtentPolicy::Measured;
	collection.Page.Records.front().MeasuredExtent = std::numeric_limits<float>::infinity();
	CHECK_FALSE(ValidateVirtualCollection(collection));

	collection = Collection();
	collection.Page.Records.front().Fields.front().Value.Type = ecs::PropertyType::Reference;
	CHECK_FALSE(ValidateVirtualCollection(collection));

	collection = Collection();
	collection.Page.First = collection.ItemCount;
	collection.Page.Records.clear();
	CHECK(ValidateVirtualCollection(collection));
	collection.Page.First++;
	CHECK_FALSE(ValidateVirtualCollection(collection));

	collection = Collection();
	collection.Overscan = VirtualCollection::MAXIMUM_OVERSCAN + 1;
	CHECK_FALSE(ValidateVirtualCollection(collection));

	collection = Collection();
	collection.FixedExtent = VirtualCollection::MAXIMUM_CANVAS_EXTENT;
	CHECK_FALSE(ValidateVirtualCollection(collection));

	collection = Collection();
	collection.Page.Records.front().Fields.front().Value = IntegerValue(0);
	collection.Page.Records.front().Fields.front().Value.Type = ecs::PropertyType::Float;
	collection.Page.Records.front().Fields.front().Value.Float = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(ValidateVirtualCollection(collection));

	collection = Collection();
	collection.Page.Records.front().Fields.front().Value.Type = ecs::PropertyType::NumberSequence;
	collection.Page.Records.front().Fields.front().Value.NumberSequence.Count = core::SEQUENCE_CAPACITY + 1;
	CHECK_FALSE(ValidateVirtualCollection(collection));
}

TEST_CASE("a virtual collection round trips its revision keys fields and extents", "[gui][virtual]") {
	VirtualCollection written = Collection();
	written.LayoutPolicy = VirtualLayoutPolicy::Grid;
	written.GridColumns = 2;
	written.GridCellStride = {48.0f, 30.0f};
	ecs::AttributeValue named;
	named.Type = ecs::PropertyType::Name;
	named.Name = core::Name("Level");
	written.Page.Records.front().Fields.push_back(VirtualField{.Name = "Rank", .Value = named});

	core::ByteWriter writer;
	REQUIRE(WriteVirtualCollection(writer, written));

	VirtualCollection restored;
	core::ByteReader reader(writer.Bytes());
	REQUIRE(ReadVirtualCollection(reader, restored));
	CHECK(reader.AtEnd());
	CHECK(restored.Revision == 41);
	CHECK(restored.ExtentPolicy == VirtualExtentPolicy::Fixed);
	CHECK(restored.FixedExtent == Catch::Approx(18.0f));
	CHECK(restored.LayoutPolicy == VirtualLayoutPolicy::Grid);
	CHECK(restored.GridColumns == 2);
	CHECK(restored.GridCellStride == engine::core::Vector2{48.0f, 30.0f});
	CHECK(restored.Overscan == 5);
	CHECK(restored.ItemCount == 1000000);
	CHECK(restored.Page.First == 400);
	CHECK(restored.Page.ExtentBefore == Catch::Approx(7200.0f));
	REQUIRE(restored.Page.Records.size() == 1);
	CHECK(restored.Page.Records.front().Key == "player-\xF0\x9F\x9A\x80");
	CHECK(restored.Page.Records.front().MeasuredExtent == Catch::Approx(30.0f));
	const ecs::AttributeValue *name = FindVirtualField(restored.Page.Records.front(), "Name");
	REQUIRE(name != nullptr);
	CHECK(name->Type == ecs::PropertyType::String);
	CHECK(name->String == "Ada");
	const ecs::AttributeValue *rank = FindVirtualField(restored.Page.Records.front(), "Rank");
	REQUIRE(rank != nullptr);
	CHECK(rank->Type == ecs::PropertyType::Name);
	CHECK(rank->Name == core::Name("Level"));
}

TEST_CASE("a virtual collection page limit matches emitted bytes exactly", "[gui][virtual]") {
	VirtualCollection boundary = BoundaryCollection();
	REQUIRE(ValidateVirtualCollection(boundary));

	core::ByteWriter writer;
	REQUIRE(WriteVirtualCollection(writer, boundary));
	CHECK(writer.Size() == VirtualCollection::MAXIMUM_PAGE_BYTES);

	VirtualCollection restored;
	core::ByteReader reader(writer.Bytes());
	REQUIRE(ReadVirtualCollection(reader, restored));
	CHECK(reader.AtEnd());
	CHECK(restored.Page.Records.size() == boundary.Page.Records.size());
	CHECK(restored.Page.Records.front().Fields.front().Value.String.size() == 604);

	boundary.Page.Records.front().Fields.front().Value.String.push_back('x');
	CHECK_FALSE(ValidateVirtualCollection(boundary));
	CHECK_FALSE(WriteVirtualCollection(writer, boundary));
}

TEST_CASE("a malformed virtual collection does not replace existing data", "[gui][virtual]") {
	VirtualCollection written = Collection();
	core::ByteWriter writer;
	REQUIRE(WriteVirtualCollection(writer, written));
	const auto bytes = writer.Bytes();
	REQUIRE(bytes.size() > 1);

	VirtualCollection preserved = Collection();
	preserved.Revision = 99;
	core::ByteReader reader(bytes.first(bytes.size() - 1));
	CHECK_FALSE(ReadVirtualCollection(reader, preserved));
	CHECK(reader.Failed());
	CHECK(preserved.Revision == 99);
	CHECK(preserved.Page.Records.front().Key == "player-\xF0\x9F\x9A\x80");
}

TEST_CASE(
	"a virtual collection rejects an over-capacity sequence before reading its entries", "[gui][virtual]"
) {
	core::ByteWriter writer;
	writer.WriteUInt8(static_cast<uint8_t>(VirtualExtentPolicy::Fixed));
	writer.WriteFloat(20.0f);
	writer.WriteUInt8(static_cast<uint8_t>(VirtualLayoutPolicy::List));
	writer.WriteUInt32(1);
	writer.WriteFloat(100.0f);
	writer.WriteFloat(24.0f);
	writer.WriteUInt32(0);
	writer.WriteUInt64(7);
	writer.WriteUInt32(1);
	writer.WriteUInt32(0);
	writer.WriteUInt32(1);
	writer.WriteString("entry");
	writer.WriteFloat(0.0f);
	writer.WriteUInt32(1);
	writer.WriteString("Curve");
	writer.WriteUInt8(static_cast<uint8_t>(ecs::PropertyType::NumberSequence));
	writer.WriteUInt32(core::SEQUENCE_CAPACITY + 1);

	VirtualCollection preserved = Collection();
	core::ByteReader reader(writer.Bytes());
	CHECK_FALSE(ReadVirtualCollection(reader, preserved));
	CHECK(reader.Failed());
	CHECK(preserved.Revision == 41);
}

TEST_CASE("an invalid virtual collection makes its snapshot save fail explicitly", "[gui][virtual]") {
	RegisterVirtualCollectionComponents();
	ecs::Store store("gui_virtual_collection.invalid_snapshot");
	VirtualCollection invalid = Collection();
	invalid.Overscan = VirtualCollection::MAXIMUM_OVERSCAN + 1;
	store.Set(store.Create(), invalid);

	core::ByteWriter writer;
	CHECK_THROWS_AS(store.Save(writer), std::invalid_argument);
	CHECK(writer.Empty());
}
