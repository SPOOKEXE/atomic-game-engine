#include "Validation.hpp"

#include <engine/computegraph/Runtime.hpp>

#include <algorithm>
#include <new>
#include <type_traits>
#include <utility>

namespace engine::computegraph {
	namespace {
		bool ValidOwner(const Owner &owner) {
			return detail::ValidName(owner.World) && owner.Runtime != 0 && owner.Generation != 0;
		}
		bool OwnerAllows(const BufferDesc &buffer, const Owner &owner) {
			return (buffer.Owner.empty() || buffer.Owner == owner.World) &&
				   (buffer.Retention != Lifetime::View || owner.View != UINT32_MAX);
		}
		Status ValidateRequest(
			const CompiledGraph &plan,
			const Request &request,
			const Owner &owner,
			uint64_t &resultBytes,
			Diagnostic &diagnostic
		) {
			if (!ValidOwner(owner) || request.Identity != owner)
				return detail::Refuse(diagnostic, Status::StaleOwner);
			const auto &buffers = plan.Definition.Buffers;
			if (request.Inputs.size() > buffers.size() || request.Outputs.size() > buffers.size())
				return detail::Refuse(diagnostic, Status::LimitExceeded);
			for (const auto &buffer : buffers)
				if (!OwnerAllows(buffer, owner))
					return detail::Refuse(diagnostic, Status::StaleOwner, buffer.Name);
			for (size_t index = 0; index < request.Inputs.size(); ++index) {
				const auto &input = request.Inputs[index];
				const auto found = detail::Find(buffers, input.Buffer);
				if (found == UINT32_MAX)
					return detail::Refuse(diagnostic, Status::UnknownBuffer, input.Buffer);
				const auto &buffer = buffers[found];
				if (!buffer.External || buffer.Type == ElementType::EngineInstance ||
					!buffer.CountBuffer.empty())
					return detail::Refuse(diagnostic, Status::AccessDenied, input.Buffer);
				if (input.Type != buffer.Type)
					return detail::Refuse(diagnostic, Status::InvalidType, input.Buffer);
				if (input.Count > buffer.Capacity ||
					uint64_t(input.Count) * buffer.Stride != input.Bytes.size())
					return detail::Refuse(diagnostic, Status::InvalidCount, input.Buffer);
				for (size_t earlier = 0; earlier < index; ++earlier)
					if (request.Inputs[earlier].Buffer == input.Buffer)
						return detail::Refuse(diagnostic, Status::DuplicateName, input.Buffer);
			}
			for (const auto &buffer : buffers) {
				if (!buffer.External || buffer.Type == ElementType::EngineInstance) continue;
				if (std::none_of(request.Inputs.begin(), request.Inputs.end(), [&](const auto &input) {
						return input.Buffer == buffer.Name;
					}))
					return detail::Refuse(diagnostic, Status::MissingProducer, buffer.Name);
			}
			resultBytes = 0;
			for (size_t index = 0; index < request.Outputs.size(); ++index) {
				const auto &output = request.Outputs[index];
				const auto found = detail::Find(buffers, output.Buffer);
				if (found == UINT32_MAX)
					return detail::Refuse(diagnostic, Status::UnknownBuffer, output.Buffer);
				const auto &buffer = buffers[found];
				if (buffer.External) return detail::Refuse(diagnostic, Status::AccessDenied, output.Buffer);
				if (output.MaximumElements > buffer.Capacity)
					return detail::Refuse(diagnostic, Status::InvalidCount, output.Buffer);
				bool written = false;
				for (const auto &pass : plan.Passes)
					for (const auto &binding : pass.Bindings)
						if (binding.Buffer == found &&
							detail::Writes(
								plan.Definition.Kernels[pass.Kernel].Ports[binding.Port].Permission
							))
							written = true;
				if (!written) return detail::Refuse(diagnostic, Status::MissingProducer, output.Buffer);
				for (size_t earlier = 0; earlier < index; ++earlier)
					if (request.Outputs[earlier].Buffer == output.Buffer)
						return detail::Refuse(diagnostic, Status::DuplicateName, output.Buffer);
				const uint64_t bytes = uint64_t(output.MaximumElements) * buffer.Stride;
				if (bytes > UINT64_MAX - resultBytes)
					return detail::Refuse(diagnostic, Status::LimitExceeded);
				resultBytes += bytes;
			}
			return Status::Ok;
		}
		struct CompletionAdmission {
			Status Code;
			Submission *Target;
		};
		CompletionAdmission ValidateCompletion(
			const Owner &owner,
			std::span<Submission> pending,
			const Completion &completion,
			Diagnostic &diagnostic
		) {
			if (completion.Identity != owner)
				return {detail::Refuse(diagnostic, Status::StaleOwner), nullptr};
			const auto found = std::find_if(pending.begin(), pending.end(), [&](const auto &pending) {
				return pending.Ticket == completion.Ticket;
			});
			if (found == pending.end()) return {detail::Refuse(diagnostic, Status::UnknownTicket), nullptr};
			if (found->Ready) return {detail::Refuse(diagnostic, Status::UnknownTicket), nullptr};
			if (completion.Code != Status::Ok) {
				if (completion.Code > Status::DeviceFailure || !completion.Outputs.empty())
					return {detail::Refuse(diagnostic, Status::InvalidCount), nullptr};
			} else {
				if (completion.Outputs.size() != found->Data.Outputs.size())
					return {detail::Refuse(diagnostic, Status::InvalidCount), nullptr};
				for (const auto &request : found->Data.Outputs) {
					const ResultBuffer *result = nullptr;
					for (const auto &output : completion.Outputs) {
						if (output.Buffer != request.Buffer) continue;
						if (result)
							return {
								detail::Refuse(diagnostic, Status::DuplicateName, output.Buffer), nullptr
							};
						result = &output;
					}
					if (!result)
						return {detail::Refuse(diagnostic, Status::UnknownBuffer, request.Buffer), nullptr};
					const auto &buffer =
						found->Plan.Definition
							.Buffers[detail::Find(found->Plan.Definition.Buffers, request.Buffer)];
					if (result->Type != buffer.Type)
						return {detail::Refuse(diagnostic, Status::InvalidType, result->Buffer), nullptr};
					if (result->Count > request.MaximumElements ||
						uint64_t(result->Count) * buffer.Stride != result->Bytes.size())
						return {detail::Refuse(diagnostic, Status::OutputOverflow, result->Buffer), nullptr};
					if (buffer.CountBuffer.empty() && result->Count != buffer.LogicalCount)
						return {detail::Refuse(diagnostic, Status::InvalidCount, result->Buffer), nullptr};
					if (!buffer.CountBuffer.empty()) {
						for (const auto &other : completion.Outputs) {
							const auto otherIndex =
								detail::Find(found->Plan.Definition.Buffers, other.Buffer);
							if (otherIndex != UINT32_MAX &&
								found->Plan.Definition.Buffers[otherIndex].CountBuffer ==
									buffer.CountBuffer &&
								other.Count != result->Count)
								return {
									detail::Refuse(diagnostic, Status::InvalidCount, other.Buffer), nullptr
								};
						}
					}
				}
			}
			return {Status::Ok, &*found};
		}
		static_assert(std::is_nothrow_move_assignable_v<Completion>);

	}

	Queue::Queue(Owner owner, RuntimeLimits limits) : Identity(std::move(owner)), Bounds(limits) {}

	Status Queue::Submit(
		const CompiledGraph &plan, const Request &request, uint64_t &ticketOut, Diagnostic &diagnostic
	) try {
		if (Pending.size() >= Bounds.MaximumPending || NextTicket == UINT64_MAX)
			return detail::Refuse(diagnostic, Status::LimitExceeded);
		if (request.Inputs.size() > plan.Definition.Buffers.size() ||
			request.Outputs.size() > plan.Definition.Buffers.size())
			return detail::Refuse(diagnostic, Status::LimitExceeded);
		uint64_t uploadBytes = 0, retainedResults = 0;
		for (const auto &pending : Pending) {
			for (const auto &input : pending.Data.Inputs)
				uploadBytes += input.Bytes.size();
			retainedResults += pending.ResultBytes;
		}
		for (const auto &input : request.Inputs) {
			if (input.Bytes.size() > Bounds.MaximumUploadBytes - uploadBytes)
				return detail::Refuse(diagnostic, Status::LimitExceeded, input.Buffer);
			uploadBytes += input.Bytes.size();
		}
		uint64_t resultBytes = 0;
		const auto status = ValidateRequest(plan, request, Identity, resultBytes, diagnostic);
		if (status != Status::Ok) return status;
		if (resultBytes > Bounds.MaximumReadbackBytes - retainedResults ||
			resultBytes > Bounds.MaximumResultBytes - retainedResults)
			return detail::Refuse(diagnostic, Status::LimitExceeded);
		Submission submission;
		submission.Ticket = NextTicket;
		submission.Data = request;
		submission.Plan = plan;
		// Counts belong to this copied submission. The installed reusable plan stays unchanged.
		for (const auto &input : request.Inputs)
			submission.Plan.Definition.Buffers[detail::Find(submission.Plan.Definition.Buffers, input.Buffer)]
				.LogicalCount = input.Count;
		for (auto &pass : submission.Plan.Passes) {
			const auto &buffer = submission.Plan.Definition.Buffers[pass.DispatchBuffer];
			if (!buffer.CountBuffer.empty()) continue;
			const auto dispatchStatus = ResolveComputeDispatch(
				buffer.LogicalCount,
				submission.Plan.Definition.Kernels[pass.Kernel].Local,
				plan.DeviceLimits,
				pass.Work,
				diagnostic
			);
			if (dispatchStatus != Status::Ok) return dispatchStatus;
		}
		submission.ResultBytes = resultBytes;
		Pending.push_back(std::move(submission));
		ticketOut = NextTicket++;
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		return detail::AllocationFailure(diagnostic);
	}

	Status Queue::Complete(const Completion &completion, Diagnostic &diagnostic) try {
		const auto admission = ValidateCompletion(Identity, Pending, completion, diagnostic);
		if (admission.Code != Status::Ok) return admission.Code;
		Completion accepted = completion;
		admission.Target->Result = std::move(accepted);
		admission.Target->Ready = true;
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		return detail::AllocationFailure(diagnostic);
	}
	Status Queue::Complete(Completion &&completion, Diagnostic &diagnostic) try {
		const auto admission = ValidateCompletion(Identity, Pending, completion, diagnostic);
		if (admission.Code != Status::Ok) return admission.Code;
		admission.Target->Result = std::move(completion);
		admission.Target->Ready = true;
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		return detail::AllocationFailure(diagnostic);
	}
	Status Queue::Take(uint64_t ticket, Completion &out, Diagnostic &diagnostic) {
		const auto found = std::find_if(Pending.begin(), Pending.end(), [&](const auto &pending) {
			return pending.Ticket == ticket;
		});
		if (found == Pending.end()) return detail::Refuse(diagnostic, Status::UnknownTicket);
		if (!found->Ready) return detail::Refuse(diagnostic, Status::NotReady);
		out = std::move(found->Result);
		Pending.erase(found);
		diagnostic = {};
		return Status::Ok;
	}
	bool Queue::Cancel(uint64_t ticket) {
		const auto found = std::find_if(Pending.begin(), Pending.end(), [&](const auto &pending) {
			return pending.Ticket == ticket;
		});
		if (found == Pending.end()) return false;
		Pending.erase(found);
		return true;
	}
	void Queue::Reset(Owner owner) {
		Pending.clear();
		Identity = std::move(owner);
	}
	std::span<const Submission> Queue::Submissions() const {
		return Pending;
	}
}
