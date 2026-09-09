#pragma once
#include <engine/render/PortalGeometry.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <string_view>

namespace engine::ecs {
	class Store;
}
namespace engine::scene {
	struct PortalSeam;
	struct SeamTransform;
}
namespace engine::render {
	struct PortalBodyDraws {
		std::vector<scene::DrawInstance> Near;
		std::vector<scene::DrawInstance> Far;
		std::vector<core::CFrame> Joints;
	};
	// Selected ordinary body rows only. The source half keeps dot(position, normal) >= offset;
	// the mapped half keeps the complement. Shared skin ranges are compacted once for both halves.
	// The caller chooses the owning body and mouth; position changes never flip the chosen side.
	// Inputs must not alias output. Invalid input leaves output unchanged.
	// Storage is reused on subsequent successful calls.
	bool SplitPortalBodyDraws(
		std::span<const scene::DrawInstance> body,
		std::span<const core::CFrame> joints,
		const scene::SeamTransform &through,
		const core::Vector3 &normal,
		float offset,
		PortalBodyDraws &out
	);

	struct PortalDrawSelection {
		std::string_view Player;
		// Indices into the appended destination draw list, for its primary eye only.
		std::vector<uint32_t> Hidden;
		size_t Appended = 0;
		size_t Replaced = 0;
		std::string_view RetainedBodyPlayer{};
		std::vector<uint32_t> RetainedBodyRows{};
	};
	// The caller authorizes the account first. Native/held rigs resolve in this
	// store; imported rows are identified while decoding their owned geometry.
	// Removing rows remaps the primary-eye hidden indices without changing its policy.
	bool RemoveRetainedPortalBody(
		const ecs::Store &,
		std::string_view player,
		std::vector<scene::DrawInstance> &,
		std::span<const uint32_t> importedRows,
		std::vector<uint32_t> &eyeHidden,
		std::string &error
	);
	// Source rows are already clipped/mapped by scene::AppendPortalClones. Convert
	// available source identities to names and compact referenced skin ranges.
	// Retired rows retain their picture; held roots can still identify their owner.
	bool EncodePortalDraws(
		ecs::Store &source,
		std::span<const scene::DrawInstance> rows,
		std::span<const core::CFrame> joints,
		std::vector<std::byte> &out,
		std::string &error
	);
	// Validates the entire payload before appending. Request-local draw rows have
	// no destination ECS identity. No source tag bits or surface slots are imported.
	// A successful append replaces selection.Hidden with indices of matching rows;
	// failures leave all destination outputs unchanged.
	// With a destination store, incoming player copies replace that store's native
	// body rows before appending. Other worlds' coincident handles remain untouched.
	bool AppendPortalDraws(
		std::span<const std::byte> bytes,
		core::Name sourceWorld,
		std::vector<scene::DrawInstance> &rows,
		std::vector<core::CFrame> &joints,
		std::string &error,
		PortalDrawSelection *selection = nullptr,
		const ecs::Store *destination = nullptr
	);
	// Maps incoming copied geometry into a portal child without resolving foreign
	// handles. Replaces older child copies of matching players; failure preserves child.
	bool ForwardPortalDraws(
		std::span<const std::byte> incoming,
		const scene::PortalSeam &seam,
		std::vector<std::byte> &child,
		std::string &error
	);
}
