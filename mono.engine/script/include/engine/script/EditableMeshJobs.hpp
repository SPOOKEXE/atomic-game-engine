#pragma once

// Deterministic, fork-joined preparation of script-authored mesh transactions.
//
// A request owns every byte it gives a worker. No store, entity row or VM value
// is touched off the owner thread. `Run` prepares all requests as one batch,
// joins it, then commits in ticket order on the caller. A runtime pumps the
// completions at the next script barrier, which gives yielding mesh methods a
// third deterministic resume source beside bus replies and child waiters.
//
// @tier L9 · shared

#include <engine/ecs/Entity.hpp>
#include <engine/scene/EditableMesh.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::script {

	// A per-world queue for script-authored editable mesh transactions.
	//
	// The queue owns all worker inputs and publishes only after the fork-join
	// barrier, so no VM value or world row crosses a thread boundary.
	//
	// @since v0.19
	class EditableMeshJobs {
	  public:
		// One request that reached its owner-thread commit point.
		//
		// @since v0.19
		struct Completion {
			// Submission order within this queue.
			uint64_t Ticket = 0;

			// The result of validating the target and publishing the geometry.
			scene::EditableMeshCommit Result = scene::EditableMeshCommit::Invalid;
		};

		// Takes ownership of one complete geometry value. The target's revision is
		// captured now and checked at commit, so an edit made while the request is
		// pending wins instead of being overwritten.
		uint64_t Submit(ecs::Store &store, ecs::Entity instance, scene::EditableMeshGeometry geometry);

		// Prepares the requests as one parallel batch, joins every worker, then
		// commits on the caller in ticket order. Requests submitted while their
		// completions are being resumed remain pending for the next barrier.
		void Run(ecs::Store &store);

		// Completed requests in ticket order.
		//
		// @return A view valid until the next `Run` or `ClearCompletions`.
		std::span<const Completion> Completions() const {
			return Completed;
		}

		// Releases completion records after a runtime has resumed their waiters.
		void ClearCompletions() {
			Completed.clear();
		}

		// How many owned requests will run at the next barrier.
		//
		// @return Pending request count.
		size_t PendingCount() const {
			return Pending.size();
		}

	  private:
		struct Request {
			uint64_t Ticket = 0;
			ecs::Entity Instance;
			uint32_t ExpectedRevision = 0;
			scene::EditableMeshGeometry Geometry;
			scene::PreparedEditableMesh Prepared;
		};

		uint64_t NextTicket = 0;
		std::vector<Request> Pending;
		std::vector<Completion> Completed;
	};
}
