#include <engine/core/FrameGraph.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/script/EditableMeshJobs.hpp>

#include <algorithm>
#include <utility>

namespace engine::script {

	uint64_t
	EditableMeshJobs::Submit(ecs::Store &store, ecs::Entity instance, scene::EditableMeshGeometry geometry) {
		const scene::EditableMesh *mesh = store.Get<scene::EditableMesh>(instance);
		const uint32_t revision = mesh == nullptr ? 0 : mesh->Revision;
		const uint64_t ticket = ++NextTicket;
		Pending.push_back(
			Request{
				.Ticket = ticket,
				.Instance = instance,
				.ExpectedRevision = revision,
				.Geometry = std::move(geometry),
				.Prepared = {},
			}
		);
		return ticket;
	}

	void EditableMeshJobs::Run(ecs::Store &store) {
		Completed.clear();
		if (Pending.empty()) {
			return;
		}

		ENGINE_PROFILE_CAT("editable mesh prepare", core::ProfileCategory::Script);

		// One mesh is already a substantial unit of work. Two requests cross the
		// measured job handover floor even for modest 4k-vertex terrain chunks;
		// with several worlds, Universe's coarser dispatch owns the pool and each
		// world runs this same batch inline on its assigned core.
		parallel::Jobs::For(
			Pending.size(),
			1,
			[this](size_t begin, size_t end) {
				for (size_t index = begin; index < end; index++) {
					Pending[index].Prepared = scene::PrepareEditableMesh(std::move(Pending[index].Geometry));
				}
			},
			2
		);
		const parallel::BatchTiming timing = parallel::Jobs::LastBatch();
		core::FrameGraph::Report(
			"editable mesh workers", core::ProfileCategory::Script, timing.BusyMilliseconds
		);

		std::sort(Pending.begin(), Pending.end(), [](const Request &left, const Request &right) {
			return left.Ticket < right.Ticket;
		});
		Completed.reserve(Pending.size());
		for (Request &request : Pending) {
			const scene::EditableMeshCommit result = scene::CommitEditableMesh(
				store, request.Instance, std::move(request.Prepared), request.ExpectedRevision
			);
			Completed.push_back(Completion{request.Ticket, result});
		}
		Pending.clear();
	}
}
