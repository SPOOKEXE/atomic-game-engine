#pragma once

// The identity and ownership rules for graph resources retained between views.

#include <engine/graph/Schedule.hpp>
#include <engine/render/PresentationDamage.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>

namespace engine::render {

	inline uint64_t GraphHistorySignature(
		uint64_t contentSignature, const scene::CameraMatrices &matrices, uint32_t width, uint32_t height
	) {
		uint64_t signature = contentSignature;
		for (const glm::mat4 *matrix : {&matrices.ViewProjection, &matrices.Projection})
			for (size_t column = 0; column < 4; column++)
				for (size_t row = 0; row < 4; row++)
					signature =
						scene::MixSignature(signature, std::bit_cast<uint32_t>((*matrix)[column][row]));
		signature = scene::MixSignature(signature, width);
		return scene::MixSignature(signature, height);
	}

	// ContentSignature deliberately omits these changing scene inputs.
	inline bool GraphHistoryReadable(const PresentationDamage &damage) {
		return !damage.Scene && !damage.Objects && !damage.Environment && !damage.Viewport && !damage.Portals;
	}

	// History ownership is an allocation concern. Both graph-owned and external
	// history inputs must reject a generation that was not completed for this view.
	inline bool GraphHistoryReadNeedsValidation(const graph::ResourceDesc &resource, bool make) {
		return !make && resource.Lifetime == graph::ResourceLifetime::History;
	}

	// The graph may chain two history writers in one command buffer. A completed
	// prior generation is temporal history, while a scheduled earlier producer is
	// the current graph result and does not depend on presentation damage.
	enum class GraphHistoryReadSource : uint8_t { CurrentProducer, PreviousGeneration, Unavailable };

	inline GraphHistoryReadSource
	SelectGraphHistoryRead(bool currentProducer, const PresentationDamage &damage) {
		if (currentProducer) return GraphHistoryReadSource::CurrentProducer;
		return GraphHistoryReadable(damage) ? GraphHistoryReadSource::PreviousGeneration
											: GraphHistoryReadSource::Unavailable;
	}

	// A queued producer is visible to later commands in this graph invocation.
	// An unqueued producer is visible only inside the command buffer recording it.
	inline bool GraphHistoryCurrentProducer(bool pending, bool sameCommand, bool submitted) {
		return submitted || (pending && sameCommand);
	}

	// A renderer-side cache may avoid recording a deterministic producer only
	// after the command that produced it entered the queue. Pending generations
	// never replace the last completed one, so cancellation cannot certify pixels
	// that were merely recorded.
	struct GraphHistoryGeneration {
		uint64_t Signature = 0;
		uint64_t PendingSignature = 0;
		const void *PendingCommand = nullptr;
		bool Ready = false;
		bool Pending = false;

		bool Matches(uint64_t candidate, const void *command) const {
			return (Ready && Signature == candidate) ||
				   (Pending && PendingCommand == command && PendingSignature == candidate);
		}

		uint64_t SignatureFor(const void *command) const {
			if (Pending && PendingCommand == command) return PendingSignature;
			return Ready ? Signature : 0;
		}

		void Stage(uint64_t candidate, const void *command) {
			PendingSignature = candidate;
			PendingCommand = command;
			Pending = true;
		}

		void Commit(const void *command) {
			if (!Pending || PendingCommand != command) return;
			Signature = PendingSignature;
			Ready = true;
			Pending = false;
			PendingCommand = nullptr;
		}

		void Discard(const void *command = nullptr) {
			if (!Pending || (command != nullptr && PendingCommand != command)) return;
			Pending = false;
			PendingCommand = nullptr;
		}
	};

	inline uint64_t GraphHistoryOwner(graph::NodeScope scope, size_t view, uint64_t world) {
		return scope == graph::NodeScope::View	  ? static_cast<uint64_t>(view)
			   : scope == graph::NodeScope::World ? world
												  : 0;
	}
}
