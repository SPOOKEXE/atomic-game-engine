#include "ChunkPool.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/ecs/SparseSet.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/ecs/Time.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <sys/wait.h>
#include <unistd.h>

namespace {
	thread_local int FailAfter = -1;
	thread_local bool FailureTriggered = false;

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

extern "C" void *__wrap__Znwm(std::size_t size) {
	FailIfRequested();
	return __real__Znwm(size);
}

extern "C" void *__wrap__ZnwmSt11align_val_t(std::size_t size, std::align_val_t alignment) {
	FailIfRequested();
	return __real__ZnwmSt11align_val_t(size, alignment);
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
