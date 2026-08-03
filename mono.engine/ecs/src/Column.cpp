#include "ChunkPool.hpp"

#include <engine/ecs/Column.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

namespace engine::ecs {

	namespace {
		// Whether this column stores bytes at all.
		//
		// A tag has a descriptor and an id but nothing to hold, and every path
		// that would touch memory has to notice.
		bool HoldsBytes(size_t stride) {
			return stride > 0;
		}
	}

	Column::Column(ComponentId type) : ComponentType(type) {
		const TypeDescriptor &descriptor = Components::Describe(type);
		Stride = descriptor.Size;
		Alignment = descriptor.Alignment == 0 ? 1 : descriptor.Alignment;
		Trivial = descriptor.Trivial;
	}

	Column::~Column() {
		Clear();
	}

	Column::Column(Column &&other) noexcept
		: ComponentType(other.ComponentType), Stride(other.Stride), Alignment(other.Alignment),
		  Trivial(other.Trivial), Chunks(std::move(other.Chunks)), Rows(other.Rows),
		  Capacity_(other.Capacity_) {
		other.Chunks.clear();
		other.Rows = 0;
		other.Capacity_ = 0;
		other.ComponentType = ComponentId{};
	}

	Column &Column::operator=(Column &&other) noexcept {
		if (this == &other) {
			return *this;
		}

		Clear();

		ComponentType = other.ComponentType;
		Stride = other.Stride;
		Alignment = other.Alignment;
		Trivial = other.Trivial;
		Chunks = std::move(other.Chunks);
		Rows = other.Rows;
		Capacity_ = other.Capacity_;

		other.Chunks.clear();
		other.Rows = 0;
		other.Capacity_ = 0;
		other.ComponentType = ComponentId{};

		return *this;
	}

	const TypeDescriptor &Column::Describe() const {
		return Components::Describe(ComponentType);
	}

	void Column::GrowToChunks(size_t chunks) {
		if (!HoldsBytes(Stride)) {
			return;
		}

		// Reserved up front so that a throwing directory growth cannot leave a
		// chunk acquired and unrecorded — the pool would never see it again.
		Chunks.reserve(chunks);
		while (Chunks.size() < chunks) {
			Chunks.push_back(ChunkPool::Acquire(ChunkBytes(Chunks.size()), Alignment));
		}
		Capacity_ = Chunks.empty() ? 0 : ChunkStart(Chunks.size() - 1) + ChunkRows(Chunks.size() - 1);
	}

	void Column::ReleaseChunksFrom(size_t chunks) {
		if (!HoldsBytes(Stride) || Chunks.size() <= chunks) {
			// One compare on the path a removal takes every time. Recomputing
			// the capacity below costs a branch and a shift, and paying it per
			// removal to arrive at the number it already held is the shape
			// `docs/CODE_QUALITY.md` calls out.
			return;
		}

		while (Chunks.size() > chunks) {
			ChunkPool::Release(Chunks.back(), ChunkBytes(Chunks.size() - 1), Alignment);
			Chunks.pop_back();
		}
		Capacity_ = Chunks.empty() ? 0 : ChunkStart(Chunks.size() - 1) + ChunkRows(Chunks.size() - 1);
	}

	void Column::Reserve(size_t rows) {
		if (!HoldsBytes(Stride)) {
			// A tag column tracks a count and owns no memory, so capacity is
			// whatever it is asked for and no allocation happens.
			Capacity_ = std::max(Capacity_, rows);
			return;
		}

		GrowToChunks(std::max(ChunksFor(rows), Chunks.size()));
	}

	void Column::GrowIfFull() {
		if (Rows < Capacity_) {
			return;
		}

		if (!HoldsBytes(Stride)) {
			Capacity_ = Rows + 1;
			return;
		}

		// One chunk, never a doubling. Geometric growth existed to amortise a
		// copy of everything already there, and there is no copy any more — a
		// chunk is acquired and linked, and the rows before it never move.
		GrowToChunks(Chunks.size() + 1);
	}

	void Column::Clear() {
		if (Rows > 0 && !Trivial && HoldsBytes(Stride)) {
			DestroyRows(0, Rows);
		}

		Rows = 0;

		// Every chunk, not merely the ones past the row count. A world whose
		// population oscillates is protected by the pool rather than by holding
		// the peak, which is the trade this whole item is.
		ReleaseChunksFrom(0);
		if (!HoldsBytes(Stride)) {
			Capacity_ = 0;
		}
	}

	void Column::DestroyRows(size_t from, size_t to) {
		const TypeDescriptor &descriptor = Describe();

		// Per chunk, because `Destruct` takes a count and a contiguous range and
		// a column is only contiguous inside one chunk. Handing it the whole row
		// count from chunk zero's base would destroy garbage past the first
		// chunk and leak everything after it.
		size_t row = from;
		while (row < to) {
			const size_t chunk = ChunkOf(row);
			const size_t offset = row - ChunkStart(chunk);
			const size_t count = std::min(to, ChunkLimit(row)) - row;
			descriptor.Destruct(static_cast<std::byte *>(Chunks[chunk]) + offset * Stride, count);
			row += count;
		}
	}

	size_t Column::PushDefault() {
		GrowIfFull();

		const size_t row = Rows;
		if (HoldsBytes(Stride)) {
			// Zeroed before construction rather than after, so that a type with
			// padding bytes serialises the same values on every run. Two rows
			// that compare equal but differ in padding would produce two
			// different snapshots of one world.
			void *destination = At(row);
			std::memset(destination, 0, Stride);
			Describe().DefaultConstruct(destination, 1);
		}

		Rows++;
		return row;
	}

	size_t Column::PushCopy(const void *value) {
		GrowIfFull();

		const size_t row = Rows;
		if (HoldsBytes(Stride)) {
			void *destination = At(row);
			if (Trivial) {
				std::memcpy(destination, value, Stride);
			} else {
				Describe().CopyConstruct(destination, value, 1);
			}
		}

		Rows++;
		return row;
	}

	void Column::Assign(size_t row, const void *value) {
		if (!HoldsBytes(Stride)) {
			return;
		}

		void *destination = At(row);
		if (Trivial) {
			std::memcpy(destination, value, Stride);
			return;
		}

		// Destroy and re-copy rather than copy-assign: the descriptor carries
		// construction and destruction, not assignment, and adding a fifth hook
		// to serve one call site is not worth the surface.
		const TypeDescriptor &descriptor = Describe();
		descriptor.Destruct(destination, 1);
		descriptor.CopyConstruct(destination, value, 1);
	}

	void Column::RemoveSwapBack(size_t row) {
		if (Rows == 0) {
			return;
		}

		const size_t last = Rows - 1;

		if (HoldsBytes(Stride)) {
			if (Trivial) {
				// Nothing to destroy, so removing the last row is free and
				// removing any other is one copy.
				if (row != last) {
					std::memcpy(At(row), At(last), Stride);
				}
			} else {
				const TypeDescriptor &descriptor = Describe();
				descriptor.Destruct(At(row), 1);

				if (row != last) {
					descriptor.MoveConstruct(At(row), At(last), 1);

					// A moved-from object is still an object. Destroying it is
					// what keeps the row count and the live object count in
					// step, and skipping it is a leak that only appears for a
					// component holding an allocation.
					descriptor.Destruct(At(last), 1);
				}
			}
		}

		Rows--;

		// The trailing chunk goes back the moment the rows stop reaching into
		// it. This is the release the whole item is about, and it is affordable
		// only because the pool is between here and the allocator — a population
		// oscillating across a boundary would otherwise allocate and free on
		// every oscillation.
		//
		// Guarded by a compare against the last chunk's first row rather than by
		// recomputing the chunk count: a removal that did not empty a chunk is
		// the overwhelmingly common one, and it is on the structural-change path.
		if (!Chunks.empty() && Rows <= ChunkStart(Chunks.size() - 1)) {
			ReleaseChunksFrom(ChunksFor(Rows));
		}
	}

	size_t Column::PushMovedFrom(Column &source, size_t sourceRow) {
		GrowIfFull();

		const size_t row = Rows;
		if (HoldsBytes(Stride)) {
			if (Trivial) {
				std::memcpy(At(row), source.At(sourceRow), Stride);
			} else {
				Describe().MoveConstruct(At(row), source.At(sourceRow), 1);
			}
		}

		Rows++;
		return row;
	}

	size_t Column::PushCopiedFrom(const Column &source, size_t sourceRow) {
		return PushCopy(source.At(sourceRow));
	}

	bool Column::Write(core::ByteWriter &writer) const {
		const TypeDescriptor &descriptor = Describe();

		if (!HoldsBytes(Stride)) {
			// A tag has no bytes, and writing none of them is the whole record.
			// It is still "written": the archetype's component list is what
			// says the tag was there.
			return true;
		}
		if (!descriptor.Serialisable) {
			return false;
		}

		// One call per chunk, appended in row order, so the bytes are the same
		// stream a single contiguous column produced and no snapshot format
		// changed when the storage did.
		for (size_t chunk = 0; chunk < ChunksFor(Rows); chunk++) {
			const size_t start = ChunkStart(chunk);
			descriptor.Write(writer, Chunks[chunk], std::min(Rows, start + ChunkRows(chunk)) - start);
		}
		return true;
	}

	bool Column::Read(core::ByteReader &reader, size_t rows) {
		Clear();

		if (!HoldsBytes(Stride)) {
			Rows = rows;
			Capacity_ = std::max(Capacity_, rows);
			return true;
		}

		const TypeDescriptor &descriptor = Describe();
		if (!descriptor.Serialisable) {
			reader.Fail();
			return false;
		}

		Reserve(rows);
		Rows = rows;

		// Construct first, then read into constructed objects. Reading into raw
		// storage would hand a half-initialised object to a destructor if the
		// buffer turned out to be short.
		//
		// A trivially copyable type skips the construction: its reader
		// overwrites every byte anyway, and a value-initialising pass over a
		// column that is about to be memcpy'd is the whole restore cost paid
		// twice.
		const size_t chunks = ChunksFor(rows);
		if (!Trivial) {
			for (size_t chunk = 0; chunk < chunks; chunk++) {
				const size_t start = ChunkStart(chunk);
				const size_t count = std::min(rows, start + ChunkRows(chunk)) - start;
				std::memset(Chunks[chunk], 0, count * Stride);
				descriptor.DefaultConstruct(Chunks[chunk], count);
			}
		}

		for (size_t chunk = 0; chunk < chunks; chunk++) {
			const size_t start = ChunkStart(chunk);
			descriptor.Read(reader, Chunks[chunk], std::min(rows, start + ChunkRows(chunk)) - start);
		}

		if (reader.Failed()) {
			Clear();
			return false;
		}

		return true;
	}
}
