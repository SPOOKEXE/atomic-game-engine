#include "SimulationAliases.hpp"

#include "FluidPayload.hpp"
#include "Mesh2DPayload.hpp"

namespace engine::imagegraph::detail {
	const SimulationReplayEntry *
	FindSimulationOrigin(const NodeContext &context, std::string_view origin, size_t processorRow) {
		if (origin.empty()) return nullptr;
		for (size_t index = context.PendingSimulationRows.size(); index > 0; --index) {
			const auto &entry = context.PendingSimulationRows[index - 1];
			if (entry.NodeId == origin && entry.ProcessorRow == processorRow && !entry.Drag && !entry.Cache &&
				!entry.Fluid.Data)
				return &entry;
		}
		if (!context.CurrentSimulation) return nullptr;
		for (const auto &entry : context.CurrentSimulation->Entries)
			if (entry.NodeId == origin && entry.ProcessorRow == processorRow && !entry.Drag && !entry.Cache &&
				!entry.Fluid.Data)
				return &entry;
		return nullptr;
	}
	const SimulationReplayEntry *
	FindFluidSimulationOrigin(const NodeContext &context, std::string_view origin, size_t processorRow) {
		if (origin.empty()) return nullptr;
		for (size_t index = context.PendingSimulationRows.size(); index > 0; --index) {
			const auto &entry = context.PendingSimulationRows[index - 1];
			if (entry.NodeId == origin && entry.ProcessorRow == processorRow && entry.Fluid.Data)
				return &entry;
		}
		if (context.CurrentSimulation)
			for (const auto &entry : context.CurrentSimulation->Entries)
				if (entry.NodeId == origin && entry.ProcessorRow == processorRow && entry.Fluid.Data)
					return &entry;
		return nullptr;
	}
	namespace {
		uint64_t AliasMeshBytes(const SimulationReplayEntry &entry) {
			return MeshAddBytes(
				sizeof(MeshData2D), RetainedSimulationEntryBytes(entry) - sizeof(SimulationReplayEntry)
			);
		}
		template <class T> uint64_t AliasBytes(const T &item, const NodeContext &context, size_t depth = 0) {
			if (depth > Limits::MaximumArrayDepth) {
				context.Fail(Status::LimitExceeded, "simulation alias nesting exceeds budget");
				return std::numeric_limits<uint64_t>::max();
			}
			if constexpr (std::is_same_v<T, FluidDomainValue>) {
				if (item.Data)
					if (const auto *entry = FindFluidSimulationOrigin(
							context, item.Data->OriginNodeId, item.Data->OriginProcessorRow
						))
						return FluidStorageBytes<true>(entry->Fluid);
				return 0;
			} else if constexpr (std::is_same_v<T, MeshValue2D>) {
				if (item.Data)
					if (const auto *entry = FindSimulationOrigin(
							context, item.Data->OriginNodeId, item.Data->OriginProcessorRow
						))
						return AliasMeshBytes(*entry);
				return 0;
			} else if constexpr (std::is_same_v<T, Path2D>) {
				if (!item.SourceOperation) return 0;
				uint64_t bytes = 0;
				if (item.SourceOperation->Mesh)
					if (const auto *entry = FindSimulationOrigin(
							context,
							item.SourceOperation->Mesh->OriginNodeId,
							item.SourceOperation->Mesh->OriginProcessorRow
						))
						bytes = AliasMeshBytes(*entry);
				for (const auto &path : item.SourceOperation->Inputs)
					bytes = MeshAddBytes(bytes, AliasBytes(path, context, depth + 1));
				return bytes;
			} else if constexpr (std::is_same_v<T, SourceArrayItem>) {
				return std::visit(
					[&](const auto &child) -> uint64_t {
						using C = std::decay_t<decltype(child)>;
						if constexpr (std::is_same_v<C, ElementValue>)
							return std::visit(
								[&](const auto &leaf) { return AliasBytes(leaf, context, depth + 1); }, child
							);
						else if constexpr (std::is_same_v<C, Image>)
							return 0;
						else {
							uint64_t bytes = 0;
							for (const auto &nested : child)
								bytes = MeshAddBytes(bytes, AliasBytes(nested, context, depth + 1));
							return bytes;
						}
					},
					item.Data
				);
			} else if constexpr (std::is_same_v<T, ArrayValue>) {
				uint64_t bytes = 0;
				for (const auto &leaf : item.Elements)
					bytes = MeshAddBytes(
						bytes,
						std::visit(
							[&](const auto &child) { return AliasBytes(child, context, depth + 1); }, leaf
						)
					);
				for (const auto &row : item.Nested)
					for (const auto &leaf : row)
						bytes = MeshAddBytes(
							bytes,
							std::visit(
								[&](const auto &child) { return AliasBytes(child, context, depth + 1); }, leaf
							)
						);
				for (const auto &child : item.Items)
					bytes = MeshAddBytes(bytes, AliasBytes(child, context, depth + 1));
				return bytes;
			} else if constexpr (std::is_same_v<T, ArraySelectorValue>) {
				return item.Data ? AliasBytes(item.Data->Values, context, depth + 1) : 0;
			} else if constexpr (std::is_same_v<T, PcxExpressionValue>) {
				uint64_t bytes = 0;
				if (item.Data) {
					for (const auto &instruction : item.Data->Instructions)
						bytes = MeshAddBytes(
							bytes,
							std::visit(
								[&](const auto &leaf) { return AliasBytes(leaf, context, depth + 1); },
								instruction.Literal
							)
						);
					for (const auto &[name, value] : item.Data->Bindings)
						bytes = MeshAddBytes(
							bytes,
							std::visit(
								[&](const auto &leaf) { return AliasBytes(leaf, context, depth + 1); }, value
							)
						);
				}
				return bytes;
			} else if constexpr (std::is_same_v<T, StructValue>) {
				uint64_t bytes = 0;
				if (item.Data)
					for (const auto &[name, child] : item.Data->Fields)
						bytes = MeshAddBytes(
							bytes,
							std::visit(
								[&](const auto &leaf) { return AliasBytes(leaf, context, depth + 1); }, child
							)
						);
				return bytes;
			} else
				return 0;
		}
		template <class T> void ReplaceAliases(T &item, NodeContext &context) {
			if constexpr (std::is_same_v<T, FluidDomainValue>) {
				if (item.Data)
					if (const auto *entry = FindFluidSimulationOrigin(
							context, item.Data->OriginNodeId, item.Data->OriginProcessorRow
						))
						item = entry->Fluid;
			} else if constexpr (std::is_same_v<T, MeshValue2D>) {
				const auto *entry = item.Data
										? FindSimulationOrigin(
											  context, item.Data->OriginNodeId, item.Data->OriginProcessorRow
										  )
										: nullptr;
				if (!entry) return;
				MeshValue2D replacement;
				auto &mesh = replacement.Data.emplace();
				static_cast<MeshTopology2D &>(mesh) = entry->Topology;
				mesh.Simulation = entry->State.Mesh;
				mesh.Verlet = true;
				mesh.OriginNodeId = entry->NodeId;
				mesh.OriginProcessorRow = entry->ProcessorRow;
				item = std::move(replacement);
			} else if constexpr (std::is_same_v<T, Path2D>) {
				if (!item.SourceOperation) return;
				if (item.SourceOperation->Mesh) {
					const auto *entry = FindSimulationOrigin(
						context,
						item.SourceOperation->Mesh->OriginNodeId,
						item.SourceOperation->Mesh->OriginProcessorRow
					);
					if (entry) {
						OwnedPayload3D<MeshData2D> replacement;
						auto &mesh = replacement.emplace();
						static_cast<MeshTopology2D &>(mesh) = entry->Topology;
						mesh.Simulation = entry->State.Mesh;
						mesh.Verlet = true;
						mesh.OriginNodeId = entry->NodeId;
						mesh.OriginProcessorRow = entry->ProcessorRow;
						item.SourceOperation->Mesh = std::move(replacement);
					}
				}
				for (auto &path : item.SourceOperation->Inputs)
					ReplaceAliases(path, context);
			} else if constexpr (std::is_same_v<T, SourceArrayItem>) {
				std::visit(
					[&](auto &child) {
						using C = std::decay_t<decltype(child)>;
						if constexpr (std::is_same_v<C, ElementValue>)
							std::visit([&](auto &leaf) { ReplaceAliases(leaf, context); }, child);
						else if constexpr (!std::is_same_v<C, Image>)
							for (auto &nested : child)
								ReplaceAliases(nested, context);
					},
					item.Data
				);
			} else if constexpr (std::is_same_v<T, ArrayValue>) {
				for (auto &leaf : item.Elements)
					std::visit([&](auto &child) { ReplaceAliases(child, context); }, leaf);
				for (auto &row : item.Nested)
					for (auto &leaf : row)
						std::visit([&](auto &child) { ReplaceAliases(child, context); }, leaf);
				for (auto &child : item.Items)
					ReplaceAliases(child, context);
			} else if constexpr (std::is_same_v<T, ArraySelectorValue>) {
				if (item.Data) ReplaceAliases(item.Data->Values, context);
			} else if constexpr (std::is_same_v<T, PcxExpressionValue>) {
				if (item.Data) {
					for (auto &instruction : item.Data->Instructions)
						std::visit([&](auto &leaf) { ReplaceAliases(leaf, context); }, instruction.Literal);
					for (auto &[name, value] : item.Data->Bindings)
						std::visit([&](auto &leaf) { ReplaceAliases(leaf, context); }, value);
				}
			} else if constexpr (std::is_same_v<T, StructValue>) {
				if (item.Data)
					for (auto &[name, child] : item.Data->Fields)
						std::visit([&](auto &leaf) { ReplaceAliases(leaf, context); }, child);
			}
		}
	}
	bool ResolveSimulationInputAliases(NodeContext &context) {
		if (context.PendingSimulationRows.empty() &&
			(!context.CurrentSimulation || context.CurrentSimulation->Entries.empty()))
			return true;
		const uint64_t slots = context.Values.size() + context.ValueViews.size();
		uint64_t bytes =
			slots * sizeof(std::pair<std::string_view, Value>) +
			(context.ValueViews.size() + slots) * sizeof(std::pair<std::string_view, const Value *>);
		uint64_t replacements = 0;
		const auto measure = [&](const Value &value) {
			const uint64_t extra =
				std::visit([&](const auto &item) { return AliasBytes(item, context); }, value);
			if (extra) {
				bytes = MeshAddBytes(bytes, MeshAddBytes(RetainedPayloadBytes(value), extra));
				++replacements;
			}
		};
		for (const auto &[port, value] : context.ValueViews)
			measure(*value);
		for (const auto &[port, value] : context.Values)
			measure(value);
		if (context.FailureCode != Status::Ok) return false;
		if (replacements == 0) return true;
		auto charge = context.ReserveWorkspace(bytes);
		if (!charge) return false;
		std::vector<std::pair<std::string_view, Value>> aliases;
		aliases.reserve(slots);
		std::vector<std::pair<std::string_view, const Value *>> views;
		views.reserve(context.ValueViews.size() + slots);
		views.insert(views.end(), context.ValueViews.begin(), context.ValueViews.end());
		const auto resolve = [&](std::string_view port, const Value &value) -> const Value * {
			if (std::visit([&](const auto &item) { return AliasBytes(item, context); }, value) == 0)
				return nullptr;
			aliases.emplace_back(port, value);
			auto &resolved = aliases.back().second;
			std::visit([&](auto &item) { ReplaceAliases(item, context); }, resolved);
			return &resolved;
		};
		for (auto &[port, value] : views)
			if (const Value *resolved = resolve(port, *value)) value = resolved;
		for (const auto &[port, value] : context.Values) {
			const bool overridden = std::any_of(views.begin(), views.end(), [&](const auto &entry) {
				return entry.first == port;
			});
			if (!overridden)
				if (const Value *resolved = resolve(port, value)) views.emplace_back(port, resolved);
		}
		context.SimulationAliasValues = std::move(aliases);
		context.ValueViews = std::move(views);
		context.SimulationAliasCharge = std::move(*charge);
		return true;
	}
	Status ResolveSimulationValueAliases(
		Value &value,
		const SimulationReplayState &replay,
		EvaluationBudget &budget,
		AllocationReservation &outputCharge,
		Diagnostic &diagnostic
	) {
		if (replay.Entries.empty()) return Status::Ok;
		const Node owner;
		const CatalogueEntry entry;
		const EvaluationRequest request;
		NodeContext context(owner, entry, request, budget);
		context.ByteBudget = budget.Available();
		context.CurrentSimulation = &replay;
		const auto refuse = [&](Status status, std::string message) {
			diagnostic = {status, {}, {}, std::move(message)};
			return status;
		};
		const uint64_t extra = std::visit([&](const auto &leaf) { return AliasBytes(leaf, context); }, value);
		if (context.FailureCode != Status::Ok) return refuse(context.FailureCode, context.FailureMessage);
		if (!extra) return Status::Ok;
		const uint64_t oldBytes = RetainedPayloadBytes(value);
		if (oldBytes > outputCharge.Bytes())
			return refuse(Status::InvalidValue, "simulation output payload has no matching reservation");
		auto replacementCharge = budget.Reserve(MeshAddBytes(oldBytes, extra));
		if (!replacementCharge)
			return refuse(Status::LimitExceeded, "simulation output alias overlap exceeds byte budget");
		Value replacement = value;
		std::visit([&](auto &leaf) { ReplaceAliases(leaf, context); }, replacement);
		if (!ValidRuntimeValue(replacement))
			return refuse(Status::InvalidValue, "simulation output alias payload is invalid");
		if (!replacementCharge->Resize(RetainedPayloadBytes(replacement)))
			return refuse(Status::LimitExceeded, "simulation output alias retained capacity exceeds budget");
		value = std::move(replacement);
		if (!outputCharge.Resize(outputCharge.Bytes() - oldBytes) ||
			!outputCharge.Merge(std::move(*replacementCharge)))
			std::terminate();
		return Status::Ok;
	}
	bool PublishSimulationFluidUpdate(NodeContext &context, const FluidDomainValue &domain) {
		if (!domain.Data || domain.Data->OriginNodeId.empty()) return true;
		if (!ValidFluidPayload(domain))
			return context.Fail(Status::InvalidValue, "FLIP domain state is invalid", "domain");
		const auto &data = *domain.Data;
		const uint64_t bytes = MeshAddBytes(
			sizeof(SimulationReplayEntry) + data.OriginNodeId.capacity(), FluidStorageBytes<true>(domain)
		);
		if (!context.ReserveOutput(bytes)) return false;
		SimulationReplayEntry update;
		update.NodeId = data.OriginNodeId;
		update.ProcessorRow = data.OriginProcessorRow;
		update.State.Tick = data.Tick;
		update.State.AuthoringRevision = data.AuthoringRevision;
		update.State.Initialized = true;
		update.Fluid = domain;
		context.SimulationUpdates.push_back(std::move(update));
		return true;
	}
	bool PublishSimulationMeshUpdate(NodeContext &context, const MeshValue2D &mesh) {
		if (!mesh.Data || !mesh.Data->Verlet || mesh.Data->OriginNodeId.empty()) return true;
		const auto &data = *mesh.Data;
		const uint64_t bytes = sizeof(SimulationReplayEntry) + data.OriginNodeId.capacity() +
							   MeshVectorBytes<true>(data.Simulation.Points) +
							   MeshVectorBytes<true>(data.Simulation.Edges) +
							   MeshVectorBytes<true>(data.Triangles) + MeshVectorBytes<true>(data.Quads) +
							   MeshVectorBytes<true>(data.SparseQuads);
		if (!context.ReserveOutput(bytes)) return false;
		context.SimulationUpdates.reserve(1);
		SimulationReplayEntry update;
		update.NodeId = data.OriginNodeId;
		update.ProcessorRow = data.OriginProcessorRow;
		update.State.Mesh = data.Simulation;
		update.State.Tick = context.Request.Tick;
		update.State.AuthoringRevision = context.Request.SimulationAuthoringRevision;
		update.State.Initialized = true;
		update.Topology = data;
		context.SimulationUpdates.push_back(std::move(update));
		return true;
	}
}
