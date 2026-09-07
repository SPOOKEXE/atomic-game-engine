#pragma once

#include <engine/core/Bytes.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/scene/Components.hpp>

#include <span>
#include <string>
#include <vector>

namespace engine::ecs {
	class Store;
}
namespace engine::physics {
	inline constexpr size_t MAXIMUM_COPIED_CONTACT_SHAPES = 256;
	inline constexpr size_t MAXIMUM_COPIED_CONTACT_POINTS = 128;

	// A finite aperture prism. Normal points toward the source body; destination
	// contacts occupy the negative half-space. First and Second are half-axes.
	struct ContactWindow {
		core::Vector3 Centre;
		core::Vector3 Normal;
		core::Vector3 First;
		core::Vector3 Second;
		float Depth = 0;
		spatial::LayerMask Layer = spatial::LayerMask::Only(0);
		spatial::LayerMask Mask = spatial::LayerMask::All();
	};
	struct CopiedContactShape {
		core::CFrame Frame;
		core::Vector3 Extent;
		scene::ShapeKind Kind = scene::ShapeKind::Box;
		std::vector<core::Vector3> Points;
	};
	struct CopiedStaticContacts {
		std::vector<CopiedContactShape> Shapes;
	};
	// Root is local-only. Only Window and copied geometry have wire encoders.
	struct CopiedBodyContacts {
		ecs::Entity Root = ecs::NULL_ENTITY;
		CopiedStaticContacts Geometry;
		bool Complete = false;
	};
	bool ValidContactWindow(const ContactWindow &window);
	bool CollectStaticContacts(
		ecs::Store &store, const ContactWindow &window, CopiedStaticContacts &contacts, std::string &failure
	);
	bool WriteContactWindow(core::ByteWriter &writer, const ContactWindow &window);
	bool ReadContactWindow(core::ByteReader &reader, ContactWindow &window);
	bool WriteCopiedContacts(core::ByteWriter &writer, const CopiedStaticContacts &contacts);
	bool ReadCopiedContacts(core::ByteReader &reader, CopiedStaticContacts &contacts);
	void RegisterCopiedContactComponents();
	void SetCopiedBodyContacts(ecs::Store &store, std::vector<CopiedBodyContacts> contacts);
	void BeginCopiedContactStep(ecs::Store &store);
	void SolveCopiedContactStep(ecs::Store &store);
}
