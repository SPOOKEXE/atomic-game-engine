#pragma once
#include "NodeExecutors.hpp"
#include "SourcePathShiftKey.hpp"

namespace engine::imagegraph::detail {
	struct SourcePathShiftSample {
		double X = 0, Y = 0, Weight = 1;
	};
	// This journal exists only for one EvaluateGraph call. It owns no source geometry.
	struct SourcePathShiftMemo {
		struct Owner {
			std::string NodeId, Port, Route;
		};
		struct Entry {
			uint64_t OwnerId = 0;
			size_t Line = 0;
			SourcePathShiftKey Ratio;
			SourcePathShiftSample Point;
		};
		AllocationReservation Charge;
		std::vector<Owner> Owners;
		std::vector<Entry> Entries;
		uint64_t LookupWork = 0;
		bool ValidationProbe = false;
		static constexpr uint64_t MAXIMUM_LOOKUP_WORK = 1u << 24;

		template <class T> bool Grow(NodeContext &context, std::vector<T> &values) {
			if (values.size() != values.capacity()) return true;
			const size_t next =
				std::min(Limits::MaximumArrayElements, std::max<size_t>(1, values.capacity() * 2));
			if (values.size() == next)
				return context.Fail(
					Status::LimitExceeded,
					"Source Shift memo exceeds its bounded sample or identity slots",
					"path"
				);
			auto admission = context.ReserveWorkspace(next * sizeof(T), "path");
			if (!admission) return false;
			const size_t old = values.capacity();
			std::vector<T> replacement;
			replacement.reserve(next);
			if (replacement.capacity() != next)
				return context.Fail(
					Status::LimitExceeded, "Source Shift memo vector capacity exceeds admitted slots", "path"
				);
			for (auto &value : values)
				replacement.push_back(std::move(value));
			values.swap(replacement);
			replacement.clear();
			std::vector<T>{}.swap(replacement);
			if (!Charge.Merge(std::move(*admission))) std::terminate();
			if (old) {
				auto release = Charge.Split(old * sizeof(T));
				if (!release) std::terminate();
			}
			return true;
		}
		uint64_t OwnerId(NodeContext &context, std::string_view route, std::string_view port) {
			for (size_t i = 0; i < Owners.size(); ++i) {
				if (++LookupWork > MAXIMUM_LOOKUP_WORK) {
					context.Fail(
						Status::LimitExceeded, "Source Shift memo lookup work exceeds bounds", "path"
					);
					return 0;
				}
				if (Owners[i].NodeId == context.Authored.Id && Owners[i].Port == port &&
					Owners[i].Route == route)
					return i + 1;
			}
			if (!Grow(context, Owners)) return 0;
			const uint64_t bytes = std::max(route.size(), std::string{}.capacity()) +
								   std::max(port.size(), std::string{}.capacity()) +
								   std::max(context.Authored.Id.size(), std::string{}.capacity());
			auto admission = context.ReserveWorkspace(bytes, "path");
			if (!admission) return 0;
			Owner identity{context.Authored.Id, std::string(port), std::string(route)};
			if (identity.NodeId.capacity() + identity.Port.capacity() + identity.Route.capacity() > bytes) {
				context.Fail(
					Status::LimitExceeded, "Source Shift memo identity capacity exceeds admission", "path"
				);
				return 0;
			}
			Owners.push_back(std::move(identity));
			if (!Charge.Merge(std::move(*admission))) std::terminate();
			return Owners.size();
		}
		const Entry *Find(NodeContext &context, uint64_t owner, const SourcePathShiftKey &key, size_t line) {
			if (!owner || owner > Owners.size()) {
				context.Fail(
					Status::InvalidValue, "Source Shift sample lacks its current evaluation identity", "path"
				);
				return nullptr;
			}
			for (const auto &entry : Entries) {
				if (++LookupWork > MAXIMUM_LOOKUP_WORK) {
					context.Fail(
						Status::LimitExceeded, "Source Shift memo lookup work exceeds bounds", "path"
					);
					return nullptr;
				}
				if (entry.OwnerId == owner && entry.Line == line && entry.Ratio == key) return &entry;
			}
			return nullptr;
		}
		bool Store(
			NodeContext &context,
			uint64_t owner,
			const SourcePathShiftKey &key,
			size_t line,
			SourcePathShiftSample point
		) {
			if (!Grow(context, Entries)) return false;
			Entries.push_back({owner, line, key, point});
			return true;
		}
	};
	// Native finite-validation probes are not source path sampling events.
	struct SourcePathShiftValidationScope {
		SourcePathShiftMemo *Memo = nullptr;
		bool Previous = false;
		explicit SourcePathShiftValidationScope(NodeContext &context, bool suppress = true)
			: Memo(suppress ? context.PathShiftMemo : nullptr) {
			if (Memo) {
				Previous = Memo->ValidationProbe;
				Memo->ValidationProbe = true;
			}
		}
		~SourcePathShiftValidationScope() {
			if (Memo) Memo->ValidationProbe = Previous;
		}
		SourcePathShiftValidationScope(const SourcePathShiftValidationScope &) = delete;
		SourcePathShiftValidationScope &operator=(const SourcePathShiftValidationScope &) = delete;
	};
	// Root evaluator stamps authored inputs before row selection and strips every external receipt.
	bool StampSourcePathShiftInputs(NodeContext &context);
	bool StampSourcePathShiftProducedValues(NodeContext &context);
	bool StampSourcePathShiftOutput(NodeContext &context, Path2D &path);
	void StripSourcePathShiftIdentities(Value &value);
	void StripSourcePathShiftIdentities(Node &node);
	bool StampSourcePathShiftHostOutput(NodeContext &context, AuthoredValue &value);
}
