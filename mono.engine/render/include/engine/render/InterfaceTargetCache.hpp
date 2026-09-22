#pragma once

// Bounded retained images for independently painted interface collector ranges.
//
// The graph retains the combined game interface image. This cache owns the
// narrower collector-range targets which may feed that image, keeping their lifetime
// and retry rules at the renderer boundary rather than in `gui::Compiled`.

#include <engine/ecs/Entity.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/DrawList.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

namespace engine::render {

	struct InterfaceTargetKey {
		ecs::Entity Collector;
		uint64_t Viewer = 0;
		uint32_t Width = 0;
		uint32_t Height = 0;
		size_t FirstCommand = 0;
		size_t CommandCount = 0;
		bool Spatial = false;

		bool operator==(const InterfaceTargetKey &) const = default;
	};

	struct InterfaceTargetLimits {
		size_t Count = 64;
		uint64_t Bytes = 64ull * 1024ull * 1024ull;
	};

	enum class InterfaceTargetWork : uint8_t {
		Skip,
		Partial,
		Full,
	};

	struct InterfaceTargetPlan {
		InterfaceTargetWork Work = InterfaceTargetWork::Full;
		std::vector<core::Rect> Damage;
	};

	// One cached image to composite at a particular paint-order range.
	struct InterfaceTargetComposite {
		void *Target = nullptr;
		gui::CollectorRange Range;
	};

	// Keeps cache baselines separate from target resources.
	//
	// A baseline advances only through `Complete(key, true)`. Failed target
	// recording therefore retries the same full or partial work even when the
	// compiler reports a cache hit on the next frame.
	class InterfaceTargetCache {
	  public:
		explicit InterfaceTargetCache(InterfaceTargetLimits limits = {});
		~InterfaceTargetCache();

		InterfaceTargetCache(const InterfaceTargetCache &) = delete;
		InterfaceTargetCache &operator=(const InterfaceTargetCache &) = delete;

		// Plans collector work from the compiler's conservative damage. The caller
		// records the target and confirms the result with `Complete`.
		InterfaceTargetPlan Begin(
			const InterfaceTargetKey &key,
			uint64_t signature,
			bool damageValid,
			std::span<const gui::Compiled::DamageRegion> damage
		);

		// Publishes a device target after it was allocated. Returns false when it
		// cannot fit within the configured count or byte budget.
		bool Attach(
			const InterfaceTargetKey &key, void *target, uint64_t bytes, std::function<void(void *)> release
		);

		// Commits successful pixels or preserves pending damage for a retry.
		void Complete(const InterfaceTargetKey &key, bool succeeded);

		// Returns ready targets in the exact paint order of collector ranges.
		std::vector<InterfaceTargetComposite>
		Composite(const gui::DrawList &list, uint64_t viewer, uint32_t width, uint32_t height) const;

		// Returns the resident target even while a newer write is pending. The
		// renderer records into that image and calls Complete only after submit.
		void *Target(const InterfaceTargetKey &key) const;

		void Clear();
		size_t TargetCount() const;
		uint64_t TargetBytes() const;

	  private:
		struct KeyHash {
			size_t operator()(const InterfaceTargetKey &key) const noexcept;
		};
		struct Entry {
			void *Target = nullptr;
			uint64_t Bytes = 0;
			std::function<void(void *)> Release;
			uint64_t Baseline = 0;
			uint64_t Pending = 0;
			uint64_t LastUse = 0;
			bool Ready = false;
			bool PendingValid = false;
			InterfaceTargetPlan PendingPlan;
		};

		using Entries = std::unordered_map<InterfaceTargetKey, Entry, KeyHash>;
		Entry *Find(const InterfaceTargetKey &key);
		const Entry *Find(const InterfaceTargetKey &key) const;
		Entry *EnsureEntry(const InterfaceTargetKey &key);
		bool MakeRoom(const InterfaceTargetKey &keep, uint64_t bytes);
		void Release(Entries::iterator entry);

		InterfaceTargetLimits Limits;
		Entries Targets;
		uint64_t Bytes = 0;
		uint64_t Use = 0;
	};
}
