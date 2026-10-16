#pragma once

// Copied typed compute requests. Device work belongs to the renderer owner pump.
// @tier L9 · shared

#include <engine/computegraph/Program.hpp>
#include <engine/computegraph/Runtime.hpp>

#include <functional>
#include <memory>

namespace engine::script {
	// Metadata for one retained output. Bytes contain only its validated logical prefix.
	struct ComputeOutput {
		// Stable document buffer identity.
		std::string Name{};
		// Exact element layout.
		computegraph::ElementType Type = computegraph::ElementType::U32;
		// Fence-observed logical elements.
		uint32_t Count = 0;
		// Readable byte length, excluding unused capacity.
		uint64_t Bytes = 0;
	};
	// CPU-only snapshot; polling never touches a device or waits for a fence.
	struct ComputePoll {
		// True for successful, failed and canceled terminal operations.
		bool Ready = false;
		// Cancellation revokes publication before resource retirement.
		bool Cancelled = false;
		// Terminal execution or admission status.
		computegraph::Status Code = computegraph::Status::Ok;
		// Bounded host diagnostic.
		std::string Detail{};
		// Present only for successful submissions.
		std::vector<ComputeOutput> Outputs{};
	};
	// One host-bound VM endpoint. No caller can select another owner or generation.
	class ComputeBridge {
	  public:
		virtual ~ComputeBridge() = default;
		// Fixed host world/view and actual VM/storage identities.
		virtual const computegraph::Owner &Identity() const = 0;
		// Queues compilation of a named reusable graph; refusal preserves installed state.
		virtual computegraph::Status
		Define(std::string name, computegraph::Program program, uint64_t &ticket, std::string &detail) = 0;
		// Copies typed inputs and reserves bounded outputs until terminal release.
		virtual computegraph::Status
		Submit(std::string name, computegraph::Request request, uint64_t &ticket, std::string &detail) = 0;
		// Copies the latest CPU snapshot without running renderer work.
		virtual computegraph::Status Poll(uint64_t ticket, ComputePoll &out) = 0;
		// Copies at most 64 KiB from a successful retained output.
		virtual computegraph::Status Read(
			uint64_t ticket,
			std::string_view name,
			uint64_t offset,
			size_t maximum,
			std::vector<std::byte> &out
		) = 0;
		// Invalidates pending publication; submitted allocations stay charged through their fence.
		virtual bool Cancel(uint64_t ticket) = 0;
		// Drops a terminal ticket. Canceled device work still retires on the owner thread.
		virtual bool Release(uint64_t ticket) = 0;
		// Idempotently revokes publication without waiting or touching device resources.
		virtual void Detach() = 0;
	};
	// Runtime calls this once with its actual VM identity and ECS storage incarnation.
	using ComputeBridgeFactory = std::function<std::shared_ptr<ComputeBridge>(uint64_t, uint64_t)>;
	// Host creates a fixed world/view factory before entering its script world.
	using ComputeWorldBridgeFactory = std::function<ComputeBridgeFactory(std::string)>;
}
