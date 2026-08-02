#include <engine/core/Bytes.hpp>
#include <engine/ecs/Column.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.ecs.column")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::ecs::Column;
using engine::ecs::ComponentId;
using engine::ecs::Components;

namespace column_test {
	struct Point {
		float X = 0.0f;
		float Y = 0.0f;
	};

	// No data. A column of these allocates nothing and counts rows.
	struct Marker {};

	// Over-aligned, so the allocation path has to honour the alignment rather
	// than assuming the default is enough.
	struct alignas(64) Wide {
		double Value = 0.0;
	};

	// Non-trivial and observable: every construction and destruction is
	// counted, so a leak or a double-destroy in the swap-back path is a number
	// that does not come back to zero.
	struct Tracked {
		static inline int Live = 0;
		static inline int Constructed = 0;

		std::string Text;

		Tracked() {
			Live++;
			Constructed++;
		}
		explicit Tracked(std::string text) : Text(std::move(text)) {
			Live++;
			Constructed++;
		}
		Tracked(const Tracked &other) : Text(other.Text) {
			Live++;
			Constructed++;
		}
		Tracked(Tracked &&other) noexcept : Text(std::move(other.Text)) {
			Live++;
			Constructed++;
		}
		Tracked &operator=(const Tracked &) = default;
		Tracked &operator=(Tracked &&) = default;
		~Tracked() {
			Live--;
		}
	};

	Point ReadPoint(const Column &column, size_t row) {
		return *static_cast<const Point *>(column.At(row));
	}

	const Tracked &ReadTracked(const Column &column, size_t row) {
		return *static_cast<const Tracked *>(column.At(row));
	}
}

using namespace column_test;

TEST_CASE("a default column holds nothing and has no type", "[ecs]") {
	Column column;
	REQUIRE(column.Empty());
	REQUIRE(column.Size() == 0);
	REQUIRE_FALSE(column.Type().IsValid());
	REQUIRE(column.Data() == nullptr);
}

TEST_CASE("rows push, read back and count", "[ecs]") {
	Column column(Components::Of<Point>());

	const Point first{1.0f, 2.0f};
	const Point second{3.0f, 4.0f};
	REQUIRE(column.PushCopy(&first) == 0);
	REQUIRE(column.PushCopy(&second) == 1);
	REQUIRE(column.Size() == 2);

	REQUIRE(ReadPoint(column, 0).X == 1.0f);
	REQUIRE(ReadPoint(column, 1).Y == 4.0f);
}

TEST_CASE("a default-pushed row is value-initialised", "[ecs]") {
	// Not merely "whatever was in the allocation". A component read before it
	// is written should be its declared default, and padding bytes should be
	// zero so that two identical worlds serialise identically.
	Column column(Components::Of<Point>());

	for (int index = 0; index < 32; index++) {
		column.PushDefault();
	}

	size_t nonZero = 0;
	for (size_t row = 0; row < column.Size(); row++) {
		const Point value = ReadPoint(column, row);
		if (value.X != 0.0f || value.Y != 0.0f) {
			nonZero++;
		}
	}
	REQUIRE(nonZero == 0);
}

TEST_CASE("rows stay contiguous as the column grows", "[ecs]") {
	// The property every batch iterator depends on: one base pointer and a
	// stride reach every row.
	Column column(Components::Of<Point>());

	constexpr size_t COUNT = 1000;
	for (size_t index = 0; index < COUNT; index++) {
		const Point value{static_cast<float>(index), 0.0f};
		column.PushCopy(&value);
	}

	REQUIRE(column.Size() == COUNT);

	const auto *base = static_cast<const Point *>(column.Data());
	size_t mismatches = 0;
	for (size_t index = 0; index < COUNT; index++) {
		if (base[index].X != static_cast<float>(index)) {
			mismatches++;
		}
	}
	REQUIRE(mismatches == 0);
}

TEST_CASE("capacity grows geometrically and is never given back", "[ecs]") {
	Column column(Components::Of<Point>());

	for (int index = 0; index < 100; index++) {
		column.PushDefault();
	}
	const size_t grown = column.Capacity();
	REQUIRE(grown >= 100);

	column.Clear();
	REQUIRE(column.Empty());

	// A world whose population oscillates must not reallocate on every
	// oscillation, which is the entire reason Clear keeps the capacity.
	REQUIRE(column.Capacity() == grown);

	const void *before = column.Data();
	for (int index = 0; index < 100; index++) {
		column.PushDefault();
	}
	REQUIRE(column.Data() == before);
}

TEST_CASE("reserve never shrinks", "[ecs]") {
	Column column(Components::Of<Point>());

	column.Reserve(256);
	const size_t reserved = column.Capacity();
	REQUIRE(reserved >= 256);

	column.Reserve(4);
	REQUIRE(column.Capacity() == reserved);
}

TEST_CASE("removal swaps the last row into the hole", "[ecs]") {
	Column column(Components::Of<Point>());
	for (int index = 0; index < 4; index++) {
		const Point value{static_cast<float>(index), 0.0f};
		column.PushCopy(&value);
	}

	column.RemoveSwapBack(1);

	REQUIRE(column.Size() == 3);
	REQUIRE(ReadPoint(column, 0).X == 0.0f);
	REQUIRE(ReadPoint(column, 1).X == 3.0f); // the last row moved here
	REQUIRE(ReadPoint(column, 2).X == 2.0f);
}

TEST_CASE("removing the last row does not move anything", "[ecs]") {
	Column column(Components::Of<Point>());
	for (int index = 0; index < 3; index++) {
		const Point value{static_cast<float>(index), 0.0f};
		column.PushCopy(&value);
	}

	column.RemoveSwapBack(2);
	REQUIRE(column.Size() == 2);
	REQUIRE(ReadPoint(column, 0).X == 0.0f);
	REQUIRE(ReadPoint(column, 1).X == 1.0f);
}

TEST_CASE("removing the only row empties the column", "[ecs]") {
	Column column(Components::Of<Point>());
	column.PushDefault();

	column.RemoveSwapBack(0);
	REQUIRE(column.Empty());

	// And removing from an empty column is a no-op rather than an underflow.
	column.RemoveSwapBack(0);
	REQUIRE(column.Size() == 0);
}

TEST_CASE("assign overwrites in place", "[ecs]") {
	Column column(Components::Of<Point>());
	column.PushDefault();

	const Point value{9.0f, 8.0f};
	column.Assign(0, &value);

	REQUIRE(column.Size() == 1);
	REQUIRE(ReadPoint(column, 0).X == 9.0f);
	REQUIRE(ReadPoint(column, 0).Y == 8.0f);
}

// --- tags ----------------------------------------------------------------

TEST_CASE("a tag column counts rows and allocates nothing", "[ecs]") {
	Column column(Components::Of<Marker>());
	REQUIRE(column.Describe().Size == 0);

	for (int index = 0; index < 10; index++) {
		column.PushDefault();
	}

	REQUIRE(column.Size() == 10);
	REQUIRE(column.Data() == nullptr);
	REQUIRE(column.At(3) == nullptr);

	column.RemoveSwapBack(2);
	REQUIRE(column.Size() == 9);

	column.Clear();
	REQUIRE(column.Empty());
}

// --- alignment -----------------------------------------------------------

TEST_CASE("an over-aligned type is stored at its alignment", "[ecs]") {
	Column column(Components::Of<Wide>());
	REQUIRE(column.Describe().Alignment == 64);

	for (int index = 0; index < 40; index++) {
		const Wide value{static_cast<double>(index)};
		column.PushCopy(&value);
	}

	// Every row, not only the base — a stride that ignored alignment would put
	// the first row right and the rest wrong.
	size_t misaligned = 0;
	for (size_t row = 0; row < column.Size(); row++) {
		if (reinterpret_cast<uintptr_t>(column.At(row)) % 64 != 0) {
			misaligned++;
		}
	}
	REQUIRE(misaligned == 0);
	REQUIRE(static_cast<const Wide *>(column.At(39))->Value == 39.0);
}

// --- non-trivial lifetimes -----------------------------------------------

TEST_CASE("a non-trivial type is destroyed exactly once per row", "[ecs]") {
	const int before = Tracked::Live;

	{
		Column column(Components::Of<Tracked>());
		REQUIRE_FALSE(column.Describe().Trivial);

		for (int index = 0; index < 50; index++) {
			const Tracked value(std::string("row-") + std::to_string(index));
			column.PushCopy(&value);
		}
		REQUIRE(column.Size() == 50);
		REQUIRE(Tracked::Live == before + 50);
	}

	// The destructor released every row. A column that only freed its bytes
	// would leave fifty strings allocated and this count high.
	REQUIRE(Tracked::Live == before);
}

TEST_CASE("a non-trivial type survives reallocation", "[ecs]") {
	const int before = Tracked::Live;

	Column column(Components::Of<Tracked>());
	for (int index = 0; index < 200; index++) {
		const Tracked value(std::string("value-") + std::to_string(index));
		column.PushCopy(&value);
	}

	// Growth move-constructs into the new block and destroys the old rows. Get
	// that wrong and the strings are either doubled or freed twice.
	REQUIRE(Tracked::Live == before + 200);
	REQUIRE(ReadTracked(column, 0).Text == "value-0");
	REQUIRE(ReadTracked(column, 199).Text == "value-199");

	column.Clear();
	REQUIRE(Tracked::Live == before);
}

TEST_CASE("swap-back destroys both the hole and the moved-from row", "[ecs]") {
	const int before = Tracked::Live;

	Column column(Components::Of<Tracked>());
	for (int index = 0; index < 4; index++) {
		const Tracked value(std::string("t") + std::to_string(index));
		column.PushCopy(&value);
	}
	REQUIRE(Tracked::Live == before + 4);

	column.RemoveSwapBack(1);

	// Three rows, three live objects. Four would mean the moved-from row was
	// left constructed; two would mean it was destroyed twice.
	REQUIRE(column.Size() == 3);
	REQUIRE(Tracked::Live == before + 3);
	REQUIRE(ReadTracked(column, 1).Text == "t3");

	column.Clear();
	REQUIRE(Tracked::Live == before);
}

TEST_CASE("moving a row between columns leaves one live object", "[ecs]") {
	const int before = Tracked::Live;

	Column source(Components::Of<Tracked>());
	Column destination(Components::Of<Tracked>());

	const Tracked value("moved");
	source.PushCopy(&value);
	REQUIRE(Tracked::Live == before + 2); // the local and the row

	destination.PushMovedFrom(source, 0);
	REQUIRE(destination.Size() == 1);
	REQUIRE(ReadTracked(destination, 0).Text == "moved");

	// The source row is still constructed — moved-from, but an object — which
	// is why the archetype removes it rather than assuming the move took it.
	REQUIRE(source.Size() == 1);
	source.RemoveSwapBack(0);

	destination.Clear();
	REQUIRE(Tracked::Live == before + 1); // only the local remains
}

TEST_CASE("a moved column leaves the source empty and typeless", "[ecs]") {
	const int before = Tracked::Live;

	Column source(Components::Of<Tracked>());
	const Tracked value("carried");
	source.PushCopy(&value);

	Column destination(std::move(source));
	REQUIRE(destination.Size() == 1);
	REQUIRE(ReadTracked(destination, 0).Text == "carried");

	REQUIRE(source.Size() == 0);
	REQUIRE_FALSE(source.Type().IsValid());
	REQUIRE(source.Data() == nullptr);

	destination.Clear();
	REQUIRE(Tracked::Live == before + 1);
}

TEST_CASE("move assignment destroys what it replaced", "[ecs]") {
	const int before = Tracked::Live;

	Column destination(Components::Of<Tracked>());
	for (int index = 0; index < 5; index++) {
		const Tracked value("old");
		destination.PushCopy(&value);
	}

	Column source(Components::Of<Tracked>());
	const Tracked replacement("new");
	source.PushCopy(&replacement);

	destination = std::move(source);

	// The five it held are gone, not leaked, and the one it took over is live.
	REQUIRE(destination.Size() == 1);
	REQUIRE(ReadTracked(destination, 0).Text == "new");
	REQUIRE(Tracked::Live == before + 2); // the row plus the local

	destination.Clear();
	REQUIRE(Tracked::Live == before + 1);
}

TEST_CASE("self move assignment does not destroy the column", "[ecs]") {
	Column column(Components::Of<Point>());
	const Point value{5.0f, 6.0f};
	column.PushCopy(&value);

	Column &alias = column;
	column = std::move(alias);

	REQUIRE(column.Size() == 1);
	REQUIRE(ReadPoint(column, 0).X == 5.0f);
}

// --- serialisation --------------------------------------------------------

TEST_CASE("a column round-trips through bytes", "[ecs]") {
	Column source(Components::Of<Point>());
	for (int index = 0; index < 16; index++) {
		const Point value{static_cast<float>(index), static_cast<float>(-index)};
		source.PushCopy(&value);
	}

	ByteWriter writer;
	REQUIRE(source.Write(writer));
	REQUIRE(writer.Size() == 16 * sizeof(Point));

	Column restored(Components::Of<Point>());
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.Read(reader, 16));
	REQUIRE(reader.AtEnd());

	REQUIRE(restored.Size() == 16);
	REQUIRE(std::memcmp(restored.Data(), source.Data(), 16 * sizeof(Point)) == 0);
}

TEST_CASE("an empty column round-trips as nothing", "[ecs]") {
	Column source(Components::Of<Point>());

	ByteWriter writer;
	REQUIRE(source.Write(writer));
	REQUIRE(writer.Empty());

	Column restored(Components::Of<Point>());
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.Read(reader, 0));
	REQUIRE(restored.Empty());
}

TEST_CASE("a tag column round-trips as a row count", "[ecs]") {
	Column source(Components::Of<Marker>());
	for (int index = 0; index < 7; index++) {
		source.PushDefault();
	}

	ByteWriter writer;
	REQUIRE(source.Write(writer));
	REQUIRE(writer.Empty()); // the count is the archetype's to record

	Column restored(Components::Of<Marker>());
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.Read(reader, 7));
	REQUIRE(restored.Size() == 7);
}

TEST_CASE("a truncated buffer leaves the column empty rather than half read", "[ecs]") {
	Column source(Components::Of<Point>());
	for (int index = 0; index < 8; index++) {
		source.PushDefault();
	}

	ByteWriter writer;
	source.Write(writer);

	// Half the rows are present; the reader is told to expect all eight.
	std::vector<std::byte> truncated(writer.Bytes().begin(), writer.Bytes().begin() + 4 * sizeof(Point));

	Column restored(Components::Of<Point>());
	ByteReader reader(truncated);
	REQUIRE_FALSE(restored.Read(reader, 8));
	REQUIRE(reader.Failed());

	// Empty, not four rows and a lie about the other four.
	REQUIRE(restored.Empty());
}

TEST_CASE("a type with no serialisation refuses rather than writing bytes", "[ecs]") {
	// A std::string's object representation is a pointer into this process.
	// Writing it would produce a snapshot that restores as a crash.
	Column column(Components::Of<Tracked>());
	const Tracked value("unserialisable");
	column.PushCopy(&value);

	ByteWriter writer;
	REQUIRE_FALSE(column.Write(writer));
	REQUIRE(writer.Empty());

	Column restored(Components::Of<Tracked>());
	ByteReader reader(writer.Bytes());
	REQUIRE_FALSE(restored.Read(reader, 1));
	REQUIRE(reader.Failed());

	column.Clear();
}
