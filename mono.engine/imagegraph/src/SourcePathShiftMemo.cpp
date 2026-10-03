#include "SourcePathShiftVisit.hpp"
namespace engine::imagegraph::detail {
	namespace {
		bool Route(
			NodeContext &context, SourcePathShiftRoute &route, std::string_view port, char kind, size_t row
		) {
			(void)port;
			if (!route.Index(kind, row))
				return context.Fail(
					Status::LimitExceeded, "Source Shift owner route exceeds bounded text", "path"
				);
			return true;
		}
		bool
		Stamp(NodeContext &context, Value &value, std::string_view port, bool trusted, bool output = false) {
			SourcePathShiftRoute route;
			if (!Route(context, route, port, output ? 'h' : 'a', output ? context.ProcessorRow : 0))
				return false;
			auto callback = [&](SourcePathData2D &op, const SourcePathShiftRoute &identity) {
				if (trusted && op.EvaluationMemoId &&
					op.EvaluationMemoId <= context.PathShiftMemo->Owners.size())
					return true;
				op.EvaluationMemoId = context.PathShiftMemo->OwnerId(context, identity.View(), port);
				return op.EvaluationMemoId != 0;
			};
			return std::visit([&](auto &leaf) { return VisitSourcePathShift(leaf, route, callback); }, value);
		}
	}
	bool StampSourcePathShiftInputs(NodeContext &context) {
		if (!context.PathShiftMemo) return true;
		for (auto &[port, value] : context.Values) {
			const bool linked = std::find(context.LinkedValues.begin(), context.LinkedValues.end(), port) !=
								context.LinkedValues.end();
			if (!Stamp(context, value, port, linked))
				return context.FailureCode == Status::Ok
						   ? context.Fail(
								 Status::LimitExceeded,
								 "Source Shift input identity traversal exceeds bounds",
								 port
							 )
						   : false;
		}
		const auto contains = [](const Value &value) {
			SourcePathShiftRoute route;
			bool found = false;
			auto callback = [&](const SourcePathData2D &, const SourcePathShiftRoute &) {
				found = true;
				return true;
			};
			std::visit([&](const auto &leaf) { return VisitSourcePathShift(leaf, route, callback); }, value);
			return found;
		};
		uint64_t bytes = 0;
		size_t count = 0;
		for (const auto &[port, value] : context.ValueViews) {
			if (!value || context.IsLinked(port) || !contains(*value)) continue;
			const auto clone = ValueClonePayloadBytes(*value);
			if (!clone ||
				bytes > Limits::MaximumEvaluationBytes - sizeof(std::pair<std::string_view, Value>) ||
				*clone > Limits::MaximumEvaluationBytes - bytes - sizeof(std::pair<std::string_view, Value>))
				return context.Fail(
					Status::LimitExceeded, "Source Shift authored view clone exceeds bounds", port
				);
			bytes += *clone + sizeof(std::pair<std::string_view, Value>);
			++count;
		}
		if (count) {
			auto charge = context.ReserveWorkspace(bytes, "path");
			if (!charge) return false;
			context.PathMemoInputValues.reserve(count);
			if (context.PathMemoInputValues.capacity() != count)
				return context.Fail(
					Status::LimitExceeded, "Source Shift authored view capacity exceeds admission", "path"
				);
			if (!context.PathMemoInputsCharge.Merge(std::move(*charge))) std::terminate();
			for (auto &[port, value] : context.ValueViews) {
				if (!value || context.IsLinked(port) || !contains(*value)) continue;
				context.PathMemoInputValues.emplace_back(port, *value);
				auto &owned = context.PathMemoInputValues.back().second;
				if (!Stamp(context, owned, port, false)) return false;
				value = &owned;
			}
		}
		return true;
	}
	bool StampSourcePathShiftProducedValues(NodeContext &context) {
		if (!context.PathShiftMemo) return true;
		for (auto &output : context.OutputValues)
			if (!Stamp(context, output.Data, output.Port, true, true))
				return context.FailureCode == Status::Ok
						   ? context.Fail(
								 Status::LimitExceeded,
								 "Source Shift producer identity traversal exceeds bounds",
								 output.Port
							 )
						   : false;
		return true;
	}
	bool StampSourcePathShiftOutput(NodeContext &context, Path2D &path) {
		if (!context.PathShiftMemo)
			return context.Fail(
				Status::UnsupportedExecution, "Source Shift requires an evaluation-owned memo journal", "path"
			);
		SourcePathShiftRoute route;
		if (!Route(context, route, "path", 'o', context.ProcessorRow)) return false;
		path.SourceOperation->EvaluationMemoId =
			context.PathShiftMemo->OwnerId(context, route.View(), "path");
		return path.SourceOperation->EvaluationMemoId != 0;
	}
	bool StampSourcePathShiftHostOutput(NodeContext &context, AuthoredValue &value) {
		return !context.PathShiftMemo || Stamp(context, value.Data, value.Port, false, true);
	}
	void StripSourcePathShiftIdentities(Node &node) {
		for (auto &value : node.Values)
			StripSourcePathShiftIdentities(value.Data);
		for (auto &value : node.SourceProperties)
			StripSourcePathShiftIdentities(value.Data);
		for (auto &input : node.DynamicInputs)
			if (input.Default) StripSourcePathShiftIdentities(*input.Default);
	}
	void StripSourcePathShiftIdentities(Value &value) {
		SourcePathShiftRoute route;
		auto callback = [](SourcePathData2D &op, const SourcePathShiftRoute &) {
			op.EvaluationMemoId = 0;
			return true;
		};
		const bool valid =
			std::visit([&](auto &leaf) { return VisitSourcePathShift(leaf, route, callback); }, value);
		if (!valid) std::terminate();
	}
}
