#pragma once
#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraphphysics::testing {
	inline imagegraph::Document RigidGraphFixture() {
		using namespace imagegraph;
		Document document;
		document.FormatVersion = 9;
		document.Project = ProjectSettings{};
		document.Project->SurfaceWidth = 32;
		document.Project->SurfaceHeight = 32;
		document.Nodes = {
			{"owner",
			 "pc.rigid_group_inline",
			 "",
			 {},
			 {{"dimension", Vector2{32, 32}},
			  {"dimension_unit", EnumValue{0}},
			  {"simulation_scale", 16.},
			  {"strength", 10.},
			  {"use_wall", true},
			  {"walls", int64_t{2}}}},
			{"texture",
			 "image.solid",
			 "rigid",
			 {},
			 {{"width", int64_t{4}}, {"height", int64_t{4}}, {"colour", Colour{255, 80, 20, 255}}}},
			{"body",
			 "pc.rigid_object",
			 "rigid",
			 {},
			 {{"spawn", true},
			  {"spawn_frame", int64_t{0}},
			  {"spawn_position", Vector2{16, 6}},
			  {"spawn_position_unit", EnumValue{0}},
			  {"fix_rotation", true}}},
			{"render", "pc.rigid_render", "rigid", {}, {{"timestep", 100.}, {"round_position", true}}}
		};
		document.Nodes.back().DynamicInputs = {{"object_0", ValueType::Rigid, std::nullopt}};
		Group group{"rigid", "Rigid native contact study"};
		group.OwnerNodeId = "owner";
		document.Groups = {group};
		document.Links = {{"texture", "image", "body", "texture"}, {"body", "object", "render", "object_0"}};
		document.Outputs = {{"image", "render", "surface_out"}, {"atlas", "render", "atlas_out"}};
		return document;
	}
}
