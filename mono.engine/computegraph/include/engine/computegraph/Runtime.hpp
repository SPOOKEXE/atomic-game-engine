#pragma once

// Bounded copied submissions and completion admission, independent of a device.
// Rendering hosts observe fences before Complete; this queue performs no execution.
// @tier L3 · shared

#include <engine/computegraph/Compile.hpp>

#include <span>

namespace engine::computegraph {
	// Actual host-supplied world identity plus runtime/generation/view ownership.
	struct Owner {
		// Stable world name copied from the hosting world, never a process-local Name ID.
		std::string World{};
		// Nonzero runtime identity and generation assigned by the host.
		uint64_t Runtime = 0;
		// Host generation used to reject work from an earlier owner lifetime.
		uint64_t Generation = 0;
		// View slot, or UINT32_MAX for world-wide work.
		uint32_t View = UINT32_MAX;
		// Exact lifetime identity for stale completion checks.
		bool operator==(const Owner &) const = default;
	};
	// Owned upload bytes. Empty input has count zero and no uploaded sentinel bytes.
	struct Upload {
		// External buffer identity and exact element layout.
		std::string Buffer{};
		// Element layout of the uploaded buffer.
		ElementType Type = ElementType::U32;
		// Current host-known logical count and exactly Count * stride bytes.
		uint32_t Count = 0;
		// Exactly Count elements encoded using the declared buffer stride.
		std::vector<std::byte> Bytes{};
	};
	// Explicit optional bounded host retrieval; GPU-to-GPU consumers need no readback.
	struct Readback {
		// Output buffer name and largest result prefix admitted for this request.
		std::string Buffer{};
		// Maximum number of elements the host may return.
		uint32_t MaximumElements = 0;
	};
	// One fully owned submission payload. VM values and pointers cannot appear here.
	struct Request {
		// Exact owner generation for execution and result publication.
		Owner Identity;
		// Copied named inputs and requested result prefixes.
		std::vector<Upload> Inputs{};
		// Named output prefixes to copy back after execution.
		std::vector<Readback> Outputs{};
	};
	// One returned buffer's valid prefix, after device count and error checks.
	struct ResultBuffer {
		// Named typed output and fence-observed logical count.
		std::string Buffer{};
		// Element layout of the returned bytes.
		ElementType Type = ElementType::U32;
		// Fence-observed number of valid returned elements.
		uint32_t Count = 0;
		// Exactly Count * declared stride bytes; no unused capacity is exposed.
		std::vector<std::byte> Bytes{};
	};
	// One terminal completion. Errors carry no output and preserve the consumer's last-good state.
	struct Completion {
		// Ticket and original owner generation.
		uint64_t Ticket = 0;
		// Owner generation under which this completion was produced.
		Owner Identity;
		// Host maps device failure/overflow to a terminal domain status.
		Status Code = Status::Ok;
		// Exact requested named results on success; empty on failure.
		std::vector<ResultBuffer> Outputs{};
	};
	// Independent CPU payload budgets, including ready results until Take or Cancel.
	struct RuntimeLimits {
		// Queued and ready requests share this limit.
		size_t MaximumPending = 8;
		// Total owned input bytes and reserved result bytes across retained requests.
		uint64_t MaximumUploadBytes = 64u * 1024u * 1024u;
		// Readback capacity reserved across retained requests.
		uint64_t MaximumReadbackBytes = 16u * 1024u * 1024u;
		// Completed result bytes retained until Take or Cancel.
		uint64_t MaximumResultBytes = 16u * 1024u * 1024u;
	};
	// Copied device adapter work. Plan indices never survive replacement as durable identities.
	struct Submission {
		// Queue identity, owned request and accepted immutable-by-convention plan snapshot.
		uint64_t Ticket = 0;
		// Owned request accepted for execution.
		Request Data;
		// Immutable-by-convention compiled plan snapshot accepted with the request.
		CompiledGraph Plan;
		// Maximum result storage reserved before accepting this work.
		uint64_t ResultBytes = 0;
		// Completion is present only after the rendering host calls Complete successfully.
		bool Ready = false;
		// Terminal result published after successful completion.
		Completion Result;
	};
	// Owner-thread queue. It does not own or wait on GPU resources; cancellation
	// invalidates results while the rendering adapter retires its in-flight handles.
	class Queue {
	  public:
		// Creates a queue for one host owner generation with explicit payload bounds.
		Queue(Owner owner, RuntimeLimits limits = {});
		// Publication rights belong to one queue and cannot be copied.
		Queue(const Queue &) = delete;
		Queue &operator=(const Queue &) = delete;
		// Checks every input and requested readback before copying or reserving output.
		// Refusal leaves pending work and ticketOut unchanged.
		Status Submit(
			const CompiledGraph &plan, const Request &request, uint64_t &ticketOut, Diagnostic &diagnostic
		);
		// Called only after the host observes its GPU fence and error/count buffers.
		// Invalid or stale results leave the pending submission unchanged.
		Status Complete(const Completion &completion, Diagnostic &diagnostic);
		// Validates every result before transferring ownership. Refusal preserves
		// both completion and the pending submission, including their payload bytes.
		Status Complete(Completion &&completion, Diagnostic &diagnostic);
		// Moves a ready result out and releases its upload/result reservation.
		Status Take(uint64_t ticket, Completion &out, Diagnostic &diagnostic);
		// Invalidates a request, releasing only CPU records and publication rights.
		bool Cancel(uint64_t ticket);
		// Replaces the host generation and invalidates every previous ticket.
		void Reset(Owner owner);
		// Immutable retained submissions; references expire on any mutation.
		std::span<const Submission> Submissions() const;

	  private:
		Owner Identity;
		RuntimeLimits Bounds;
		uint64_t NextTicket = 1;
		std::vector<Submission> Pending{};
	};
}
