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
	// Configured limit for maximumcopiedcontactshapes.
	inline constexpr size_t MAXIMUM_COPIED_CONTACT_SHAPES = 256;
	// Configured limit for maximumcopiedcontactpoints.
	inline constexpr size_t MAXIMUM_COPIED_CONTACT_POINTS = 128;

	// A finite aperture prism. Normal points toward the source body; destination
	// contacts occupy the negative half-space. First and Second are half-axes.
	struct ContactWindow {
		// Vector value for centre.
		core::Vector3 Centre;
		// Contact or sweep surface normal.
		core::Vector3 Normal;
		// Vector value for first.
		core::Vector3 First;
		// Vector value for second.
		core::Vector3 Second;
		// Texture depth in texels.
		float Depth = 0;
		// Collision layer used by this contact window.
		spatial::LayerMask Layer = spatial::LayerMask::Only(0);
		// Collision mask used by this contact window.
		spatial::LayerMask Mask = spatial::LayerMask::All();
	};
	// Copied Contact Shape declaration.
	struct CopiedContactShape {
		// Coordinate frame associated with this record.
		core::CFrame Frame;
		// Vector value for extent.
		core::Vector3 Extent;
		// Contact source classification used to interpret this copied contact row.
		scene::ShapeKind Kind = scene::ShapeKind::Box;
		// Points kept in their declared order.
		std::vector<core::Vector3> Points;
	};
	// Copied Static Contacts declaration.
	struct CopiedStaticContacts {
		// Shapes kept in their declared order.
		std::vector<CopiedContactShape> Shapes;
	};
	// Root is local-only. Only Window and copied geometry have wire encoders.
	struct CopiedBodyContacts {
		// Root entity associated with this record.
		ecs::Entity Root = ecs::NULL_ENTITY;
		// Copied collider geometry used by the contact solver.
		CopiedStaticContacts Geometry;
		// Whether the result covers all requested data.
		bool Complete = false;
	};
	// Checks that a copied-contact window has a valid ordered tick interval.
	bool ValidContactWindow(const ContactWindow &window);
	// Copies static-contact facts for the requested completed contact window.
	bool CollectStaticContacts(
		ecs::Store &store, const ContactWindow &window, CopiedStaticContacts &contacts, std::string &failure
	);
	// Writes the contact-window wire record to the caller-owned byte stream.
	bool WriteContactWindow(core::ByteWriter &writer, const ContactWindow &window);
	// Reads one validated contact-window wire record without accepting malformed bounds.
	bool ReadContactWindow(core::ByteReader &reader, ContactWindow &window);
	// Writes copied static-contact rows in their portable wire representation.
	bool WriteCopiedContacts(core::ByteWriter &writer, const CopiedStaticContacts &contacts);
	// Reads bounded copied static-contact rows from the portable wire representation.
	bool ReadCopiedContacts(core::ByteReader &reader, CopiedStaticContacts &contacts);
	// Registers ECS component metadata for copied-contact snapshots.
	void RegisterCopiedContactComponents();
	// Replaces each body's copied contacts with the completed-step snapshot.
	void SetCopiedBodyContacts(ecs::Store &store, std::vector<CopiedBodyContacts> contacts);
	// Opens storage for facts collected during the next copied contact step.
	void BeginCopiedContactStep(ecs::Store &store);
	// Finalizes copied contact facts after physics has solved the step.
	void SolveCopiedContactStep(ecs::Store &store);
}
