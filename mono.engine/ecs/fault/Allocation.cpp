#include "ChunkPool.hpp"
#include "StoreState.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Schema.hpp>
#include <engine/ecs/SparseSet.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/ecs/Time.hpp>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
	thread_local int FailAfter = -1;
	thread_local bool FailureTriggered = false;
	thread_local bool TrackSchemaCopy = false;
	thread_local std::array<void *, 8> SchemaAllocations{};
	thread_local std::array<bool, 8> SchemaReleased{};
	thread_local size_t SchemaAllocationCount = 0;

	void TrackSchemaFree(void *pointer) {
		if (!TrackSchemaCopy) return;
		for (size_t i = 0; i < SchemaAllocationCount; ++i)
			if (SchemaAllocations[i] == pointer) SchemaReleased[i] = true;
	}

	void FailIfRequested() {
		if (FailAfter == 0) {
			FailureTriggered = true;
			throw std::bad_alloc();
		}
		if (FailAfter > 0) --FailAfter;
	}

	struct Observed {
		int Value = 0;
	};

	struct Tracked {
		static constexpr uint32_t MAGIC = 0xa710c123;
		static inline int ThrowAfter = -1;
		static inline int InvalidDestructions = 0;
		uint32_t Magic = 0;
		uint32_t Value = 0;

		Tracked() {
			if (ThrowAfter == 0) throw std::bad_alloc();
			if (ThrowAfter > 0) --ThrowAfter;
			Magic = MAGIC;
		}
		Tracked(const Tracked &other) : Magic(MAGIC), Value(other.Value) {}
		~Tracked() {
			if (Magic != MAGIC) ++InvalidDestructions;
		}
	};

	void WriteTracked(engine::core::ByteWriter &writer, const void *data, size_t count) {
		const auto *values = static_cast<const Tracked *>(data);
		for (size_t i = 0; i < count; ++i)
			writer.WriteUInt32(values[i].Value);
	}

	void ReadTracked(engine::core::ByteReader &reader, void *data, size_t count) {
		auto *values = static_cast<Tracked *>(data);
		for (size_t i = 0; i < count; ++i)
			values[i].Value = reader.ReadUInt32();
	}
}

extern "C" void *__real__Znwm(std::size_t);
extern "C" void *__real__ZnwmSt11align_val_t(std::size_t, std::align_val_t);
extern "C" void __real__ZdlPv(void *);
extern "C" void __real__ZdlPvm(void *, std::size_t);

extern "C" void *__wrap__Znwm(std::size_t size) {
	FailIfRequested();
	void *pointer = __real__Znwm(size);
	if (TrackSchemaCopy && SchemaAllocationCount < SchemaAllocations.size())
		SchemaAllocations[SchemaAllocationCount++] = pointer;
	return pointer;
}

extern "C" void *__wrap__ZnwmSt11align_val_t(std::size_t size, std::align_val_t alignment) {
	FailIfRequested();
	return __real__ZnwmSt11align_val_t(size, alignment);
}

extern "C" void __wrap__ZdlPv(void *pointer) {
	TrackSchemaFree(pointer);
	__real__ZdlPv(pointer);
}

extern "C" void __wrap__ZdlPvm(void *pointer, std::size_t size) {
	TrackSchemaFree(pointer);
	__real__ZdlPvm(pointer, size);
}

namespace {
	using engine::ecs::EntityRange;
	using engine::ecs::SparseSet;

	int SparsePage(int failure, EntityRange range, bool laterPage) {
		SparseSet directory;
		const uint32_t count = laterPage ? SparseSet::FIRST_PAGE_SIZE : 0;
		for (uint32_t i = 0; i < count; ++i)
			(void)directory.Allocate(range);
		const uint32_t first = range == EntityRange::Predicted ? SparseSet::PREDICTED_BASE : 0;
		const uint32_t oldGeneration = laterPage ? directory.Generation(first) : 0;
		FailAfter = failure;
		bool refused = false;
		try {
			(void)directory.Allocate(range);
		} catch (const std::bad_alloc &) {
			refused = true;
		}
		if (!refused) {
			FailAfter = -1;
			return 10;
		}
		// Clear must work while the allocator remains exhausted.
		directory.Clear();
		FailAfter = -1;
		if (directory.Allocate(range) != first) return 2;
		if (laterPage && directory.Generation(first) == oldGeneration) return 7;
		return 0;
	}

	int ChunkRelease(int failure) {
		(void)engine::ecs::ChunkPool::RetainedBytes();
		engine::ecs::ChunkPool::Trim();
		void *chunk = ::operator new(64, std::align_val_t(16));
		FailureTriggered = false;
		FailAfter = failure;
		try {
			engine::ecs::ChunkPool::Release(chunk, 64, 16);
		} catch (const std::bad_alloc &) {
			FailAfter = -1;
			::operator delete(chunk, std::align_val_t(16));
			return 8;
		}
		FailAfter = -1;
		return FailureTriggered ? 0 : 9;
	}

	int SparseRetry(int failure, EntityRange range, bool laterPage) {
		SparseSet directory;
		const uint32_t count = laterPage ? SparseSet::FIRST_PAGE_SIZE : 0;
		for (uint32_t i = 0; i < count; ++i)
			(void)directory.Allocate(range);
		const uint32_t expected = (range == EntityRange::Predicted ? SparseSet::PREDICTED_BASE : 0) + count;
		FailAfter = failure;
		bool refused = false;
		try {
			(void)directory.Allocate(range);
		} catch (const std::bad_alloc &) {
			refused = true;
		}
		FailAfter = -1;
		if (!refused) return 10;
		return directory.Allocate(range) == expected ? 0 : 3;
	}

	int Load(int failure) {
		engine::ecs::Store source("source");
		const auto sourceEntity = source.Create();
		source.Set<Observed>(sourceEntity, Observed{42});
		engine::core::ByteWriter bytes;
		if (!source.Save(bytes)) return 4;

		int notifications = 0;
		engine::ecs::Store target("target");
		target.OnChanged<Observed>([&](engine::ecs::Store &, engine::ecs::Entity, const Observed &) {
			++notifications;
		});
		(void)target.Create();
		const auto old = target.Create();
		target.Set<Observed>(old, Observed{7});
		target.ClearChanges();
		engine::core::ByteReader reader(bytes.Bytes());
		engine::ecs::ChunkPool::Trim();

		FailAfter = failure;
		bool refused = false;
		try {
			(void)target.Load(reader);
		} catch (const std::bad_alloc &) {
			refused = true;
		}
		if (!refused) {
			FailAfter = -1;
			return 10;
		}
		// The failed store must remain usable with a clock even before memory returns.
		if (target.Resource<engine::ecs::WorldTime>() == nullptr) return 5;
		const bool cleared = !target.Alive(old);
		FailAfter = -1;
		const auto after = target.Create();
		target.Set<Observed>(after, Observed{9});
		(void)target.FlushSignals();
		if (notifications != 1) return 6;
		return cleared ? 11 : 0;
	}

	int ThrowingConstructor() {
		engine::ecs::Components::Register<Tracked>("ecs.test.allocation.Tracked", WriteTracked, ReadTracked);
		engine::ecs::Store source("source");
		Tracked value;
		source.Set<Tracked>(source.Create(), value);
		source.Set<Tracked>(source.Create(), value);
		engine::core::ByteWriter bytes;
		if (!source.Save(bytes)) return 25;
		engine::ecs::Store target("target");
		engine::core::ByteReader reader(bytes.Bytes());
		Tracked::InvalidDestructions = 0;
		Tracked::ThrowAfter = 1;
		bool refused = false;
		try {
			(void)target.Load(reader);
		} catch (const std::bad_alloc &) {
			refused = true;
		}
		Tracked::ThrowAfter = -1;
		return refused && Tracked::InvalidDestructions == 0 ? 0 : 26;
	}

	int ArchetypeTrimFailure() {
		using namespace engine::ecs;
		StoreState state;
		const ComponentId component = Components::Of<Observed>();
		std::vector<Entity> entities;
		entities.reserve(16);
		for (int i = 0; i < 16; ++i) {
			const Entity entity = CreateEntity(state);
			const Observed value{i};
			SetComponent(state, entity, component, &value);
			entities.push_back(entity);
		}
		const auto tableIndex = state.Directory.Locate(EntityId::Of(entities.front()).Index)->Archetype;
		Archetype &table = state.Tables[tableIndex];
		while (table.Rows() > 1 && table.Entities().capacity() < 4 * (table.Rows() - 1) + 4)
			DestroyEntity(state, entities[table.Rows() - 1]);
		if (table.Rows() < 2) return 30;
		const Entity victim = entities.front();
		const Entity moved = table.Entities().back();
		const int expected = static_cast<const Observed *>(GetComponent(state, moved, component))->Value;
		FailureTriggered = false;
		FailAfter = 0;
		bool threw = false;
		try {
			DestroyEntity(state, victim);
		} catch (const std::bad_alloc &) {
			threw = true;
		}
		FailAfter = -1;
		if (!FailureTriggered) return 31;
		if (threw) return 32;
		const auto *value = static_cast<const Observed *>(GetComponent(state, moved, component));
		if (IsEntityAlive(state, victim) || value == nullptr || value->Value != expected) return 33;
		DestroyEntity(state, moved);
		return IsEntityAlive(state, moved) ? 34 : 0;
	}

	int FreeWithoutAllocation(SparseSet &directory, uint32_t index) {
		const uint32_t generation = directory.Generation(index);
		const size_t before = directory.LiveCount();
		FailureTriggered = false;
		FailAfter = 0;
		bool threw = false;
		try {
			directory.Free(index);
		} catch (const std::bad_alloc &) {
			threw = true;
		}
		FailAfter = -1;
		if (threw) return 35;
		if (directory.Live(index) || directory.Generation(index) == generation ||
			directory.LiveCount() + 1 != before)
			return 36;
		return 0;
	}

	int FreeCapacity() {
		for (const EntityRange range : {EntityRange::Authoritative, EntityRange::Predicted}) {
			const uint32_t base = range == EntityRange::Predicted ? SparseSet::PREDICTED_BASE : 0;
			{
				SparseSet directory;
				const uint32_t first = directory.Allocate(range);
				(void)directory.Allocate(range);
				if (const int result = FreeWithoutAllocation(directory, first)) return result;
				if (directory.Allocate(range) != first) return 37;
			}
			{
				SparseSet directory;
				directory.Adopt(base, 1);
				directory.Adopt(base + 1, 1);
				if (const int result = FreeWithoutAllocation(directory, base)) return result;
				if (directory.Allocate(range) != base) return 38;
			}
			{
				SparseSet source;
				for (int i = 0; i < 4; ++i)
					(void)source.Allocate(range);
				source.Free(base + 1);
				engine::core::ByteWriter writer;
				source.WriteAllocationState(writer);
				SparseSet restored;
				for (uint32_t i = 0; i < 4; ++i)
					restored.Restore(base + i, source.Generation(base + i), source.Live(base + i));
				restored.FinishRestore(
					range == EntityRange::Authoritative ? 4 : 0, range == EntityRange::Predicted ? 4 : 0
				);
				engine::core::ByteReader reader(writer.Bytes());
				if (!restored.ReadAllocationState(reader)) return 39;
				if (const int result = FreeWithoutAllocation(restored, base)) return result;
				if (restored.Allocate(range) != base || restored.Allocate(range) != base + 1) return 40;
			}
			{
				SparseSet directory;
				for (uint32_t i = 0; i < SparseSet::FIRST_PAGE_SIZE + 2; ++i)
					(void)directory.Allocate(range);
				directory.Free(base + SparseSet::FIRST_PAGE_SIZE);
				directory.Free(base + SparseSet::FIRST_PAGE_SIZE + 1);
				if (const int result = FreeWithoutAllocation(directory, base)) return result;
				if (directory.Allocate(range) != base) return 41;
				if (directory.Allocate(range) != base + SparseSet::FIRST_PAGE_SIZE) return 42;
			}
		}
		return 0;
	}

	int FreeFallback() {
		for (const EntityRange range : {EntityRange::Authoritative, EntityRange::Predicted}) {
			const uint32_t base = range == EntityRange::Predicted ? SparseSet::PREDICTED_BASE : 0;
			SparseSet source;
			for (int i = 0; i < 8; ++i)
				(void)source.Allocate(range);
			source.Free(base);
			if (const int result = FreeWithoutAllocation(source, base + 1)) return result;
			if (!FailureTriggered) return 43;
			if (const int result = FreeWithoutAllocation(source, base + 2)) return result;
			engine::core::ByteWriter bytes;
			source.WriteAllocationState(bytes);
			SparseSet restored;
			for (uint32_t i = 0; i < 8; ++i)
				restored.Restore(base + i, source.Generation(base + i), source.Live(base + i));
			restored.FinishRestore(
				range == EntityRange::Authoritative ? 8 : 0, range == EntityRange::Predicted ? 8 : 0
			);
			engine::core::ByteReader reader(bytes.Bytes());
			if (!restored.ReadAllocationState(reader)) return 44;
			for (int i = 0; i < 8; ++i)
				if (source.Allocate(range) != restored.Allocate(range)) return 45;
			SparseSet pages;
			const uint32_t top = SparseSet::FIRST_PAGE_SIZE + SparseSet::PAGE_SIZE;
			for (uint32_t i = 0; i < top + 3; ++i)
				(void)pages.Allocate(range);
			if (const int result = FreeWithoutAllocation(pages, base)) return result;
			if (const int result = FreeWithoutAllocation(pages, base + SparseSet::FIRST_PAGE_SIZE))
				return result;
			for (uint32_t i = 0; i < 3; ++i)
				if (const int result = FreeWithoutAllocation(pages, base + top + i)) return result;
			if (pages.Allocate(range) != base + SparseSet::FIRST_PAGE_SIZE || pages.Allocate(range) != base ||
				pages.Allocate(range) != base + top)
				return 46;
		}
		return 0;
	}

	int FailedSparseAdoptGrowth() {
		for (const EntityRange range : {EntityRange::Authoritative, EntityRange::Predicted}) {
			const uint32_t base = range == EntityRange::Predicted ? SparseSet::PREDICTED_BASE : 0;
			int failures = 0, successes = 0;
			for (int failure = 0; failure < 16; ++failure) {
				SparseSet directory;
				for (int i = 0; i < 8; ++i) {
					(void)directory.Allocate(EntityRange::Authoritative);
					(void)directory.Allocate(EntityRange::Predicted);
				}
				for (const EntityRange current : {EntityRange::Authoritative, EntityRange::Predicted}) {
					const uint32_t offset = current == EntityRange::Predicted ? SparseSet::PREDICTED_BASE : 0;
					if (const int result = FreeWithoutAllocation(directory, offset + 1)) return result;
					if (const int result = FreeWithoutAllocation(directory, offset + 3)) return result;
				}
				const size_t authoritative = directory.ResidentSlots(EntityRange::Authoritative);
				const size_t predicted = directory.ResidentSlots(EntityRange::Predicted);
				const uint32_t target = base + SparseSet::FIRST_PAGE_SIZE + 5;
				bool threw = false;
				FailAfter = failure;
				try {
					directory.Adopt(target, 5);
				} catch (const std::bad_alloc &) {
					threw = true;
				}
				FailAfter = -1;
				if (!threw) {
					++successes;
					if (!directory.Live(target) || directory.LiveCount() != 13) return 47;
					continue;
				}
				++failures;
				if (directory.Live(target) || directory.LiveCount() != 12 ||
					directory.ResidentSlots(EntityRange::Authoritative) != authoritative ||
					directory.ResidentSlots(EntityRange::Predicted) != predicted)
					return 48;
				for (const EntityRange current : {EntityRange::Authoritative, EntityRange::Predicted}) {
					const uint32_t offset = current == EntityRange::Predicted ? SparseSet::PREDICTED_BASE : 0;
					if (directory.Allocate(current) != offset + 3 ||
						directory.Allocate(current) != offset + 1 ||
						directory.Allocate(current) != offset + 8)
						return 49;
				}
			}
			if (failures == 0 || successes == 0) return 50;
		}
		return 0;
	}

	int FailedSparseRebuildBoth() {
		SparseSet directory;
		for (int i = 0; i < 8; ++i) {
			(void)directory.Allocate();
			(void)directory.Allocate(EntityRange::Predicted);
		}
		for (const EntityRange range : {EntityRange::Authoritative, EntityRange::Predicted}) {
			const uint32_t base = range == EntityRange::Predicted ? SparseSet::PREDICTED_BASE : 0;
			if (const int result = FreeWithoutAllocation(directory, base + 1)) return result;
			if (const int result = FreeWithoutAllocation(directory, base + 3)) return result;
		}
		bool threw = false;
		FailAfter = 1;
		try {
			directory.FinishRestore(8, 8);
		} catch (const std::bad_alloc &) {
			threw = true;
		}
		FailAfter = -1;
		if (!threw || directory.LiveCount() != 12) return 51;
		for (const EntityRange range : {EntityRange::Authoritative, EntityRange::Predicted}) {
			const uint32_t base = range == EntityRange::Predicted ? SparseSet::PREDICTED_BASE : 0;
			if (directory.Allocate(range) != base + 3 || directory.Allocate(range) != base + 1) return 52;
		}
		return 0;
	}

	int SchemaCopyFailure() {
		using engine::ecs::FieldSpec;
		using engine::ecs::PropertyType;
		using engine::ecs::Schemas;
		const FieldSpec fields[] = {{"First", PropertyType::String}, {"Second", PropertyType::String}};
		const auto result = Schemas::Register("ecs.allocation.failure.schema", fields);
		if (!result.Id.IsValid()) return 53;
		const auto *schema = Schemas::Of(result.Id);
		const auto descriptor = engine::ecs::Components::Describe(result.Id);
		void *source = ::operator new(descriptor.Size * 2, std::align_val_t(descriptor.Alignment));
		void *target = ::operator new(descriptor.Size * 2, std::align_val_t(descriptor.Alignment));
		descriptor.DefaultConstruct(source, 2);
		for (size_t row = 0; row < 2; ++row)
			for (const auto &field : schema->Fields())
				*reinterpret_cast<std::string *>(
					static_cast<std::byte *>(source) + row * descriptor.Size + field.Offset
				) = std::string(2048, 'x');
		for (int failure = 0; failure < 4; ++failure) {
			SchemaAllocationCount = 0;
			SchemaReleased.fill(false);
			FailureTriggered = false;
			TrackSchemaCopy = true;
			FailAfter = failure;
			bool threw = false;
			try {
				descriptor.CopyConstruct(target, source, 2);
			} catch (const std::bad_alloc &) {
				threw = true;
			}
			FailAfter = -1;
			TrackSchemaCopy = false;
			if (!threw || !FailureTriggered || SchemaAllocationCount != static_cast<size_t>(failure))
				return 54;
			for (size_t i = 0; i < SchemaAllocationCount; ++i)
				if (!SchemaReleased[i]) return 55;
			for (size_t row = 0; row < 2; ++row)
				for (const auto &field : schema->Fields()) {
					const auto *value = reinterpret_cast<const std::string *>(
						static_cast<const std::byte *>(source) + row * descriptor.Size + field.Offset
					);
					if (value->size() != 2048 || (*value)[0] != 'x') return 56;
				}
		}
		descriptor.CopyConstruct(target, source, 2);
		for (size_t row = 0; row < 2; ++row)
			for (const auto &field : schema->Fields()) {
				const auto *original = reinterpret_cast<const std::string *>(
					static_cast<const std::byte *>(source) + row * descriptor.Size + field.Offset
				);
				const auto *copy = reinterpret_cast<const std::string *>(
					static_cast<const std::byte *>(target) + row * descriptor.Size + field.Offset
				);
				if (*original != *copy) return 57;
			}
		descriptor.Destruct(target, 2);
		descriptor.Destruct(source, 2);
		::operator delete(source, std::align_val_t(descriptor.Alignment));
		::operator delete(target, std::align_val_t(descriptor.Alignment));
		return 0;
	}

	int Run(int (*test)(int), int maximum) {
		int failures = 0;
		bool clearedStore = false;
		for (int point = 0; point < maximum; ++point) {
			const pid_t child = fork();
			if (child < 0) return 20;
			if (child == 0) _exit(test(point));
			int status = 0;
			while (waitpid(child, &status, 0) < 0) {
				if (errno != EINTR) return 21;
			}
			if (!WIFEXITED(status)) return 22;
			const int code = WEXITSTATUS(status);
			if (code == 10) continue;
			if (code == 11 && test == Load) {
				clearedStore = true;
				++failures;
				continue;
			}
			if (code != 0) return code;
			++failures;
		}
		if (failures == 0) return 23;
		return test != Load || clearedStore ? 0 : 24;
	}

	int InitialAuthoritative(int n) {
		return SparsePage(n, EntityRange::Authoritative, false);
	}
	int InitialPredicted(int n) {
		return SparsePage(n, EntityRange::Predicted, false);
	}
	int LaterAuthoritative(int n) {
		return SparsePage(n, EntityRange::Authoritative, true);
	}
	int RetryAuthoritative(int n) {
		return SparseRetry(n, EntityRange::Authoritative, false);
	}
	int RetryPredicted(int n) {
		return SparseRetry(n, EntityRange::Predicted, true);
	}
}

int main() {
	if (ChunkRelease(0) != 0) return 8;
	if (ThrowingConstructor() != 0) return 26;
	if (const int result = ArchetypeTrimFailure(); result != 0) {
		std::fprintf(stderr, "archetype trim fault probe failed: %d\n", result);
		return result;
	}
	if (const int result = FreeCapacity(); result != 0) {
		std::fprintf(stderr, "free capacity fault probe failed: %d\n", result);
		return result;
	}
	for (auto test : {FreeFallback, FailedSparseAdoptGrowth, FailedSparseRebuildBoth, SchemaCopyFailure}) {
		if (const int result = test()) {
			std::fprintf(stderr, "ECS fault probe failed: %d\n", result);
			return result;
		}
	}
	for (auto test :
		 {InitialAuthoritative,
		  InitialPredicted,
		  LaterAuthoritative,
		  RetryAuthoritative,
		  RetryPredicted,
		  Load}) {
		const int result = Run(test, test == Load ? 64 : 8);
		if (result != 0) {
			std::fprintf(stderr, "allocation fault probe failed: %d\n", result);
			return result;
		}
	}
	return 0;
}
