#include "Validation.hpp"

#include <engine/computegraph/Compile.hpp>

#include <algorithm>
#include <new>
#include <utility>

namespace engine::computegraph {
	namespace {
		bool ValidLocal(const std::array<uint32_t, 3> &local, const DispatchLimits &limits) {
			uint64_t threads = 1;
			for (size_t axis = 0; axis < local.size(); ++axis) {
				if (local[axis] == 0 || local[axis] > limits.MaximumLocal[axis] ||
					threads > limits.MaximumThreads / local[axis])
					return false;
				threads *= local[axis];
			}
			return true;
		}

		Status CheckBudget(
			const Document &document,
			std::span<const KernelReflection> reflections,
			const Limits &limits,
			Diagnostic &diagnostic
		) {
			if (document.Buffers.size() > limits.MaximumBuffers ||
				document.Kernels.size() > limits.MaximumKernels ||
				document.Passes.size() > limits.MaximumPasses || reflections.size() > limits.MaximumKernels ||
				document.Buffers.size() >= UINT32_MAX || document.Kernels.size() >= UINT32_MAX ||
				document.Passes.size() >= UINT32_MAX)
				return detail::Refuse(diagnostic, Status::LimitExceeded);
			if (reflections.size() != document.Kernels.size())
				return detail::Refuse(diagnostic, Status::ReflectionMismatch);
			size_t nameBytes = 0, ports = 0, bindings = 0;
			const auto name = [&](std::string_view value, bool optional = false) {
				if (optional && value.empty()) return true;
				if (!detail::ValidName(value) || value.size() > limits.MaximumNameBytes - nameBytes)
					return false;
				nameBytes += value.size();
				return true;
			};
			for (const auto &buffer : document.Buffers)
				if (!name(buffer.Name) || !name(buffer.Owner, true) || !name(buffer.CountBuffer, true))
					return detail::Refuse(diagnostic, Status::InvalidName, buffer.Name);
			const auto kernel = [&](const auto &declaration) {
				if (!name(declaration.Name) || declaration.Ports.size() > limits.MaximumPorts - ports)
					return false;
				ports += declaration.Ports.size();
				for (const auto &port : declaration.Ports)
					if (!name(port.Name)) return false;
				return true;
			};
			for (const auto &declaration : document.Kernels)
				if (!kernel(declaration))
					return detail::Refuse(diagnostic, Status::LimitExceeded, declaration.Name);
			for (const auto &reflection : reflections)
				if (!kernel(reflection))
					return detail::Refuse(diagnostic, Status::LimitExceeded, reflection.Name);
			for (const auto &pass : document.Passes) {
				if (!name(pass.Name) || !name(pass.Kernel) || !name(pass.DispatchBuffer) ||
					pass.Bindings.size() > limits.MaximumBindings - bindings)
					return detail::Refuse(diagnostic, Status::LimitExceeded, pass.Name);
				bindings += pass.Bindings.size();
				for (const auto &binding : pass.Bindings)
					if (!name(binding.Port) || !name(binding.Buffer))
						return detail::Refuse(diagnostic, Status::InvalidName, pass.Name);
			}
			return Status::Ok;
		}

		Status CompileBuffers(
			const Document &document, const Limits &limits, CompiledGraph &plan, Diagnostic &diagnostic
		) {
			for (size_t index = 0; index < document.Buffers.size(); ++index) {
				const auto &buffer = document.Buffers[index];
				if (detail::Find(document.Buffers, buffer.Name) != index)
					return detail::Refuse(diagnostic, Status::DuplicateName, buffer.Name);
				if (!detail::ValidLayout(buffer.Type, buffer.Stride))
					return detail::Refuse(diagnostic, Status::InvalidType, buffer.Name);
				if (!detail::ValidAccess(buffer.Permission) ||
					(buffer.External && buffer.Permission != Access::Read) ||
					(buffer.Type == ElementType::EngineInstance &&
					 (!buffer.External || buffer.Name != "engineInstanceBuffer" ||
					  buffer.Permission != Access::Read)))
					return detail::Refuse(diagnostic, Status::AccessDenied, buffer.Name);
				if (buffer.Retention != Lifetime::Submission && buffer.Retention != Lifetime::World &&
					buffer.Retention != Lifetime::View)
					return detail::Refuse(diagnostic, Status::InvalidType, buffer.Name);
				if (buffer.Retention != Lifetime::Submission && buffer.Owner.empty())
					return detail::Refuse(diagnostic, Status::InvalidName, buffer.Name);
				if (buffer.LogicalCount > buffer.Capacity ||
					(!buffer.CountBuffer.empty() && buffer.LogicalCount != 0))
					return detail::Refuse(diagnostic, Status::InvalidCount, buffer.Name);
				const uint64_t bytes = uint64_t(std::max(1u, buffer.Capacity)) * buffer.Stride;
				// Caller limits may tighten, but never widen, engine payload ceilings.
				if (bytes > std::min(limits.MaximumBufferBytes, uint64_t{64u * 1024u * 1024u}) ||
					bytes > std::min(limits.MaximumTotalBytes, uint64_t{256u * 1024u * 1024u}) - plan.Bytes)
					return detail::Refuse(diagnostic, Status::LimitExceeded, buffer.Name);
				uint32_t countIndex = UINT32_MAX;
				if (!buffer.CountBuffer.empty()) {
					countIndex = detail::Find(document.Buffers, buffer.CountBuffer);
					if (countIndex == UINT32_MAX)
						return detail::Refuse(diagnostic, Status::UnknownBuffer, buffer.CountBuffer);
					const auto &count = document.Buffers[countIndex];
					if (countIndex == index || count.Type != ElementType::U32 || count.Stride != 4 ||
						count.Capacity != 1 || count.LogicalCount != 1 || !count.CountBuffer.empty() ||
						!detail::Reads(count.Permission) || count.Owner != buffer.Owner ||
						count.Retention != buffer.Retention)
						return detail::Refuse(diagnostic, Status::InvalidCount, buffer.Name);
				}
				plan.Allocations.push_back({bytes, countIndex});
				plan.Bytes += bytes;
			}
			return Status::Ok;
		}

		Status CompilePasses(
			const Document &document,
			const DispatchLimits &limits,
			CompiledGraph &plan,
			Diagnostic &diagnostic
		) {
			for (size_t passIndex = 0; passIndex < document.Passes.size(); ++passIndex) {
				const auto &pass = document.Passes[passIndex];
				if (detail::Find(document.Passes, pass.Name) != passIndex)
					return detail::Refuse(diagnostic, Status::DuplicateName, pass.Name);
				const uint32_t kernelIndex = detail::Find(document.Kernels, pass.Kernel);
				if (kernelIndex == UINT32_MAX)
					return detail::Refuse(diagnostic, Status::UnknownKernel, pass.Kernel);
				const auto &kernel = document.Kernels[kernelIndex];
				if (pass.Bindings.size() != kernel.Ports.size())
					return detail::Refuse(diagnostic, Status::ReflectionMismatch, pass.Name);
				CompiledPass work;
				work.Kernel = kernelIndex;
				work.DispatchBuffer = detail::Find(document.Buffers, pass.DispatchBuffer);
				if (work.DispatchBuffer == UINT32_MAX)
					return detail::Refuse(diagnostic, Status::UnknownBuffer, pass.DispatchBuffer);
				const auto &dispatch = document.Buffers[work.DispatchBuffer];
				const auto status = ResolveComputeDispatch(
					dispatch.CountBuffer.empty() ? dispatch.LogicalCount : dispatch.Capacity,
					kernel.Local,
					limits,
					work.Work,
					diagnostic
				);
				if (status != Status::Ok) return status;
				work.Work.DeviceCount = !dispatch.CountBuffer.empty();
				for (size_t portIndex = 0; portIndex < kernel.Ports.size(); ++portIndex) {
					const auto &port = kernel.Ports[portIndex];
					const Binding *selected = nullptr;
					for (const auto &binding : pass.Bindings) {
						if (binding.Port != port.Name) continue;
						if (selected) return detail::Refuse(diagnostic, Status::DuplicateName, port.Name);
						selected = &binding;
					}
					if (!selected) return detail::Refuse(diagnostic, Status::ReflectionMismatch, port.Name);
					const uint32_t bufferIndex = detail::Find(document.Buffers, selected->Buffer);
					if (bufferIndex == UINT32_MAX)
						return detail::Refuse(diagnostic, Status::UnknownBuffer, selected->Buffer);
					const auto &buffer = document.Buffers[bufferIndex];
					if (buffer.Type != port.Type || buffer.Stride != port.Stride ||
						plan.Allocations[bufferIndex].Bytes < port.MinimumBytes)
						return detail::Refuse(diagnostic, Status::InvalidType, buffer.Name);
					if (!detail::Allows(buffer.Permission, port.Permission))
						return detail::Refuse(diagnostic, Status::AccessDenied, buffer.Name);
					for (const auto &prior : work.Bindings)
						if (prior.Buffer == bufferIndex &&
							(detail::Writes(port.Permission) ||
							 detail::Writes(kernel.Ports[prior.Port].Permission)))
							return detail::Refuse(diagnostic, Status::AliasConflict, buffer.Name);
					work.Bindings.push_back({static_cast<uint32_t>(portIndex), bufferIndex});
				}
				const auto readsBuffer = [&](uint32_t index) {
					return std::any_of(work.Bindings.begin(), work.Bindings.end(), [&](const auto &binding) {
						return binding.Buffer == index &&
							   detail::Reads(kernel.Ports[binding.Port].Permission);
					});
				};
				// A device-count dispatch must actually bind both data and count for its shader guard.
				const bool boundDispatch =
					std::any_of(work.Bindings.begin(), work.Bindings.end(), [&](const auto &binding) {
						return binding.Buffer == work.DispatchBuffer;
					});
				if (!boundDispatch || (work.Work.DeviceCount &&
									   !readsBuffer(plan.Allocations[work.DispatchBuffer].CountBuffer)))
					return detail::Refuse(diagnostic, Status::InvalidCount, pass.Name);
				for (const auto &binding : work.Bindings) {
					const auto count = plan.Allocations[binding.Buffer].CountBuffer;
					if (count != UINT32_MAX && detail::Reads(kernel.Ports[binding.Port].Permission) &&
						!readsBuffer(count))
						return detail::Refuse(
							diagnostic, Status::InvalidCount, document.Buffers[binding.Buffer].Name
						);
				}
				plan.Passes.push_back(std::move(work));
			}
			return Status::Ok;
		}

		Status CompileTopology(
			const Document &document, const Limits &limits, CompiledGraph &plan, Diagnostic &diagnostic
		) {
			std::vector<uint32_t> writers(document.Buffers.size(), UINT32_MAX);
			for (size_t passIndex = 0; passIndex < plan.Passes.size(); ++passIndex) {
				const auto &pass = plan.Passes[passIndex];
				for (const auto &binding : pass.Bindings) {
					if (!detail::Writes(document.Kernels[pass.Kernel].Ports[binding.Port].Permission))
						continue;
					if (writers[binding.Buffer] != UINT32_MAX)
						return detail::Refuse(
							diagnostic, Status::MultipleProducers, document.Buffers[binding.Buffer].Name
						);
					writers[binding.Buffer] = static_cast<uint32_t>(passIndex);
				}
			}
			std::vector<graphcore::Dependency> edges;
			for (size_t passIndex = 0; passIndex < plan.Passes.size(); ++passIndex) {
				const auto &pass = plan.Passes[passIndex];
				for (const auto &binding : pass.Bindings) {
					if (!detail::Reads(document.Kernels[pass.Kernel].Ports[binding.Port].Permission))
						continue;
					const auto &buffer = document.Buffers[binding.Buffer];
					if (buffer.External) continue;
					const auto writer = writers[binding.Buffer];
					if (writer == UINT32_MAX || writer == passIndex)
						return detail::Refuse(diagnostic, Status::MissingProducer, buffer.Name);
					edges.push_back({writer, static_cast<uint32_t>(passIndex)});
					plan.Barriers.push_back({writer, static_cast<uint32_t>(passIndex), binding.Buffer});
				}
			}
			graphcore::Limits bounds;
			bounds.MaximumNodes = limits.MaximumPasses;
			bounds.MaximumConnections = limits.MaximumBindings;
			graphcore::Diagnostic topologyDiagnostic;
			const auto status = graphcore::CompileDependencies(
				document.Passes.size(),
				edges,
				bounds,
				plan.Topology,
				topologyDiagnostic,
				graphcore::OrderPolicy::LowestReady
			);
			if (status != graphcore::CompileStatus::Ok)
				return detail::Refuse(
					diagnostic,
					status == graphcore::CompileStatus::Cycle			   ? Status::Cycle
					: status == graphcore::CompileStatus::AllocationFailed ? Status::AllocationFailed
																		   : Status::LimitExceeded
				);
			return Status::Ok;
		}
	}

	Status ValidateComputeBindings(
		const KernelDesc &kernel,
		const KernelReflection &reflection,
		const DispatchLimits &limits,
		Diagnostic &diagnostic
	) try {
		if (kernel.Ports.size() > 512 || reflection.Ports.size() > 512)
			return detail::Refuse(diagnostic, Status::LimitExceeded, kernel.Name);
		if (!detail::ValidName(kernel.Name) || kernel.Name != reflection.Name || kernel.Version == 0 ||
			kernel.Version != reflection.Version || kernel.Local != reflection.Local ||
			kernel.Ports.size() != reflection.Ports.size() || !ValidLocal(kernel.Local, limits))
			return detail::Refuse(diagnostic, Status::ReflectionMismatch, kernel.Name);
		for (size_t index = 0; index < kernel.Ports.size(); ++index) {
			const auto &port = kernel.Ports[index];
			if (!detail::ValidName(port.Name) || !detail::ValidLayout(port.Type, port.Stride) ||
				!detail::ValidAccess(port.Permission) ||
				(port.Type == ElementType::EngineInstance && port.Permission != Access::Read))
				return detail::Refuse(diagnostic, Status::ReflectionMismatch, port.Name);
			const uint32_t reflected = detail::Find(reflection.Ports, port.Name);
			if (reflected == UINT32_MAX || reflection.Ports[reflected] != port)
				return detail::Refuse(diagnostic, Status::ReflectionMismatch, port.Name);
			uint32_t groupSize = 0;
			for (size_t otherIndex = 0; otherIndex < kernel.Ports.size(); ++otherIndex) {
				const auto &other = kernel.Ports[otherIndex];
				if (otherIndex != index &&
					(other.Name == port.Name || (other.Set == port.Set && other.Binding == port.Binding)))
					return detail::Refuse(diagnostic, Status::ReflectionMismatch, port.Name);
				if (detail::Writes(other.Permission) == detail::Writes(port.Permission)) ++groupSize;
			}
			if (port.Set != (detail::Writes(port.Permission) ? 1u : 0u) || port.Binding >= groupSize)
				return detail::Refuse(diagnostic, Status::ReflectionMismatch, port.Name);
		}
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		return detail::AllocationFailure(diagnostic);
	}

	Status ResolveComputeDispatch(
		uint32_t elements,
		const std::array<uint32_t, 3> &local,
		const DispatchLimits &limits,
		Dispatch &out,
		Diagnostic &diagnostic
	) try {
		if (!ValidLocal(local, limits) || local[1] != 1 || local[2] != 1)
			return detail::Refuse(diagnostic, Status::InvalidDispatch);
		Dispatch resolved;
		resolved.Elements = elements;
		if (elements != 0) {
			resolved.Groups = {elements / local[0] + uint32_t(elements % local[0] != 0), 1, 1};
			for (size_t axis = 0; axis < resolved.Groups.size(); ++axis)
				if (resolved.Groups[axis] > limits.MaximumGroups[axis])
					return detail::Refuse(diagnostic, Status::InvalidDispatch);
		}
		out = resolved;
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		return detail::AllocationFailure(diagnostic);
	}

	Status Compile(
		const Document &document,
		std::span<const KernelReflection> reflections,
		const Limits &limits,
		const DispatchLimits &dispatchLimits,
		CompiledGraph &out,
		Diagnostic &diagnostic
	) {
		try {
			const auto budget = CheckBudget(document, reflections, limits, diagnostic);
			if (budget != Status::Ok) return budget;
			CompiledGraph plan;
			plan.DeviceLimits = dispatchLimits;
			auto status = CompileBuffers(document, limits, plan, diagnostic);
			if (status != Status::Ok) return status;
			for (size_t index = 0; index < document.Kernels.size(); ++index) {
				const auto &kernel = document.Kernels[index];
				if (detail::Find(document.Kernels, kernel.Name) != index)
					return detail::Refuse(diagnostic, Status::DuplicateName, kernel.Name);
				const auto reflected = detail::Find(reflections, kernel.Name);
				if (reflected == UINT32_MAX)
					return detail::Refuse(diagnostic, Status::ReflectionMismatch, kernel.Name);
				status = ValidateComputeBindings(kernel, reflections[reflected], dispatchLimits, diagnostic);
				if (status != Status::Ok) return status;
			}
			status = CompilePasses(document, dispatchLimits, plan, diagnostic);
			if (status != Status::Ok) return status;
			status = CompileTopology(document, limits, plan, diagnostic);
			if (status != Status::Ok) return status;
			plan.Definition = document;
			out = std::move(plan);
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return detail::AllocationFailure(diagnostic);
		}
	}
}
