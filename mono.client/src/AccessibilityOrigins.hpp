#pragma once

#include <client/Accessibility.hpp>
#include <deque>
#include <span>
#include <unordered_map>
#include <vector>

namespace client {
	// Bounded native IDs. A retired ID is never reassigned to another target.
	class AccessibilityOrigins {
	  public:
		static constexpr size_t MAX_TARGETS = 131072;
		static constexpr size_t MAX_TREES = 128;

		bool Publish(
			std::span<const engine::gui::SemanticNode> nodes,
			uint64_t worldEpoch,
			uint64_t frameSequence,
			bool reuseCurrent
		);
		AccessibilityAction Resolve(uint64_t nativeId, engine::gui::SemanticAction action) const;
		void DiscardHistory() noexcept;
		void Retire() noexcept;
		const std::vector<uint64_t> &CurrentIds() const {
			return Current;
		}

	  private:
		struct Origin {
			engine::ecs::Entity Instance;
			engine::ecs::Entity Collector;
			uint64_t WorldEpoch = 0;
			uint64_t FrameSequence = 0;
		};
		std::unordered_map<uint64_t, Origin> Targets;
		std::vector<uint64_t> Current;
		std::deque<std::vector<uint64_t>> History;
		uint64_t CurrentWorld = 0;
	};

	// Canonical displayed tree and its retained native-node origins.
	class AccessibilityTree {
	  public:
		bool Publish(
			const engine::gui::SemanticSnapshot &snapshot,
			uint64_t worldEpoch,
			uint64_t frameSequence,
			bool reuseProven
		);
		void Retire() noexcept;
		bool ChangesMode(uint64_t frameSequence) const noexcept {
			return !Encoding.empty() && (frameSequence == 0) != (FrameSequence == 0);
		}
		bool HasLegacyTree(uint64_t worldEpoch) const noexcept {
			return !Encoding.empty() && WorldEpoch == worldEpoch && FrameSequence == 0;
		}
		const engine::gui::SemanticSnapshot &Snapshot() const {
			return Tree;
		}
		const AccessibilityOrigins &Origins() const {
			return NodeOrigins;
		}

	  private:
		engine::gui::SemanticSnapshot Tree;
		AccessibilityOrigins NodeOrigins;
		std::vector<std::byte> Encoding;
		uint64_t WorldEpoch = 0;
		uint64_t FrameSequence = 0;
		bool HasCompleteEncoding = false;
	};

	// Bounded callback queue. Overflow is returned as an explicit rejection.
	class AccessibilityRequests {
	  public:
		static constexpr size_t MAX_PENDING = 1024;
		void Push(const AccessibilityAction &action) noexcept;
		void Retire() noexcept;
		std::vector<AccessibilityAction> Drain();

	  private:
		std::vector<AccessibilityAction> Pending;
		uint64_t Rejected = 0;
	};
}
