#include "ChunkPool.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/ecs/Column.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.ecs.column")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::ecs::ChunkPool;
using engine::ecs::Column;
using engine::ecs::ComponentId;
using engine::ecs::Components;

namespace {
	// Chunks the pool has taken from the allocator, read off the metrics sink
	// the pool now counts into. Nothing in this binary drains, so the value is
	// a running total for the length of the suite.
	double ChunksAllocated() {
		const auto counter = engine::core::Metrics::Get(ChunkPool::ALLOCATED_COUNTER);
		return counter ? counter->Value : 0.0;
	}

	// Chunks handed back out of the freelist, read the same way.
	double ChunksReused() {
		const auto counter = engine::core::Metrics::Get(ChunkPool::REUSED_COUNTER);
		return counter ? counter->Value : 0.0;
	}
}

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
	REQUIRE(column.ChunkCount() == 0);
	REQUIRE(column.At(0) == nullptr);
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

TEST_CASE("rows stay contiguous inside a chunk and the directory reaches every one", "[ecs]") {
	// The property every batch iterator depends on, in the form chunking leaves
	// it: one base pointer and a stride reach every row *of one chunk*, and the
	// directory reaches every chunk. A batch never crosses a boundary precisely
	// so that the first half of that sentence stays true.
	Column column(Components::Of<Point>());

	// Deliberately several chunks and not a whole number of them, so the short
	// trailing chunk is covered rather than only the full ones.
	constexpr size_t COUNT = 1000;
	for (size_t index = 0; index < COUNT; index++) {
		const Point value{static_cast<float>(index), 0.0f};
		column.PushCopy(&value);
	}

	REQUIRE(column.Size() == COUNT);
	REQUIRE(column.ChunkCount() == Column::ChunksFor(COUNT));
	REQUIRE(column.ChunkCount() > 4);

	size_t mismatches = 0;
	for (size_t index = 0; index < COUNT; index++) {
		const size_t chunk = Column::ChunkOf(index);
		const size_t offset = index - Column::ChunkStart(chunk);
		const auto *base = static_cast<const Point *>(column.ChunkData()[chunk]);
		if (base[offset].X != static_cast<float>(index)) {
			mismatches++;
		}

		// And the row accessor agrees with the arithmetic the iteration paths
		// do by hand, which is the thing that would silently diverge.
		if (column.At(index) != &base[offset]) {
			mismatches++;
		}
	}
	REQUIRE(mismatches == 0);

	// The boundaries partition the rows: every chunk starts where the previous
	// one ended, so no row is in two chunks and none is in none.
	size_t gaps = 0;
	for (size_t chunk = 1; chunk < column.ChunkCount(); chunk++) {
		if (Column::ChunkStart(chunk) != Column::ChunkStart(chunk - 1) + Column::ChunkRows(chunk - 1)) {
			gaps++;
		}
	}
	REQUIRE(gaps == 0);
}

TEST_CASE("a chunk keeps its address while later chunks come and go", "[ecs]") {
	// What a batch body holding a pointer depends on. Growth links a new chunk
	// rather than copying everything into a bigger allocation, so the rows
	// already there never move - which is what took the copy out of the growth
	// curve the column already had.
	Column column(Components::Of<Point>());
	for (size_t index = 0; index < Column::FIRST_CHUNK_ROWS; index++) {
		column.PushDefault();
	}
	REQUIRE(column.ChunkCount() == 1);

	void *first = column.ChunkData()[0];
	for (size_t index = 0; index < 1000; index++) {
		column.PushDefault();
	}

	REQUIRE(column.ChunkCount() > 4);
	REQUIRE(column.ChunkData()[0] == first);
}

TEST_CASE("capacity is never more than twice the rows", "[ecs]") {
	// The property doubling chunks buy, and the one a fixed chunk size cannot
	// have: a column of a million rows is a handful of chunks with almost every
	// row in the largest two, and a column of one row costs eight - which is
	// exactly what the old first capacity charged, so nothing got worse for a
	// world that never grows.
	Column column(Components::Of<Point>());
	size_t worst = 0;
	for (size_t index = 1; index <= 5000; index++) {
		column.PushDefault();
		worst = std::max(worst, column.Capacity() * 8 / index);
	}

	// Eight rows minimum, so the ratio only settles once past it.
	REQUIRE(column.Capacity() <= 2 * column.Size());
	REQUIRE(worst <= 8 * Column::FIRST_CHUNK_ROWS);
}

TEST_CASE("capacity follows the population back down", "[ecs]") {
	// **The item.** A column used to keep its high-water mark forever, so a
	// world that peaked at ten thousand entities and settled at a hundred held
	// the peak - a thousand of those measured 703 MB against 2.7 MB of live
	// rows. A chunk goes back the moment the rows stop reaching into it.
	Column column(Components::Of<Point>());

	constexpr size_t PEAK = 10'000;
	for (size_t index = 0; index < PEAK; index++) {
		column.PushDefault();
	}
	const size_t atPeak = column.ResidentBytes();
	REQUIRE(atPeak >= PEAK * sizeof(Point));

	while (column.Size() > 100) {
		column.RemoveSwapBack(column.Size() - 1);
	}

	REQUIRE(column.ChunkCount() == Column::ChunksFor(100));
	REQUIRE(column.ResidentBytes() == column.Capacity() * sizeof(Point));
	REQUIRE(column.Capacity() < 2 * 100 + Column::FIRST_CHUNK_ROWS);
	REQUIRE(atPeak > 30 * column.ResidentBytes());
}

TEST_CASE("a population oscillating across a chunk boundary never reaches the allocator", "[ecs]") {
	// The other half of the trade, and the reason the pool exists at all. Giving
	// a chunk back on every dip would put eight times the allocations in front
	// of a large world's columns, which is exactly the measurement that got a
	// smaller `SparseSet` page rejected at 8-21% slower. The pool is what makes
	// the release free; without it this test is what would go red.
	//
	// Emptied first because the pool is process-wide and capped: a suite that
	// has just destroyed a large world can leave it at the cap, where the next
	// release goes to the allocator and this case would fail for a reason that
	// is not the one it is about.
	ChunkPool::Trim();

	Column column(Components::Of<Point>());
	for (size_t index = 0; index < 1024; index++) {
		column.PushDefault();
	}

	// Warm: whatever the boundary crossing needs has been taken from the
	// allocator once by the time the count is read.
	column.PushDefault();
	column.RemoveSwapBack(column.Size() - 1);

	const double allocated = ChunksAllocated();
	for (int cycle = 0; cycle < 200; cycle++) {
		column.PushDefault();
		column.RemoveSwapBack(column.Size() - 1);
	}

	REQUIRE(ChunksAllocated() == allocated);
}

TEST_CASE("clearing hands every chunk back and refilling takes them from the pool", "[ecs]") {
	ChunkPool::Trim();

	Column column(Components::Of<Point>());
	for (size_t index = 0; index < 1024; index++) {
		column.PushDefault();
	}
	const size_t chunks = column.ChunkCount();
	REQUIRE(chunks == Column::ChunksFor(1024));

	column.Clear();
	REQUIRE(column.Empty());
	REQUIRE(column.ChunkCount() == 0);
	REQUIRE(column.ResidentBytes() == 0);

	const double allocated = ChunksAllocated();
	const double reused = ChunksReused();
	for (size_t index = 0; index < 1024; index++) {
		column.PushDefault();
	}

	REQUIRE(column.ChunkCount() == chunks);
	REQUIRE(ChunksAllocated() == allocated);

	// The other half of the same claim, and the half that had no coverage at
	// all while it was a counter only this module could read: the refill came
	// out of the freelist rather than being satisfied some other way.
	REQUIRE(ChunksReused() == reused + static_cast<double>(chunks));
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
	REQUIRE(column.ChunkCount() == 0);
	REQUIRE(column.At(3) == nullptr);
	REQUIRE(column.ResidentBytes() == 0);

	column.RemoveSwapBack(2);
	REQUIRE(column.Size() == 9);

	column.Clear();
	REQUIRE(column.Empty());
}

TEST_CASE("the pool stops retaining past its cap", "[ecs]") {
	// **A pool that never trims relocates the leak and reports success.** The
	// cap is the trim policy that does the work - `Trim` is for a host that
	// knows it has stopped needing the spares - so it is the one that has to be
	// pinned rather than described.
	ChunkPool::Trim();
	REQUIRE(ChunkPool::RetainedBytes() == 0);

	// `Wide` is 64 bytes, so a column reaching a quarter of a million rows holds
	// well past the cap in its largest chunks alone.
	{
		Column column(Components::Of<Wide>());
		column.Reserve(4 * ChunkPool::RETAINED_BYTES_CAP / sizeof(Wide));
		REQUIRE(column.ResidentBytes() > 2 * ChunkPool::RETAINED_BYTES_CAP);
	}

	REQUIRE(ChunkPool::RetainedBytes() <= ChunkPool::RETAINED_BYTES_CAP);
	ChunkPool::Trim();
	REQUIRE(ChunkPool::RetainedBytes() == 0);
}

// --- alignment -----------------------------------------------------------

TEST_CASE("an over-aligned type is stored at its alignment in every chunk", "[ecs]") {
	Column column(Components::Of<Wide>());
	REQUIRE(column.Describe().Alignment == 64);

	// Past several boundaries on purpose. This is the guard against a pool that
	// hands back a chunk it allocated at the default alignment: the first chunk
	// would very likely be aligned by luck, and every one after it would not.
	const size_t count = 300;
	for (size_t index = 0; index < count; index++) {
		const Wide value{static_cast<double>(index)};
		column.PushCopy(&value);
	}
	REQUIRE(column.ChunkCount() > 4);

	// Every row, not only the base - a stride that ignored alignment would put
	// the first row right and the rest wrong.
	size_t misaligned = 0;
	for (size_t row = 0; row < column.Size(); row++) {
		if (reinterpret_cast<uintptr_t>(column.At(row)) % 64 != 0) {
			misaligned++;
		}
	}
	REQUIRE(misaligned == 0);
	REQUIRE(static_cast<const Wide *>(column.At(count - 1))->Value == static_cast<double>(count - 1));
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

	// The source row is still constructed - moved-from, but an object - which
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
	REQUIRE(source.ChunkCount() == 0);

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
	size_t differing = 0;
	for (size_t row = 0; row < 16; row++) {
		if (std::memcmp(restored.At(row), source.At(row), sizeof(Point)) != 0) {
			differing++;
		}
	}
	REQUIRE(differing == 0);
}

TEST_CASE("a column of several chunks round-trips as one stream", "[ecs]") {
	// The bytes are the same stream a single contiguous column produced, so the
	// snapshot format did not change when the storage did. A `Write` that took
	// the whole row count from chunk zero would read past its end and a `Read`
	// that did the same would write past it.
	Column source(Components::Of<Point>());
	const size_t count = 1000;
	for (size_t index = 0; index < count; index++) {
		const Point value{static_cast<float>(index), static_cast<float>(-static_cast<int>(index))};
		source.PushCopy(&value);
	}

	ByteWriter writer;
	REQUIRE(source.Write(writer));
	REQUIRE(writer.Size() == count * sizeof(Point));

	Column restored(Components::Of<Point>());
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.Read(reader, count));
	REQUIRE(reader.AtEnd());
	REQUIRE(restored.Size() == count);
	REQUIRE(restored.ChunkCount() == Column::ChunksFor(count));

	size_t differing = 0;
	for (size_t row = 0; row < count; row++) {
		if (static_cast<const Point *>(restored.At(row))->X != static_cast<float>(row)) {
			differing++;
		}
	}
	REQUIRE(differing == 0);
}

TEST_CASE("a non-trivial type spanning chunks is destroyed exactly once per row", "[ecs]") {
	// `Destruct` takes a contiguous range, so a `Clear` that handed it the whole
	// row count from chunk zero's base would destroy garbage past the first
	// chunk and leak every string after it.
	const int before = Tracked::Live;

	{
		Column column(Components::Of<Tracked>());
		const size_t count = 300;
		for (size_t index = 0; index < count; index++) {
			const Tracked value(std::string("row-") + std::to_string(index));
			column.PushCopy(&value);
		}
		REQUIRE(column.ChunkCount() > 4);
		REQUIRE(Tracked::Live == before + static_cast<int>(count));
	}

	REQUIRE(Tracked::Live == before);
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
