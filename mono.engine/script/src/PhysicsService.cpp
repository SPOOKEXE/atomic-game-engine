// `PhysicsService`, the read-only script view of `spatial::CollisionGroups`.
//
// Each group is handed back as a record of its properties rather than as a
// live object: `Name`, and `CollidesWith`, the names of every group it meets.
// The registry is process-wide, so a script writing it would change collisions
// in every world; registration and the matrix stay with C++ and Studio.
//
// No `Index` on the record. A group's index depends on registration order and
// `CollisionGroups.hpp` keeps it off every save file and wire for that reason;
// a script holding one would be holding the same unstable number.
//
// @tier L9 · shared

#include <engine/script/LuauTags.hpp>
#include <engine/script/ScriptCall.hpp>
#include <engine/script/ServiceSurface.hpp>
#include <engine/spatial/CollisionGroups.hpp>

#include <array>
#include <string>

namespace engine::script {
	namespace {

		constexpr const char *PHYSICS_METHODS_KEY = "engine.physicsservice.methods";

		ScriptValue Text(std::string_view text) {
			ScriptValue value{ValueTag::String};
			value.Text = std::string(text);
			return value;
		}

		// One group's properties, as the record both methods return.
		ScriptValue GroupRecord(core::Name group, const std::vector<core::Name> &all) {
			ScriptValue collides{ValueTag::Array};
			for (const core::Name other : all) {
				if (spatial::CollisionGroups::Collidable(group, other)) {
					collides.Items.push_back(Text(other.Text()));
				}
			}
			ScriptValue record{ValueTag::Map};
			record.Entries.emplace_back("Name", Text(group.Text()));
			record.Entries.emplace_back("CollidesWith", std::move(collides));
			return record;
		}

		// `PhysicsService:GetCollisionGroup(name)`: the group's record, or nil
		// when nothing registered that name.
		void GetCollisionGroup(ScriptCall &call) {
			const core::Name name(call.AsString(0));
			if (spatial::CollisionGroups::IndexOf(name) == spatial::NO_GROUP) {
				call.ReturnNil();
				return;
			}
			call.ReturnValue(GroupRecord(name, spatial::CollisionGroups::Names()));
		}

		// `PhysicsService:GetCollisionGroups()`: every group's record, in
		// registration order. `Default` is always first.
		void GetCollisionGroups(ScriptCall &call) {
			const std::vector<core::Name> all = spatial::CollisionGroups::Names();
			ScriptValue groups{ValueTag::Array};
			for (const core::Name group : all) {
				groups.Items.push_back(GroupRecord(group, all));
			}
			call.ReturnValue(groups);
		}

		// `PhysicsService.CollisionGroupCount`, read only.
		void GetCollisionGroupCount(ScriptCall &call) {
			call.ReturnNumber(spatial::CollisionGroups::Count());
		}
	}

	const ServiceSurface &PhysicsServiceSurface() {
		static constexpr std::array<ServiceMethod, 2> METHODS{{
			{"GetCollisionGroup", GetCollisionGroup},
			{"GetCollisionGroups", GetCollisionGroups},
		}};

		static constexpr std::array<ServiceProperty, 1> PROPERTIES{{
			{"CollisionGroupCount", GetCollisionGroupCount, nullptr},
		}};

		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "PhysicsService";
			surface.Methods = METHODS;
			surface.Properties = PROPERTIES;

			// A live property needs a userdata in Luau; see `ServiceSurface`.
			surface.Tag = TAG_PHYSICS_SERVICE;
			surface.MethodsKey = PHYSICS_METHODS_KEY;
			return surface;
		}();
		return SURFACE;
	}
}
