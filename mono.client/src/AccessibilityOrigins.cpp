#include "AccessibilityOrigins.hpp"

#include <engine/gui/SemanticSnapshotCodec.hpp>

#include <atomic>
#include <limits>
#include <new>
#include <utility>

namespace client {
	namespace {
		std::atomic<uint64_t> NextNativeId{1};

		uint64_t AllocateNativeId() {
			uint64_t next = NextNativeId.load(std::memory_order_relaxed);
			while (next != std::numeric_limits<uint64_t>::max()) {
				if (NextNativeId.compare_exchange_weak(next, next + 1, std::memory_order_relaxed))
					return next;
			}
			return 0;
		}
	}

	bool AccessibilityOrigins::Publish(
		std::span<const engine::gui::SemanticNode> nodes,
		uint64_t worldEpoch,
		uint64_t frameSequence,
		bool reuseCurrent
	) {
		if (nodes.size() > 65536) return false;
		if (reuseCurrent) {
			if (worldEpoch != CurrentWorld || nodes.size() != Current.size()) return false;
			for (size_t i = 0; i < nodes.size(); ++i) {
				const auto found = Targets.find(Current[i]);
				if (found == Targets.end() || found->second.Instance != nodes[i].Instance ||
					found->second.Collector != nodes[i].Collector)
					return false;
			}
			for (const auto id : Current)
				Targets.find(id)->second.FrameSequence = frameSequence;
			return true;
		}

		std::vector<uint64_t> next;
		bool savedCurrent = false;
		try {
			next.reserve(nodes.size());
			if (!Current.empty()) {
				History.push_back(Current);
				savedCurrent = true;
			}
			Targets.reserve(Targets.size() + nodes.size());
			for (const auto &node : nodes) {
				const uint64_t id = AllocateNativeId();
				if (id == 0) throw std::bad_alloc();
				next.push_back(id);
				Targets.emplace(id, Origin{node.Instance, node.Collector, worldEpoch, frameSequence});
			}
		} catch (const std::bad_alloc &) {
			for (const uint64_t id : next)
				Targets.erase(id);
			if (savedCurrent) History.pop_back();
			return false;
		}
		Current = std::move(next);
		CurrentWorld = worldEpoch;
		while ((!History.empty() && Targets.size() > MAX_TARGETS) || History.size() > MAX_TREES) {
			for (const uint64_t id : History.front())
				Targets.erase(id);
			History.pop_front();
		}
		return true;
	}

	AccessibilityAction
	AccessibilityOrigins::Resolve(uint64_t nativeId, engine::gui::SemanticAction action) const {
		AccessibilityAction result;
		result.NativeNodeId = nativeId;
		result.Action = action;
		const auto found = Targets.find(nativeId);
		if (found == Targets.end()) {
			result.Status = AccessibilityActionStatus::Expired;
			return result;
		}
		result.Target = found->second.Instance;
		result.Collector = found->second.Collector;
		result.WorldEpoch = found->second.WorldEpoch;
		result.FrameSequence = found->second.FrameSequence;
		return result;
	}

	void AccessibilityOrigins::DiscardHistory() noexcept {
		for (const auto &tree : History)
			for (const auto id : tree)
				Targets.erase(id);
		History.clear();
	}

	void AccessibilityOrigins::Retire() noexcept {
		Targets.clear();
		Current.clear();
		History.clear();
		CurrentWorld = 0;
	}

	bool AccessibilityTree::Publish(
		const engine::gui::SemanticSnapshot &snapshot,
		uint64_t worldEpoch,
		uint64_t frameSequence,
		bool reuseProven
	) {
		constexpr size_t encodedBudget = 1024 * 1024;
		constexpr size_t decodedBudget = 64 * 1024 * 1024;
		std::vector<std::byte> encoding;
		engine::gui::SemanticSnapshot candidate;
		if (!engine::gui::EncodeSemanticSnapshot(snapshot, encodedBudget, encoding) ||
			!engine::gui::DecodeSemanticSnapshot(encoding, decodedBudget, candidate))
			return false;
		const bool sameFrame = frameSequence != 0 && frameSequence == FrameSequence;
		const bool modeChanged = ChangesMode(frameSequence);
		const bool sameMode = !modeChanged;
		const bool reuse = !candidate.Truncated && HasCompleteEncoding && WorldEpoch == worldEpoch &&
						   Encoding == encoding && sameMode && (reuseProven || sameFrame);
		if (!NodeOrigins.Publish(candidate.Nodes, worldEpoch, frameSequence, reuse)) return false;
		if (modeChanged) NodeOrigins.DiscardHistory();
		Tree = std::move(candidate);
		WorldEpoch = worldEpoch;
		FrameSequence = frameSequence;
		HasCompleteEncoding = !Tree.Truncated;
		Encoding = std::move(encoding);
		return true;
	}

	void AccessibilityTree::Retire() noexcept {
		NodeOrigins.Retire();
		Tree.Nodes.clear();
		Tree.Truncated = false;
		Encoding.clear();
		WorldEpoch = 0;
		FrameSequence = 0;
		HasCompleteEncoding = false;
	}

	void AccessibilityRequests::Push(const AccessibilityAction &action) noexcept {
		try {
			if (Pending.size() < MAX_PENDING) {
				Pending.push_back(action);
				return;
			}
		} catch (const std::bad_alloc &) {}
		if (Rejected != std::numeric_limits<uint64_t>::max()) ++Rejected;
	}

	void AccessibilityRequests::Retire() noexcept {
		for (auto &action : Pending)
			if (action.Status == AccessibilityActionStatus::Ready)
				action.Status = AccessibilityActionStatus::Expired;
	}

	std::vector<AccessibilityAction> AccessibilityRequests::Drain() {
		if (Rejected != 0) {
			AccessibilityAction overflow;
			overflow.Status = AccessibilityActionStatus::Overflow;
			overflow.RejectedCount = Rejected;
			try {
				Pending.push_back(overflow);
				Rejected = 0;
			} catch (const std::bad_alloc &) {
				// Retain the rejection count so a later drain can report it.
			}
		}
		return std::exchange(Pending, {});
	}
}
