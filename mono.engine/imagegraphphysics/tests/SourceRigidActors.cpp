#include "RigidActorHarness.hpp"
TEST_SUITE_ID("engine.imagegraphphysics.source_rigid_actors")

TEST_CASE("Path Collider preserves sampled identities including empty final collider", "[rigid][source]") {
	Harness h;
	Path2D p;
	p.Anchors = {{{0, 0, 0, 0, 0, 0}, 0}, {{20, 10, 20, 10, 20, 10}, 1}};
	auto result = h.Run(
		"pc.rigid_path_collider",
		"path",
		{{"path", p}, {"samples", int64_t{4}}, {"friction", .7}, {"bounciness", .3}}
	);
	auto s = h.Snapshot();
	REQUIRE_FALSE((s.Bodies.size() != 4));
	REQUIRE_FALSE(
		(s.Bodies.back().Friction != .7 || s.Bodies.back().Restitution != .3 ||
		 s.Bodies.back().Position != Vector2{})
	);
}

TEST_CASE("Impulse and Variable observe actual velocity with exact replay", "[rigid][source]") {
	Harness a;
	Image texture{3, 3, std::vector<uint8_t>(36, 255)};
	auto objects = a.Run(
		"pc.rigid_object",
		"body",
		{{"spawn_position", Vector2{16, 16}}, {"spawn_position_unit", EnumValue{0}}},
		&texture
	);
	a.Run(
		"pc.rigid_force_apply",
		"force",
		{{"object", objects[0].Data},
		 {"force_type", EnumValue{1}},
		 {"trigger", true},
		 {"force", Vector2{16, 0}},
		 {"strength", 1.}}
	);
	auto variables = a.Run("pc.rigid_variable", "observe", {{"objects", objects[0].Data}});
	const auto *velocity =
		&*std::find_if(variables.begin(), variables.end(), [](auto &v) { return v.Port == "velocity"; });
	const auto actual = std::get<Vector2>(std::get<ArrayValue>(velocity->Data).Elements.at(0));
	REQUIRE_FALSE((actual.X <= 0 || actual.Y != 0));
	auto before = a.Snapshot();
	auto again = a.Snapshot();
	REQUIRE_FALSE((before != again));
}

TEST_CASE("Stream spawn supports empty producer lists and ID shader channels", "[rigid][source]") {
	Image texture{3, 3, std::vector<uint8_t>(36, 255)};
	Harness stream;
	stream.DocumentData.Links.push_back({"recipe", "object", "spawner", "object"});
	auto empty = stream.Run("pc.rigid_object", "recipe", {{"spawn", false}}, &texture);
	REQUIRE_FALSE((!std::get<ArrayValue>(empty[0].Data).Elements.empty()));
	stream.NextDraws = {
		{SourceBuiltinRandomOperation::SeedObservation, 0, 0, 100},
		{SourceBuiltinRandomOperation::Random, 0, 1, .25},
		{SourceBuiltinRandomOperation::Random, 0, 1, .75},
		{SourceBuiltinRandomOperation::Random, 0, 1, .5},
		{SourceBuiltinRandomOperation::Random, 0, 1, .8},
		{SourceBuiltinRandomOperation::Random, 0, 1, .3},
		{SourceBuiltinRandomOperation::RandomRange, 1, 1, 1}
	};
	auto spawned = stream.Run(
		"pc.rigid_object_spawner",
		"spawner",
		{{"object", empty[0].Data},
		 {"spawn_area", Area{16, 16, 8, 8, 0, 0}},
		 {"spawn_area_unit", EnumValue{0}}}
	);
	auto physical = stream.Snapshot();
	REQUIRE_FALSE((physical.Bodies.size() != 1 || physical.Bodies[0].Position != Vector2{12, 16}));

	stream.NextDraws = {{SourceBuiltinRandomOperation::Random, 0, 1, 1}};
	ArrayValue general;
	general.ElementType = ValueType::Rigid;
	for (const auto &value : std::get<ArrayValue>(spawned[0].Data).Elements)
		general.Items.push_back({value});
	REQUIRE(general.Items.size() == 1);
	auto ids = stream.Run(
		"pc.rigid_render_id", "ids", {{"object_0", general}, {"normalize_output", true}, {"simulate", false}}
	);
	REQUIRE_FALSE((std::get<int64_t>(ids[0].Data) != 2));
	SurfacePixel idPixel{};
	REQUIRE_FALSE(
		(stream.Images.size() != 1 || !LoadSurfacePixel(stream.Images[0].second, 12, 16, idPixel) ||
		 idPixel != SurfacePixel{1, 1, 1, 1})
	);
}

TEST_CASE("Override and activation mutate the shared physical body", "[rigid][source]") {
	Image texture{3, 3, std::vector<uint8_t>(36, 255)};
	Harness actions;
	auto alias = actions.Run(
		"pc.rigid_object",
		"body",
		{{"spawn_position", Vector2{16, 16}}, {"spawn_position_unit", EnumValue{0}}},
		&texture
	);
	actions.Run(
		"pc.rigid_override",
		"override",
		{{"object", alias[0].Data},
		 {"set_positions", true},
		 {"positions", Vector2{.5, .75}},
		 {"set_friction", true},
		 {"friction", .8},
		 {"set_gravity_scale", true},
		 {"gravity_scale", 0.}}
	);
	auto changed = actions.Snapshot();
	REQUIRE_FALSE(
		(changed.Bodies[0].Position != Vector2{8, 12} || std::abs(changed.Bodies[0].Friction - .8) > 1e-6 ||
		 changed.Bodies[0].GravityScale != 0)
	);
	actions.Run("pc.rigid_activate", "activate", {{"object", alias[0].Data}, {"activated", false}});
	auto disabled = actions.Snapshot();
	REQUIRE_FALSE((disabled.Bodies[0].Enabled));
}

TEST_CASE("Fix Joint creates a native weld", "[rigid][source]") {
	Image texture{3, 3, std::vector<uint8_t>(36, 255)};
	Harness joints;
	auto first = joints.Run(
		"pc.rigid_object",
		"a",
		{{"spawn_position", Vector2{12, 16}}, {"spawn_position_unit", EnumValue{0}}},
		&texture
	);
	auto second = joints.Run(
		"pc.rigid_object",
		"b",
		{{"spawn_position", Vector2{20, 16}}, {"spawn_position_unit", EnumValue{0}}},
		&texture
	);
	joints.Run("pc.rigid_joint_fix", "weld", {{"object_a", first[0].Data}, {"object_b", second[0].Data}});
	REQUIRE_FALSE((joints.Snapshot().ActiveJoints != 1));
}

TEST_CASE("Rotate Joint creates a native motor", "[rigid][source]") {
	Image texture{3, 3, std::vector<uint8_t>(36, 255)};
	Harness motor;
	auto first = motor.Run(
		"pc.rigid_object",
		"a",
		{{"spawn_position", Vector2{12, 16}}, {"spawn_position_unit", EnumValue{0}}},
		&texture
	);
	auto second = motor.Run(
		"pc.rigid_object",
		"b",
		{{"spawn_position", Vector2{20, 16}}, {"spawn_position_unit", EnumValue{0}}},
		&texture
	);
	motor.Run("pc.rigid_joint_rotate", "motor", {{"object_a", first[0].Data}, {"object_b", second[0].Data}});
	REQUIRE_FALSE((motor.Snapshot().ActiveJoints != 1));
}

TEST_CASE("Texture lists and mesh actions create genuine polygon bodies", "[rigid][source]") {
	Image texture{3, 3, std::vector<uint8_t>(36, 255)};
	Harness arrays;
	AtlasValue atlas;
	atlas.Data.emplace().Surface = SurfaceValue{texture};
	atlas.Data->Position = {4, 0};
	ArrayValue texlist;
	texlist.ElementType = ValueType::Atlas;
	texlist.Elements = {atlas, atlas};
	auto list = arrays.Run(
		"pc.rigid_object",
		"array",
		{{"texture", texlist}, {"spawn_position", Vector2{16, 16}}, {"spawn_position_unit", EnumValue{0}}}
	);
	REQUIRE_FALSE(
		(std::get<ArrayValue>(list[0].Data).Elements.size() != 2 || arrays.Snapshot().Bodies.size() != 2 ||
		 arrays.Snapshot().Bodies[0].Position != Vector2{20, 16})
	);

	Image odd{5, 5, std::vector<uint8_t>(100, 255)};
	std::vector<Vector2> mesh;
	Diagnostic diagnostic;
	REQUIRE_FALSE(
		(SourceRigidGenerateObjectMesh(odd, 0, true, Limits::MaximumEvaluationBytes, mesh, diagnostic) !=
			 Status::Ok ||
		 mesh.size() < 3 || mesh.size() > 8)
	);
	const auto priorMesh = mesh;
	auto transparent = odd;
	std::fill(transparent.Pixels.begin(), transparent.Pixels.end(), 0);
	REQUIRE_FALSE(
		(SourceRigidGenerateObjectMesh(
			 transparent, 0, true, Limits::MaximumEvaluationBytes, mesh, diagnostic
		 ) != Status::Ok ||
		 mesh != priorMesh)
	);
	REQUIRE_FALSE(
		(SourceRigidGenerateObjectMesh(odd, 0, true, 1, mesh, diagnostic) != Status::LimitExceeded ||
		 mesh != priorMesh)
	);
	ArrayValue meshes;
	meshes.ElementType = ValueType::Scalar;
	std::vector<SourceArrayItem> points;
	for (const auto &point : mesh)
		points.push_back({ElementValue{point}});
	meshes.Items.push_back({std::move(points)});
	Harness custom;
	auto poly = custom.Run(
		"pc.rigid_object",
		"polygon",
		{{"attribute_mesh", meshes},
		 {"shape", EnumValue{2}},
		 {"spawn_position", Vector2{16, 16}},
		 {"spawn_position_unit", EnumValue{0}}},
		&odd
	);
	REQUIRE_FALSE((custom.Snapshot().Bodies.size() != 1 || custom.Snapshot().Bodies[0].Mass <= 0));
}

TEST_CASE(
	"Ordered render prefixes preserve pre-impulse pixels and advance the shared world", "[rigid][source]"
) {
	Harness h;
	Image texture{3, 3, std::vector<uint8_t>(36, 255)};
	auto object = h.Run(
		"pc.rigid_object",
		"body",
		{{"spawn_position", Vector2{16, 16}}, {"spawn_position_unit", EnumValue{0}}},
		&texture
	);
	h.Run("pc.rigid_render", "first", {{"object_0", object[0].Data}, {"timestep", 100.}, {"simulate", true}});
	REQUIRE(h.Images.size() == 1);
	const auto first = h.Images[0].second;
	const auto firstPose = h.Snapshot();
	h.Run(
		"pc.rigid_force_apply",
		"impulse",
		{{"object", object[0].Data},
		 {"force_type", EnumValue{1}},
		 {"trigger", true},
		 {"force", Vector2{2, 0}},
		 {"strength", 1.}}
	);
	h.Run(
		"pc.rigid_render", "second", {{"object_0", object[0].Data}, {"timestep", 100.}, {"simulate", true}}
	);
	REQUIRE(h.Images.size() == 1);
	CHECK(h.Images[0].second != first);
	const auto finalPose = h.Snapshot();
	REQUIRE(finalPose.Bodies.size() == 1);
	CHECK(finalPose.Bodies[0].Position.X > firstPose.Bodies[0].Position.X);
	SourceRigidSnapshot prefix;
	Diagnostic diagnostic;
	const auto &events = h.State.Owners[0].History.Frames[0].Events;
	auto boundary = std::find_if(events.rbegin(), events.rend(), [](const auto &event) {
		return event.Position.ConsumerId == "first";
	});
	REQUIRE(boundary != events.rend());
	REQUIRE(
		h.Provider.Replay(
			h.State.Owners[0].History,
			0,
			boundary->Position,
			Limits::MaximumEvaluationBytes,
			prefix,
			diagnostic
		) == Status::Ok
	);
	CHECK(prefix == firstPose);
	CHECK(h.Snapshot() == finalPose);
}

TEST_CASE("Static segment and wall actors retain native colliders and coefficients", "[rigid][source]") {
	Harness h;
	auto wall =
		h.Run("pc.rigid_wall", "wall", {{"sides", int64_t{15}}, {"friction", .75}, {"bounciness", .4}});
	auto segment = h.Run(
		"pc.rigid_object_segment",
		"segment",
		{{"segment_start", Vector2{4, 8}},
		 {"segment_start_unit", EnumValue{0}},
		 {"segment_end", Vector2{28, 8}},
		 {"segment_end_unit", EnumValue{0}},
		 {"friction", .6},
		 {"bounciness", .3}}
	);
	CHECK(std::get<ArrayValue>(wall[0].Data).Elements.size() == 4);
	CHECK(std::get<ArrayValue>(segment[0].Data).Elements.size() == 1);
	const auto snapshot = h.Snapshot();
	REQUIRE(snapshot.Bodies.size() == 5);
	for (const auto &body : snapshot.Bodies) {
		CHECK(body.Mass == 0);
		CHECK(body.Enabled);
	}
	CHECK(std::abs(snapshot.Bodies.back().Friction - .6) < 1e-6);
	CHECK(std::abs(snapshot.Bodies.back().Restitution - .3) < 1e-6);
}

TEST_CASE("Explosion acts on the selected body and obeys trigger", "[rigid][source]") {
	Harness h;
	Image texture{3, 3, std::vector<uint8_t>(36, 255)};
	auto object = h.Run(
		"pc.rigid_object",
		"body",
		{{"spawn_position", Vector2{20, 16}}, {"spawn_position_unit", EnumValue{0}}},
		&texture
	);
	h.Run("pc.rigid_explode", "dormant", {{"object", object[0].Data}, {"trigger", false}});
	CHECK(h.Snapshot().Bodies[0].LinearVelocity == Vector2{});
	h.Run(
		"pc.rigid_explode",
		"blast",
		{{"object", object[0].Data},
		 {"trigger", true},
		 {"position", Vector2{16, 16}},
		 {"position_unit", EnumValue{0}},
		 {"range", 10.},
		 {"range_unit", EnumValue{0}},
		 {"strength", 2.}}
	);
	const auto snapshot = h.Snapshot();
	REQUIRE(snapshot.Bodies.size() == 1);
	CHECK(snapshot.Bodies[0].LinearVelocity.X > 0);
	CHECK(snapshot.Bodies[0].LinearVelocity.Y == 0);
}

TEST_CASE("Sensor reads overlapping native bodies after an ordered solver step", "[rigid][source]") {
	Harness h;
	Image texture{3, 3, std::vector<uint8_t>(36, 255)};
	auto object = h.Run(
		"pc.rigid_object",
		"body",
		{{"spawn_position", Vector2{16, 16}}, {"spawn_position_unit", EnumValue{0}}},
		&texture
	);
	h.Run(
		"pc.rigid_sensor",
		"sensor",
		{{"detect_objects", object[0].Data},
		 {"position", Vector2{16, 16}},
		 {"position_unit", EnumValue{0}},
		 {"span", Vector2{4, 4}},
		 {"span_unit", EnumValue{0}}}
	);
	h.Run("pc.rigid_render", "step", {{"object_0", object[0].Data}, {"timestep", 20.}});
	h.Request.Tick = 1;
	auto found = h.Run(
		"pc.rigid_sensor",
		"sensor",
		{{"detect_objects", object[0].Data},
		 {"position", Vector2{16, 16}},
		 {"position_unit", EnumValue{0}},
		 {"span", Vector2{4, 4}},
		 {"span_unit", EnumValue{0}}}
	);
	REQUIRE(found.size() == 1);
	const auto &array = std::get<ArrayValue>(found[0].Data);
	REQUIRE(array.Elements.size() == 1);
	CHECK(
		std::get<RigidValue>(array.Elements[0]).Data->BodyId ==
		std::get<RigidValue>(std::get<ArrayValue>(object[0].Data).Elements[0]).Data->BodyId
	);
	CHECK(h.Snapshot().Overlaps.size() == 1);
}

TEST_CASE(
	"Collision observer retains pair history and only marks first observed contact", "[rigid][source]"
) {
	Harness h;
	Image texture{3, 3, std::vector<uint8_t>(36, 255)};
	auto first = h.Run(
		"pc.rigid_object",
		"a",
		{{"spawn_position", Vector2{16, 16}}, {"spawn_position_unit", EnumValue{0}}},
		&texture
	);
	h.Run(
		"pc.rigid_object",
		"b",
		{{"spawn_position", Vector2{17, 16}}, {"spawn_position_unit", EnumValue{0}}},
		&texture
	);
	h.Run("pc.rigid_render", "step", {{"object_0", first[0].Data}, {"timestep", 20.}});
	REQUIRE_FALSE(h.Snapshot().Contacts.empty());
	auto output = h.Run("pc.rigid_object_get_collision", "contacts", {{"objects", first[0].Data}});
	const auto read = [&](const auto &values, std::string_view port) -> const Value & {
		auto it =
			std::find_if(values.begin(), values.end(), [&](const auto &value) { return value.Port == port; });
		if (it == values.end()) throw std::runtime_error("missing observer port");
		return it->Data;
	};
	CHECK(std::get<bool>(read(output, "new_collision_trigger")));
	CHECK_FALSE(std::get<ArrayValue>(read(output, "collision_data")).Elements.empty());
	CHECK_FALSE(std::get<ArrayValue>(read(output, "new_collision_points")).Elements.empty());
	h.Request.Tick = 1;
	auto retained = h.Run("pc.rigid_object_get_collision", "contacts", {{"objects", first[0].Data}});
	CHECK_FALSE(std::get<bool>(read(retained, "new_collision_trigger")));
	CHECK(std::get<ArrayValue>(read(retained, "new_collision_points")).Elements.empty());
	CHECK_FALSE(std::get<ArrayValue>(read(retained, "collision_data")).Elements.empty());
}

TEST_CASE("Fracture creates two owned polygon bodies joined by the region tree", "[rigid][source]") {
	Harness h;
	Image base{10, 5, std::vector<uint8_t>(200)}, map = base;
	for (uint32_t y = 0; y < 5; ++y)
		for (uint32_t x = 0; x < 10; ++x) {
			REQUIRE(StoreSurfacePixel(base, x, y, {.25, .5, 1, 1}));
			REQUIRE(
				StoreSurfacePixel(map, x, y, x < 5 ? SurfacePixel{1, 0, 0, 1} : SurfacePixel{0, 1, 0, 1})
			);
		}
	AtlasValue source, regions;
	source.Data.emplace().Surface = SurfaceValue{base};
	regions.Data.emplace().Surface = SurfaceValue{map};
	auto objects = h.Run(
		"pc.rigid_fracture",
		"fracture",
		{{"base_texture", source},
		 {"fracture_map", regions},
		 {"fracture_threshold", 0.},
		 {"mesh_expansion", 0.},
		 {"use_joint", true},
		 {"position", Vector2{16, 16}},
		 {"position_unit", EnumValue{0}}}
	);
	REQUIRE(objects.size() == 1);
	CHECK(std::get<ArrayValue>(objects[0].Data).Elements.size() == 2);
	const auto snapshot = h.Snapshot();
	REQUIRE(snapshot.Bodies.size() == 2);
	CHECK(snapshot.ActiveJoints == 1);
	CHECK(snapshot.Bodies[0].Mass > 0);
	CHECK(snapshot.Bodies[1].Mass > 0);
	const auto &visuals = h.State.Owners[0].VisualFrames[0].Mutations;
	REQUIRE(visuals.size() == 2);
	REQUIRE(visuals[0].Data.Texture);
	REQUIRE(visuals[1].Data.Texture);
	CHECK(visuals[0].Data.Texture->Data.Width == 5);
	CHECK(visuals[1].Data.Texture->Data.Width == 5);
	std::fill(base.Pixels.begin(), base.Pixels.end(), 0);
	CHECK(visuals[0].Data.Texture->Data.Pixels != base.Pixels);
}

TEST_CASE(
	"Override resolves source manual arrays per object and keeps short surface fallback", "[rigid][source]"
) {
	Harness h;
	Image texture{3, 3, std::vector<uint8_t>(36, 255)}, replacement = texture;
	std::fill(replacement.Pixels.begin(), replacement.Pixels.end(), 127);
	AtlasValue atlas;
	atlas.Data.emplace().Surface = SurfaceValue{texture};
	ArrayValue textures;
	textures.ElementType = ValueType::Atlas;
	textures.Elements = {atlas, atlas};
	auto objects = h.Run(
		"pc.rigid_object",
		"body",
		{{"texture", textures}, {"spawn_position", Vector2{16, 16}}, {"spawn_position_unit", EnumValue{0}}}
	);
	ArrayValue positions, scales, alpha, friction, surfaces;
	positions.ElementType = scales.ElementType = ValueType::Vector2;
	positions.Elements = {Vector2{8, 12}, Vector2{24, 20}};
	scales.Elements = {Vector2{3, 4}, Vector2{5, 6}};
	alpha.ElementType = friction.ElementType = ValueType::Scalar;
	alpha.Elements = {.5};
	friction.Elements = {.2};
	surfaces.ElementType = ValueType::Image;
	surfaces.Elements = {SurfaceValue{replacement}};
	h.Run(
		"pc.rigid_override",
		"override",
		{{"object", objects[0].Data},
		 {"set_positions", true},
		 {"positions", positions},
		 {"set_scales", true},
		 {"scales", scales},
		 {"set_alpha", true},
		 {"alpha", alpha},
		 {"set_friction", true},
		 {"friction", friction},
		 {"set_surfaces", true},
		 {"surfaces", surfaces}}
	);
	const auto snapshot = h.Snapshot();
	REQUIRE(snapshot.Bodies.size() == 2);
	CHECK(snapshot.Bodies[0].Position == Vector2{8, 12});
	CHECK(snapshot.Bodies[1].Position == Vector2{24, 20});
	CHECK(std::abs(snapshot.Bodies[0].Friction - .2) < 1e-6);
	CHECK(snapshot.Bodies[1].Friction == 0);
	const auto &visuals = h.State.Owners[0].VisualFrames[0].Mutations;
	REQUIRE(visuals.size() == 4);
	CHECK(visuals[2].Data.XScale == 3);
	CHECK(visuals[2].Data.YScale == 4);
	CHECK(visuals[3].Data.XScale == 5);
	CHECK(visuals[3].Data.YScale == 6);
	CHECK(visuals[2].Data.Alpha == .5);
	CHECK(visuals[3].Data.Alpha == 0);
	REQUIRE(visuals[2].Data.Texture);
	REQUIRE(visuals[3].Data.Texture);
	CHECK(visuals[2].Data.Texture->Data == replacement);
	CHECK(visuals[3].Data.Texture->Data == texture);
}
