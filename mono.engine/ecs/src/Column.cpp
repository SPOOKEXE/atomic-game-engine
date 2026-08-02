#include <engine/core/Log.hpp>
#include <engine/ecs/Column.hpp>

#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

namespace engine::ecs {

	namespace {
		// The capacity a column jumps to on its first growth.
		//
		// Small, because most archetypes in a scene hold a handful of entities
		// and paying a kilobyte for each of them adds up faster than the
		// reallocations it saves. The ones that matter grow geometrically past
		// this within a few pushes.
		constexpr size_t FIRST_CAPACITY = 8;

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
		Release();
	}

	Column::Column(Column &&other) noexcept
		: ComponentType(other.ComponentType), Stride(other.Stride), Alignment(other.Alignment),
		  Trivial(other.Trivial), Storage(other.Storage), Rows(other.Rows), Capacity_(other.Capacity_) {
		other.Storage = nullptr;
		other.Rows = 0;
		other.Capacity_ = 0;
		other.ComponentType = ComponentId{};
	}

	Column &Column::operator=(Column &&other) noexcept {
		if (this == &other) {
			return *this;
		}

		Clear();
		Release();

		ComponentType = other.ComponentType;
		Stride = other.Stride;
		Alignment = other.Alignment;
		Trivial = other.Trivial;
		Storage = other.Storage;
		Rows = other.Rows;
		Capacity_ = other.Capacity_;

		other.Storage = nullptr;
		other.Rows = 0;
		other.Capacity_ = 0;
		other.ComponentType = ComponentId{};

		return *this;
	}

	const TypeDescriptor &Column::Describe() const {
		return Components::Describe(ComponentType);
	}

	void Column::Release() {
		if (Storage != nullptr) {
			::operator delete(Storage, std::align_val_t(Alignment));
			Storage = nullptr;
		}
		Capacity_ = 0;
	}

	void Column::Reallocate(size_t rows) {
		if (!HoldsBytes(Stride)) {
			// A tag column tracks a count and owns no memory, so capacity is
			// whatever it is asked for and no allocation happens.
			Capacity_ = rows;
			return;
		}

		void *replacement = ::operator new(rows * Stride, std::align_val_t(Alignment));

		if (Rows > 0) {
			if (Trivial) {
				std::memcpy(replacement, Storage, Rows * Stride);
			} else {
				const TypeDescriptor &descriptor = Describe();
				descriptor.MoveConstruct(replacement, Storage, Rows);
				descriptor.Destruct(Storage, Rows);
			}
		}

		if (Storage != nullptr) {
			::operator delete(Storage, std::align_val_t(Alignment));
		}

		Storage = replacement;
		Capacity_ = rows;
	}

	void Column::Reserve(size_t rows) {
		if (rows <= Capacity_) {
			return;
		}
		Reallocate(rows);
	}

	void Column::GrowIfFull() {
		if (Rows < Capacity_) {
			return;
		}
		Reallocate(Capacity_ == 0 ? FIRST_CAPACITY : Capacity_ * 2);
	}

	void Column::Clear() {
		if (Rows == 0) {
			return;
		}

		if (!Trivial && HoldsBytes(Stride)) {
			Describe().Destruct(Storage, Rows);
		}
		Rows = 0;
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
		if (Rows > 0) {
			descriptor.Write(writer, Storage, Rows);
		}
		return true;
	}

	bool Column::Read(core::ByteReader &reader, size_t rows) {
		Clear();

		if (!HoldsBytes(Stride)) {
			Rows = rows;
			return true;
		}

		const TypeDescriptor &descriptor = Describe();
		if (!descriptor.Serialisable) {
			reader.Fail();
			return false;
		}

		Reserve(rows);

		// Construct first, then read into constructed objects. Reading into raw
		// storage would hand a half-initialised object to a destructor if the
		// buffer turned out to be short.
		//
		// A trivially copyable type skips the construction: its reader
		// overwrites every byte anyway, and a value-initialising pass over a
		// column that is about to be memcpy'd is the whole restore cost paid
		// twice.
		if (rows > 0 && !Trivial) {
			std::memset(Storage, 0, rows * Stride);
			descriptor.DefaultConstruct(Storage, rows);
		}
		Rows = rows;

		descriptor.Read(reader, Storage, rows);
		if (reader.Failed()) {
			Clear();
			return false;
		}

		return true;
	}
}
