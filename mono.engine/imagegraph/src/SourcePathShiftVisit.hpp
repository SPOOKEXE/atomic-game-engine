#pragma once
#include "GroupReplayInternal.hpp"
#include "PixelBuilderPayload.hpp"
#include "SourcePathShiftMemo.hpp"

#include <charconv>
#include <type_traits>

namespace engine::imagegraph::detail {
	struct SourcePathShiftRoute {
		std::array<char, 4096> Bytes{};
		size_t Size = 0;
		bool Append(std::string_view text) {
			if (text.size() > Bytes.size() - Size) return false;
			std::copy(text.begin(), text.end(), Bytes.begin() + Size);
			Size += text.size();
			return true;
		}
		bool Index(char kind, size_t index) {
			if (!Append(std::string_view(&kind, 1))) return false;
			const auto end = std::to_chars(Bytes.data() + Size, Bytes.data() + Bytes.size(), index);
			if (end.ec != std::errc{}) return false;
			Size = size_t(end.ptr - Bytes.data());
			return Append("/");
		}
		std::string_view View() const {
			return {Bytes.data(), Size};
		}
	};
	template <class T, class Fn>
	bool VisitSourcePathShift(T &value, SourcePathShiftRoute &route, Fn &callback, size_t depth = 0) {
		using Leaf = std::remove_cv_t<T>;
		if (depth > 64) return false;
		const auto descend = [&](auto &child, char kind, size_t index) {
			const size_t size = route.Size;
			if (!route.Index(kind, index)) return false;
			const bool ok = VisitSourcePathShift(child, route, callback, depth + 1);
			route.Size = size;
			return ok;
		};
		const auto variant = [&](auto &child, char kind, size_t index) {
			return std::visit([&](auto &leaf) { return descend(leaf, kind, index); }, child);
		};
		if constexpr (std::is_same_v<Leaf, Node>) {
			for (size_t i = 0; i < value.Values.size(); ++i)
				if (!variant(value.Values[i].Data, 'n', i)) return false;
			for (size_t i = 0; i < value.SourceProperties.size(); ++i)
				if (!variant(value.SourceProperties[i].Data, 's', i)) return false;
			for (size_t i = 0; i < value.DynamicInputs.size(); ++i)
				if (value.DynamicInputs[i].Default && !variant(*value.DynamicInputs[i].Default, 'd', i))
					return false;
		} else if constexpr (std::is_same_v<Leaf, Document>) {
			for (size_t i = 0; i < value.Nodes.size(); ++i)
				if (!descend(value.Nodes[i], 'n', i)) return false;
			for (size_t i = 0; i < value.Keyframes.size(); ++i)
				if (!variant(value.Keyframes[i].Data, 'k', i)) return false;
			for (size_t i = 0; i < value.Junctions.size(); ++i)
				if (value.Junctions[i].Default && !variant(*value.Junctions[i].Default, 'j', i)) return false;
		} else if constexpr (std::is_same_v<Leaf, DynamicSurfaceValue>) {
			if (value.Data) {
				if (!descend(value.Data->Authored, 'd', 0)) return false;
				for (size_t i = 0; i < value.Data->PcxObservations.size(); ++i)
					if (!variant(value.Data->PcxObservations[i].Data, 'o', i)) return false;
				const auto host = [&](auto &capture, size_t i, char tag) {
					const size_t saved = route.Size;
					if (!route.Index(tag, i)) return false;
					if (!descend(capture.Authored, 'n', 0)) return false;
					for (size_t j = 0; j < capture.Inputs.size(); ++j)
						if (!variant(capture.Inputs[j].Data, 'i', j)) return false;
					if constexpr (requires { capture.Outputs; })
						for (size_t j = 0; j < capture.Outputs.size(); ++j)
							if (!variant(capture.Outputs[j].Data, 'o', j)) return false;
					route.Size = saved;
					return true;
				};
				for (size_t i = 0; i < value.Data->HostCaptures.size(); ++i)
					if (!host(value.Data->HostCaptures[i], i, 'h')) return false;
				for (size_t i = 0; i < value.Data->BuiltinRandomCaptures.size(); ++i)
					if (!host(value.Data->BuiltinRandomCaptures[i], i, 'b')) return false;
				if (value.Data->Groups)
					if (auto *groups = GroupReplayAccess::Get(value.Data->Groups->Replay)) {
						for (size_t i = 0; i < groups->Entries.size(); ++i) {
							auto &entry = groups->Entries[i];
							const size_t saved = route.Size;
							if (!route.Index('g', i)) return false;
							if (entry.ParentReset && !variant(*entry.ParentReset, 'p', 0)) return false;
							if (entry.SubtypeStatic && !variant(*entry.SubtypeStatic, 's', 0)) return false;
							for (size_t j = 0; j < entry.ParentKeys.size(); ++j)
								if (!variant(entry.ParentKeys[j].Data, 'p', j + 1)) return false;
							for (size_t j = 0; j < entry.SubtypeKeys.size(); ++j)
								if (!variant(entry.SubtypeKeys[j].Data, 's', j + 1)) return false;
							route.Size = saved;
						}
						for (size_t i = 0; i < groups->SharedSubtypes.size(); ++i) {
							auto &overlay = groups->SharedSubtypes[i];
							const size_t saved = route.Size;
							if (!route.Index('u', i)) return false;
							if (overlay.Fixed && !variant(*overlay.Fixed, 'f', 0)) return false;
							for (size_t j = 0; j < overlay.Keys.size(); ++j)
								if (!variant(overlay.Keys[j].Data, 'k', j)) return false;
							route.Size = saved;
						}
					}
				if (value.Data->DataHistory)
					for (size_t i = 0; i < value.Data->DataHistory->Entries.size(); ++i) {
						const auto saved = route.Size;
						if (!route.Index('r', i)) return false;
						for (size_t j = 0; j < value.Data->DataHistory->Entries[i].Values.size(); ++j)
							if (!variant(value.Data->DataHistory->Entries[i].Values[j].Data, 'v', j))
								return false;
						route.Size = saved;
					}
			}
		} else if constexpr (std::is_same_v<Leaf, Path2D>) {
			if (value.SourceOperation) {
				auto &op = *value.SourceOperation;
				if (op.Kind == SourcePathOperationKind::Shift && !callback(op, route)) return false;
				for (size_t i = 0; i < op.Inputs.size(); ++i)
					if (!descend(op.Inputs[i], 'p', i)) return false;
				if (op.WeightInput3D && !descend(*op.WeightInput3D, 'w', 0)) return false;
			}
		} else if constexpr (std::is_same_v<Leaf, PathData3D>) {
			if (value.Source2D && !descend(*value.Source2D, 's', 0)) return false;
			if (value.SourceOperation)
				for (size_t i = 0; i < value.SourceOperation->Inputs.size(); ++i)
					if (!descend(value.SourceOperation->Inputs[i], 't', i)) return false;
		} else if constexpr (std::is_same_v<Leaf, PathValue3D>) {
			if (value.Data) {
				if (value.Data->Source2D && !descend(*value.Data->Source2D, 's', 0)) return false;
				if (value.Data->SourceOperation)
					for (size_t i = 0; i < value.Data->SourceOperation->Inputs.size(); ++i)
						if (!descend(value.Data->SourceOperation->Inputs[i], 't', i)) return false;
			}
		} else if constexpr (std::is_same_v<Leaf, ArrayValue>) {
			for (size_t i = 0; i < value.Elements.size(); ++i)
				if (!variant(value.Elements[i], 'a', i)) return false;
			for (size_t row = 0; row < value.Nested.size(); ++row) {
				const size_t size = route.Size;
				if (!route.Index('r', row)) return false;
				for (size_t i = 0; i < value.Nested[row].size(); ++i)
					if (!variant(value.Nested[row][i], 'a', i)) return false;
				route.Size = size;
			}
			for (size_t i = 0; i < value.Items.size(); ++i)
				if (!descend(value.Items[i], 'i', i)) return false;
		} else if constexpr (std::is_same_v<Leaf, SourceArrayItem>) {
			return std::visit(
				[&](auto &item) {
					using C = std::decay_t<decltype(item)>;
					if constexpr (std::is_same_v<C, ElementValue>)
						return variant(item, 'v', 0);
					else if constexpr (std::is_same_v<C, std::vector<SourceArrayItem>>) {
						for (size_t i = 0; i < item.size(); ++i)
							if (!descend(item[i], 'i', i)) return false;
					}
					return true;
				},
				value.Data
			);
		} else if constexpr (std::is_same_v<Leaf, StructValue>) {
			if (value.Data)
				for (size_t i = 0; i < value.Data->Fields.size(); ++i)
					if (!variant(value.Data->Fields[i].second, 'f', i)) return false;
		} else if constexpr (std::is_same_v<Leaf, ArraySelectorValue>) {
			if (value.Data && !descend(value.Data->Values, 'd', 0)) return false;
		} else if constexpr (std::is_same_v<Leaf, PcxExpressionValue>) {
			if (value.Data) {
				for (size_t i = 0; i < value.Data->Instructions.size(); ++i)
					if (!variant(value.Data->Instructions[i].Literal, 'l', i)) return false;
				for (size_t i = 0; i < value.Data->Bindings.size(); ++i)
					if (!variant(value.Data->Bindings[i].second, 'b', i)) return false;
			}
		}
		return true;
	}
}
