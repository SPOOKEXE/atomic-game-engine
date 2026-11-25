// The `Part` class tree: its enums, every class in registration order, the
// effect classes appended after the properties, and the class id accessors.

#include "PartInternal.hpp"

namespace engine::scene {

	namespace part_detail {
		// The class tree, built once for the process.
		//
		// A function-local static, so the tree exists before the first caller
		// reads an id from it and cannot be registered twice. Classes are
		// process-wide and never unregister, exactly as components are, so
		// there is nothing to tear down.
		ecs::ClassId RegisterTree() {
			RegisterSceneComponents();

			// **`Material` is not registered as an enum, and its absence is the
			// point.** It held seventeen names - `Plastic`, `Wood`, `Metal` -
			// and the membership check was the only thing it did: no renderer
			// sampled anything different because a part said `Wood`. A material
			// is content now, named by a `Material` instance and resolved
			// against what a publisher published - `scene/Materials.hpp`. A game
			// that wants its own named set still registers one with
			// `ecs::EnumTable::Register`; the engine no longer ships one that
			// promises something it cannot draw.

			// The faces of a box, for `SurfaceCamera::Face`.
			//
			// **Generated from the enum rather than typed out beside it**, which
			// is the difference between one declaration and two that agree until
			// they do not. `NormalId`'s ordinals are the storage - a `Face` of 1
			// is `Top` in a game file - so a literal list here would be a second
			// place the order lives, and getting it wrong would load every saved
			// mirror pointing at the wrong side of its pane. Silently: nothing
			// about a face is checkable at load time.
			//
			// The loop walks the enum's own range, so adding a seventh face means
			// adding it in one place. `Describe(NormalId)` is what makes it
			// possible, and `scene/Enums.hpp` says why that one round-trips where
			// its neighbours are only for logs.
			std::array<std::string_view, 6> normals{};
			for (size_t index = 0; index < normals.size(); index++) {
				normals[index] = Describe(static_cast<NormalId>(index));
			}
			ecs::EnumTable::Register(NormalIdEnum().Text(), normals);

			// The surface effects, generated from `Describe` for the same reason
			// the faces are: a member list written out twice is a member list
			// that gains an entry in one place.
			std::array<std::string_view, SURFACE_EFFECT_COUNT> effects{};
			for (size_t index = 0; index < effects.size(); index++) {
				effects[index] = Describe(static_cast<SurfaceEffect>(index));
			}
			ecs::EnumTable::Register(SurfaceEffectEnum().Text(), effects);

			// The alpha modes, by the same rule: a member list registered once
			// so a script setting `.AlphaMode = "Transparency"` is checked against it.
			ecs::EnumTable::Register(
				AlphaModeEnum().Text(),
				std::array<std::string_view, 4>{"Overlay", "Transparency", "TintMask", "Opaque"}
			);
			ecs::EnumTable::Register(
				ResamplerModeEnum().Text(), std::array<std::string_view, 2>{"Default", "Pixelated"}
			);
			ecs::EnumTable::Register(
				AutoMeshLodStrategyEnum().Text(), std::array<std::string_view, 2>{"Decimated", "Reduced"}
			);
			ecs::EnumTable::Register(
				AuthoredAffordanceKindEnum().Text(),
				std::array<std::string_view, 5>{"None", "Walkable", "Climbable", "Interactable", "Cover"}
			);

			// The collider shapes. **In `ShapeKind`'s own declaration order**,
			// because `CollisionShapeProperty` converts between an ordinal and
			// the enum by casting - so a list written in a different order here
			// would silently make every part a different shape than it says.
			ecs::EnumTable::Register(
				ShapeKindEnum().Text(),
				std::array<std::string_view, 6>{"Box", "Sphere", "Cylinder", "Hull", "Mesh", "Capsule"}
			);

			// The default collision group, so `CollisionGroup` reads back
			// something a script can compare rather than an invalid name on a
			// part nobody configured.
			spatial::CollisionGroups::Register(spatial::CollisionGroups::DEFAULT);

			// **`ecs`'s, not this module's.** `Instance`, `Name` and `Parent`
			// all project components `ecs` owns, and they were declared here
			// only because `scene` happened to be the first module with a class
			// tree. v0.8 added a second - `gui`, which is `shared` and may not
			// link this one - so the root moved down to where its components
			// already live. Same correction `ecs.Hierarchy`'s registration went
			// through, and the `ParentProperty` this file used to hold is gone with it.
			const ecs::ClassId instance = ecs::Classes::RegisterInstanceRoot();

			// PVInstance is everything with a place in the world. Roblox's
			// split, kept because v0.6 binds `Instance.new` to this same table
			// and a tree that differs from the one scripts expect is a
			// migration nobody asked for.
			const std::array pv{ecs::Components::Of<Transform>(), ecs::Components::Of<Pivot>()};
			const ecs::ClassId pvInstance = ecs::Classes::Register("PVInstance", instance, pv);
			ecs::Classes::SetCreatable(pvInstance, false);

			// Fields are placed content, not a particle feature. A consumer selects
			// the nearest ancestor field, so the hierarchy owns the source origin
			// and no process-local entity handle has to be serialised on it.
			const std::array vectorField2DSet{ecs::Components::Of<VectorField2D>()};
			const ecs::ClassId vectorField2D =
				ecs::Classes::Register("VectorField2D", pvInstance, vectorField2DSet);
			const std::array vectorField3DSet{ecs::Components::Of<VectorField3D>()};
			const ecs::ClassId vectorField3D =
				ecs::Classes::Register("VectorField3D", pvInstance, vectorField3DSet);

			// A field owns bounded initial conditions and styles. Device-local live
			// particle positions do not enter authored worlds or ECS snapshots.
			const std::array gpuParticleField{
				ecs::Components::Of<GpuParticleField>(), ecs::Components::Of<GpuParticleSource>()
			};
			const ecs::ClassId gpuParticleFieldClass =
				ecs::Classes::Register("GpuParticleField", pvInstance, gpuParticleField);

			const std::array base{
				ecs::Components::Of<Bounds>(),
				ecs::Components::Of<Visual>(),
				ecs::Components::Of<Collider>(),
				ecs::Components::Of<Surface>(),

				// **What is drawn is interpolated, so what is drawn carries the
				// component interpolation needs.** This used to be added by
				// whichever scene wanted it, which meant a part created from a
				// script - `Instance.new("Part")` and nothing else - was a
				// complete, correct part that the renderer silently skipped,
				// because `CollectInstances` matches on
				// `<Transform, PreviousTransform, Bounds, Visual>`.
				//
				// A class whose instances cannot be drawn without a component
				// the class does not have is a class with a footnote. This is
				// the footnote paid off.
				ecs::Components::Of<PreviousTransform>(),

				// **What a surface is made of, and what groups it belongs to.**
				// Both are on `BasePart` rather than on `MeshPart`, which costs
				// sixteen bytes on every part in the world and buys a draw-list
				// pass with no optional join - `SurfaceAppearance`'s own header
				// carries the whole argument.
				ecs::Components::Of<SurfaceAppearance>(),
				ecs::Components::Of<Tags>(),

				// **On the class for `SurfaceAppearance`'s reason, and never
				// replicated for `LocalTransparency`'s own.** Four bytes on
				// every part, in exchange for `engine::render::CollectInstances`
				// reading it as a plain column rather than a per-row join.
				ecs::Components::Of<LocalTransparency>(),

				// **On the class, so every part has one, for
				// `SurfaceAppearance`'s reason.** A properties panel that could
				// only show density and friction on the parts somebody had
				// already customised would be a panel with a hole in it, and an
				// optional component is a join per row for four floats. The
				// flag inside says whether any of it is used -
				// `PhysicsProperties::Custom`.
				ecs::Components::Of<PhysicsProperties>(),

				// **On the class since v0.15, for the same reason
				// `PhysicsProperties` is.** All four of its fields are authored
				// - a mass, two drags and a body kind - and while its presence
				// was what said "unanchored", anchoring a part deleted them and
				// unanchoring it brought back the defaults rather than what the
				// author typed. `Simulated` carries that decision now, so this
				// is free to be what it reads as: the part's physical
				// description. Forty bytes on every part, which is the trade
				// the two entries above already make.
				ecs::Components::Of<RigidBody>(),

				// **`Simulated` is deliberately not here, and that is the safe
				// default rather than an omission.** A part is static until
				// something says otherwise, so the safe default is the one that
				// adds nothing - a bare `CreateInstance`, which is what
				// `Instance.new("Part")` is, produces a row the solver leaves
				// alone and the broad phase files in the static index, and the
				// two agree because neither was told anything.
				//
				// Until v0.18 the tag was `Anchored` and had to be *listed here*
				// to reach that same default, so every placed thing in every
				// scene carried a component to say it was ordinary.
			};
			const ecs::ClassId basePart = ecs::Classes::Register("BasePart", pvInstance, base);
			ecs::Classes::SetCreatable(basePart, false);

			// Part adds nothing of its own: BasePart already holds the full
			// set, and Part is the concrete leaf a script asks for by name. `Motion` and `Simulated` are the
			// two deliberately absent - whether a part has them is `PartDesc::Simulated`'s decision, and
			// putting either in the class set would land static geometry in the dynamic archetype.
			// `RigidBody` used to be in that sentence and no longer is: it holds
			// the author's numbers rather than the world's decision.
			const ecs::ClassId part = ecs::Classes::Register("Part", basePart, {});

			// **A `SpawnLocation` is a `Part` that says who may stand up on
			// it**, and it is a class at v0.15 because until then it was a
			// *name*: `scene::FindSpawn` looked for a child called
			// `SpawnLocation` and `Characters.hpp` recorded that as a deliberate
			// stop - a class with teams on it would have been a class with a
			// footnote while teams did not exist. They do now, so the footnote
			// is paid off and `docs/DEFERRED.md` D00119 closes with it.
			//
			// **A part rather than an `Instance` with a `Transform`.** Every
			// scene in `mono.engine/examples` builds its spawn as a block and
			// expects to see it and to stand on it, so anything that was not
			// drawn or collided would be a different thing wearing the name.
			//
			// The component is in the class set, which is `SurfaceCamera`'s
			// argument: `Instance.new("SpawnLocation")` has to make a working
			// spawn rather than one that becomes a spawn once somebody assigns a
			// field.
			const std::array pad{ecs::Components::Of<SpawnLocation>()};
			const ecs::ClassId spawnLocation = ecs::Classes::Register("SpawnLocation", part, pad);

			// **A `Model` is a `PVInstance` and adds nothing.** It is a
			// container with a place in the world, which is exactly what
			// `PVInstance` already is - so what the class buys is the
			// vocabulary: `Instance.new("Model")` resolves, `:IsA("Model")`
			// answers, and a character is a thing rather than six loose parts
			// under Workspace. `scene::MakeCharacter` is its first caller.
			const ecs::ClassId modelClass = ecs::Classes::Register("Model", pvInstance, {});

			// A semantic container for structural destruction. It intentionally adds
			// no component: the hierarchy is the group membership, and a second list
			// would become stale as soon as an author reparents one piece.
			const ecs::ClassId breakGroupClass = ecs::Classes::Register("BreakGroup", modelClass, {});
			(void)breakGroupClass;

			// A WorldRoot is a Model whose descendants form a self-contained scene.
			// Workspace and WorldModel share this ancestry, so `IsA("WorldRoot")`
			// reaches both without storing a second tag beside the class tree.
			const ecs::ClassId worldRootClass = ecs::Classes::Register("WorldRoot", modelClass, {});
			ecs::Classes::SetCreatable(worldRootClass, false);
			const ecs::ClassId worldModelClass = ecs::Classes::Register("WorldModel", worldRootClass, {});
			(void)worldModelClass;

			// **A `Tool` is a `Model` a character can be holding**, and holding
			// it is a reparent rather than a flag - `scene/Tools.hpp` carries the
			// whole decision, including why the handle is a `CharacterLimb` and
			// not an `Attachment`.
			//
			// **Roblox's abstract `BackpackItem` is deliberately not between the
			// two.** It carries members this engine has nothing behind, and
			// registering it would put an instantiable class that does nothing
			// into the insert palette - which is the exact objection
			// `docs/retired/DEFERRED.md` D00120 held this class back for.
			//
			// The component is in the class set, which is `SurfaceCamera`'s
			// argument: `Instance.new("Tool")` has to make something that can be
			// equipped rather than something that becomes a tool once a script
			// assigns a field.
			const std::array grip{ecs::Components::Of<Tool>()};
			const ecs::ClassId toolClass = ecs::Classes::Register("Tool", modelClass, grip);
			const std::array accessoryPoints{ecs::Components::Of<Accessory>()};
			(void)ecs::Classes::Register("Accessory", modelClass, accessoryPoints);

			// **A `MeshPart` is a `BasePart` whose mesh came from somewhere
			// else**, and that is the whole of the difference. It adds no
			// component: `Visual::Mesh` already names a mesh and
			// `SurfaceAppearance::ColourMap` already names a texture, both on
			// `BasePart`, because a plain `Part` may name a built-in and a
			// texture just as legitimately.
			//
			// So what the class is *for* is the vocabulary. A script written
			// against Roblox says `Instance.new("MeshPart")` and reads
			// `.MeshId` and `.TextureID`, and a class tree that made it say
			// `Part` and `.Mesh` would be a migration nobody asked for -
			// `scene/AGENTS.md`'s argument for keeping the tree Roblox's,
			// applied to the class v0.9 exists to add.
			const ecs::ClassId meshPart = ecs::Classes::Register("MeshPart", basePart, {});

			// A script-authored rig needs a class whose component set includes the
			// optional skeleton. Imported meshes remain ordinary `MeshPart`s with the
			// same component added by their importer.
			const std::array skinned{ecs::Components::Of<Skeleton>()};
			const ecs::ClassId skinnedMeshPart = ecs::Classes::Register("SkinnedMeshPart", meshPart, skinned);

			// **A camera is an instance, because a camera is a row.**
			// `scene::Camera` has been a component since v0.4 precisely so a
			// world can hold several - a spectator, a cutscene, a security
			// monitor - and `ActiveCamera` names the live one. What was missing
			// was a class, so `Instance.new("Camera")` had nothing to resolve to
			// and a script could not make one, aim one, or ask which was live.
			//
			// Derives from `PVInstance` rather than from `BasePart`: a camera
			// has a place in the world and is not drawn, collided or bounded.
			// A scriptable camera moves during `Heartbeat`, so portal traversal
			// needs the same tick-start frame a moving part uses. Surface cameras
			// inherit it harmlessly; only `ActiveCamera` is ever considered for a
			// crossing.
			const std::array camera{
				ecs::Components::Of<Camera>(),
				ecs::Components::Of<PreviousTransform>(),
				ecs::Components::Of<CameraSubject>()
			};
			const ecs::ClassId cameraClass = ecs::Classes::Register("Camera", pvInstance, camera);
			ecs::Classes::SetRuntimeLocal(cameraClass, true);
			// The authority's default camera is saved with its world.
			const ecs::ClassId worldCamera = ecs::Classes::Register("WorldCamera", cameraClass, {});
			ecs::Classes::SetCreatable(worldCamera, false);

			// **A surface camera is a camera you parent to a part**, and that is
			// the whole of the class.
			//
			// It derives from `Camera` rather than standing beside it, because
			// it *is* one - it has a field of view, clip planes and a place in
			// the world, and `workspace.CurrentCamera` is a question anybody may
			// ask of it. What it adds is a texture to render into and a face to
			// project off.
			//
			// **The component is in the class set, so `Instance.new` makes a
			// working one.** On `Camera` the surface component is structural -
			// `SurfaceSize` adds and removes it - which is right there, because
			// an ordinary camera that acquired a render target by being asked
			// its size would be a surprise. Here it is what the class is for, so
			// a `SurfaceCamera` that had to be given a size before it became one
			// would be a class with a footnote. Same argument
			// `PreviousTransform` on `BasePart` settles.
			const std::array surface{ecs::Components::Of<SurfaceCamera>()};
			const ecs::ClassId surfaceCameraClass =
				ecs::Classes::Register("SurfaceCamera", cameraClass, surface);

			// **A `Portal` is a `SurfaceCamera` that has been told where the
			// other end is, and deriving says exactly that.** Everything a
			// mirror has it keeps - the face, the surface size, the image
			// transparency, the effect, the tag filter - because a portal
			// renders through the same pass, projects onto its pane the same
			// way, and differs only in where its camera stands.
			//
			// **A class rather than a property on `SurfaceCamera`**, so a scene
			// says which of the two it means. Instantiating one is the whole
			// declaration: `Instance.new("Portal")` reads as a hole, where a
			// mirror with a nil destination would read as a mirror somebody had
			// not finished configuring.
			const std::array portal{ecs::Components::Of<Portal>()};
			const ecs::ClassId portalClass = ecs::Classes::Register("Portal", surfaceCameraClass, portal);

			// **A sound is an `Instance`, not a `PVInstance`, and the omission
			// is the design.** It has no place of its own: under `Workspace` it
			// is heard everywhere at one level, and inside a part it is heard
			// from that part and falls off with distance. Giving it a
			// `Transform` would be a second opinion about where a thing is -
			// rule 2 with a speaker attached - and would make "attach a sound
			// to a thing" a field to keep in step with a parent that already
			// says it.
			//
			// Nothing in this module plays one. `scene` is `shared` and a
			// server has no mixer; the client walks these rows and drives
			// `engine::audio`, which is the same split `Visual::Mesh` has
			// against the renderer.
			const std::array emitter{ecs::Components::Of<Sound>()};
			const ecs::ClassId soundClass = ecs::Classes::Register("Sound", instance, emitter);

			// A group is a named parent for sounds. Its mixer controls deliberately
			// do not exist until the client mixer can consume them: exposing Volume
			// or playback controls first would accept authored state that changes no
			// audio. Keeping the instance class now preserves imported UI trees.
			ecs::Classes::Register("SoundGroup", instance, {});

			// **An `Attachment` is an `Instance` and not a `PVInstance`**, which
			// is `Sound`'s omission for a related reason. A `PVInstance` carries a
			// `Transform` - a world-space placement - and an attachment already
			// holds a `CFrame` relative to its parent. Two of those on one row is
			// two opinions about where a point is, and `SetParent` would silently
			// decide which one won.
			//
			// The whole reason this class exists before anything that uses it is
			// that beams, trails and particle emitters are all parented to one.
			// Building any of those first means building them twice.
			const std::array point{ecs::Components::Of<Attachment>()};
			const ecs::ClassId attachmentClass = ecs::Classes::Register("Attachment", instance, point);

			// **A `Material` is an `Instance` under a drawable**, which is
			// Roblox's `SurfaceAppearance` arrangement and is what replaces the
			// seventeen-name `Material` enum this tree used to register. Not a
			// `PVInstance`, for `Attachment`'s reason: it has no place in the
			// world of its own, and a `Transform` on this row would be a second
			// opinion about where its parent is.
			//
			// **An instance rather than a property**, because `ROADMAP.md` v0.11
			// grows this into something carrying several texture references, and
			// a `core::Name` field on `BasePart` could never grow children.
			// `scene/Materials.hpp` carries the whole argument, including what
			// `ResolveMaterials` writes and what it costs.
			const std::array binding{ecs::Components::Of<MaterialRef>()};
			const ecs::ClassId materialClass = ecs::Classes::Register("Material", instance, binding);

			// **One component, three classes**, which is `Collider`'s trade across
			// three shapes. The three lights differ by two fields; three
			// components would be three columns, three queries and three upload
			// paths for something a renderer packs into one array either way.
			//
			// Each class sets its own `Kind` as a *prototype default*, so
			// `Instance.new("SpotLight")` is a spot light without a script saying
			// so - which is the whole point of the prototype row. A `Kind`
			// property is deliberately not declared: the class is the answer, and
			// a second way to say it is the duplicate `AGENTS.md` warns about.
			// **A `Humanoid` is an `Instance` under a character model**, which is
			// Roblox's arrangement exactly: the humanoid is a sibling of the parts
			// it drives rather than a component on one of them. That is what lets
			// a character be a model of several parts with one thing steering it,
			// and it is why `StepCharacters` walks `<Humanoid, Motion>` rather
			// than looking for a humanoid on a part.
			const std::array body{ecs::Components::Of<Humanoid>()};
			const ecs::ClassId humanoidClass = ecs::Classes::Register("Humanoid", instance, body);

			// The key names, so a script can say `Enum.KeyCode.Space` and be told
			// when it is wrong. **Generated from `Describe` rather than typed out
			// beside the enum**, which is the rule `NormalId` already follows here
			// - one declaration, and adding a key means adding it in one place.
			std::array<std::string_view, static_cast<size_t>(KeyCode::Count)> keys{};
			for (size_t index = 0; index < keys.size(); index++) {
				keys[index] = Describe(static_cast<KeyCode>(index));
			}
			ecs::EnumTable::Register("KeyCode", keys);

			// **Every input source and not only the three buttons**, because an
			// `InputObject` reports where an event came from and `MouseButton1..3`
			// can only describe a click. Generated from `Describe` for `KeyCode`'s
			// reason - one declaration, and adding a source means adding it in one
			// place. The buttons keep ordinals 0 to 2, which
			// `UserInputService:IsMouseButtonPressed` relies on and `Input.cpp`
			// holds with a `static_assert`.
			std::array<std::string_view, static_cast<size_t>(InputSource::Count)> sources{};
			for (size_t index = 0; index < sources.size(); index++) {
				sources[index] = Describe(static_cast<InputSource>(index));
			}
			ecs::EnumTable::Register("UserInputType", sources);

			// **The states a bound action's handler is told about.** Registered
			// here beside the other input enums rather than in
			// `script::OpenInputServices`, because the bindings generator does not
			// open a VM - an enum registered at VM-open time is one the manifest
			// never sees, and `Enum_UserInputState` came out of the declaration
			// file as an unknown type.
			ecs::EnumTable::Register(
				"UserInputState", std::array<std::string_view, 3>{"Begin", "Change", "End"}
			);

			ecs::EnumTable::Register(
				"MouseBehavior",
				std::array<std::string_view, 3>{"Default", "LockCenter", "LockCurrentPosition"}
			);

			// **What a bound action's handler answers with**, and the two members
			// are the whole of Roblox's set. Registered here for the reason
			// `UserInputState` above it is: the bindings generator does not open a
			// VM, so an enum registered at VM-open time is one the declaration
			// files never learn about.
			ecs::EnumTable::Register("ContextActionResult", std::array<std::string_view, 2>{"Sink", "Pass"});

			// **Where the ear is, and only the two modes the mixer can honour.**
			// Roblox's `Enum.ListenerType` has four; `CFrame` and `ObjectCFrame`
			// place the ear *and turn it*, and `client::SoundStage` posts a
			// position with no facing - so registering either would offer an
			// author a member that changes nothing. `scene/Audio.hpp` carries the
			// argument, and this is generated from its `Describe` for the reason
			// the keys above are.
			std::array<std::string_view, static_cast<size_t>(ListenerMode::Count)> listeners{};
			for (size_t index = 0; index < listeners.size(); index++) {
				listeners[index] = Describe(static_cast<ListenerMode>(index));
			}
			ecs::EnumTable::Register("ListenerType", listeners);
			ecs::EnumTable::Register(
				"CameraType",
				std::array<std::string_view, 5>{
					"Classic", "LockFirstPerson", "ShiftLock", "Scriptable", "Custom"
				}
			);

			const std::array bulb{ecs::Components::Of<Light>()};
			const ecs::ClassId lightClass = ecs::Classes::Register("Light", instance, bulb);
			// **Registered and not kept**, because a point light declares no property of
			// its own: everything it has is on `Light`, and the class exists so that
			// `Instance.new("PointLight")` resolves and so that `:IsA("PointLight")`
			// answers. The two below are kept only to hang a prototype default off.
			(void)ecs::Classes::Register("PointLight", lightClass, {});
			const ecs::ClassId spotLightClass = ecs::Classes::Register("SpotLight", lightClass, {});
			const ecs::ClassId surfaceLightClass = ecs::Classes::Register("SurfaceLight", lightClass, {});

			Light spot;
			spot.Kind = LightKind::Spot;
			ecs::Classes::Default(spotLightClass, spot);

			Light surfaceLight;
			surfaceLight.Kind = LightKind::Surface;
			ecs::Classes::Default(surfaceLightClass, surfaceLight);

			// `ValueBase` is the shared type relationship and carries no storage
			// itself. Each concrete leaf owns exactly the component matching its
			// `Value` property, so a `BoolValue` does not pay for a string and an
			// `ObjectValue` cannot contain a value of the wrong kind.
			const ecs::ClassId valueBase = ecs::Classes::Register("ValueBase", instance, {});
			ecs::Classes::SetCreatable(valueBase, false);

			const std::array text{ecs::Components::Of<TextContent>()};
			const ecs::ClassId stringValue = ecs::Classes::Register("StringValue", valueBase, text);
			const ecs::ClassId localizationTable =
				ecs::Classes::Register("LocalizationTable", valueBase, text);

			const std::array boolean{ecs::Components::Of<BoolValue>()};
			const ecs::ClassId boolValue = ecs::Classes::Register("BoolValue", valueBase, boolean);

			const std::array frameValue{ecs::Components::Of<CFrameValue>()};
			const ecs::ClassId cframeValue = ecs::Classes::Register("CFrameValue", valueBase, frameValue);

			const std::array colourValue{ecs::Components::Of<Color3Value>()};
			const ecs::ClassId color3Value = ecs::Classes::Register("Color3Value", valueBase, colourValue);

			const std::array integer{ecs::Components::Of<IntValue>()};
			const ecs::ClassId intValue = ecs::Classes::Register("IntValue", valueBase, integer);

			const std::array number{ecs::Components::Of<NumberValue>()};
			const ecs::ClassId numberValue = ecs::Classes::Register("NumberValue", valueBase, number);

			const std::array object{ecs::Components::Of<ObjectValue>()};
			const ecs::ClassId objectValue = ecs::Classes::Register("ObjectValue", valueBase, object);

			const std::array vectorValue{ecs::Components::Of<Vector3Value>()};
			const ecs::ClassId vector3Value = ecs::Classes::Register("Vector3Value", valueBase, vectorValue);

			// --- the forward-declared classes --------------------------------
			//
			// Registered rather than left until something reads them, which is
			// decision 16: a surface may ship complete and frozen with its
			// implementation deliberately unwired. What that buys concretely is
			// that a game file, a Rojo tree or a `.rbxl` import carrying one of
			// these resolves rather than failing with "no class named", and that
			// the studio's insert menu - built by walking everything registered
			// under `Instance` - can offer them. `docs/FUTURE_COMPONENTS.md` says
			// what each gets wired to.

			// **A `Bone` is an `Instance` and not Roblox's `Attachment`**, which
			// is the one place this rig departs from that tree and is argued in
			// `Skinning.hpp`: an attachment already carries a frame that
			// `ResolveAttachments` fills against its parent *part*, so inheriting
			// it would put two world frames on one row resolved by two passes
			// against two different parents.
			const std::array joint{ecs::Components::Of<Bone>()};
			const ecs::ClassId boneClass = ecs::Classes::Register("Bone", instance, joint);
			const std::array keypoint{ecs::Components::Of<RigKeypoint>()};
			const ecs::ClassId rigKeypointClass = ecs::Classes::Register("RigKeypoint", instance, keypoint);

			// **A `Skeleton` is not a class**, and that is deliberate. It is a
			// component on whatever drawable is skinned - a `MeshPart` today -
			// rather than an instance of its own, because a rig with an instance
			// between the mesh and its bones would put the bones somewhere other
			// than under the thing they deform, and `SkeletonOf` walks up from a
			// bone expecting to reach it. Roblox has no `Skeleton` class either.

			// **Four animation classes, and a track is one of them.** Roblox's
			// `AnimationTrack` is a userdata because its `Animator` is a black
			// box; `ROADMAP.md` v0.24 says this engine's is not going to be one,
			// and a row in the store is what that means here - it saves, it
			// replicates and a script can read it.
			const std::array animationBytes{ecs::Components::Of<AnimationBuffer>()};
			const ecs::ClassId animationBufferClass =
				ecs::Classes::Register("AnimationBuffer", instance, animationBytes);

			const std::array clip{ecs::Components::Of<AnimationClip>()};
			const ecs::ClassId animationClass = ecs::Classes::Register("Animation", instance, clip);

			const std::array driver{ecs::Components::Of<Animator>()};
			const ecs::ClassId animatorClass = ecs::Classes::Register("Animator", instance, driver);

			const std::array track{ecs::Components::Of<AnimationTrack>()};
			const ecs::ClassId trackClass = ecs::Classes::Register("AnimationTrack", instance, track);

			ecs::EnumTable::Register(
				"AnimationPriority",
				std::array<std::string_view, 5>{"Core", "Idle", "Movement", "Action", "Override"}
			);

			std::array<std::string_view, SKYBOX_COMPUTE_SHADER_COUNT> skyShaders{};
			for (size_t index = 0; index < skyShaders.size(); index++) {
				skyShaders[index] = Describe(static_cast<SkyboxComputeShader>(index));
			}
			ecs::EnumTable::Register(SkyboxComputeShaderEnum().Text(), skyShaders);

			std::array<std::string_view, CLOUD_COMPUTE_SHADER_COUNT> cloudShaders{};
			for (size_t index = 0; index < cloudShaders.size(); index++) {
				cloudShaders[index] = Describe(static_cast<CloudComputeShader>(index));
			}
			ecs::EnumTable::Register(CloudComputeShaderEnum().Text(), cloudShaders);

			std::array<std::string_view, ATMOSPHERE_PROCEDURAL_SHADER_COUNT> atmosphereShaders{};
			for (size_t index = 0; index < atmosphereShaders.size(); index++) {
				atmosphereShaders[index] = Describe(static_cast<AtmosphereProceduralShader>(index));
			}
			ecs::EnumTable::Register(AtmosphereProceduralShaderEnum().Text(), atmosphereShaders);
			ecs::EnumTable::Register(
				VolumeShapeEnum().Text(), std::array<std::string_view, 2>{"Box", "Ellipsoid"}
			);
			ecs::EnumTable::Register(
				VolumeChannelEnum().Text(),
				std::array<std::string_view, 4>{"Fog", "Humidity", "Condensate", "Rain"}
			);
			ecs::EnumTable::Register(LensShapeEnum().Text(), std::array<std::string_view, 1>{"Sphere"});
			ecs::EnumTable::Register(
				ParticleResponseEnum().Text(), std::array<std::string_view, 3>{"Bounce", "Stick", "Die"}
			);
			ecs::EnumTable::Register(
				ParticleChannelEnum().Text(), std::array<std::string_view, 4>{"Rain", "Snow", "Hail", "All"}
			);

			// **The sky, both halves of it under `Lighting`.** Neither is a
			// `PVInstance`, which is `Sound`'s and `Attachment`'s omission for
			// their reason: the air has no place in the world, and a `Transform`
			// here would be a second opinion about where `Lighting` is.
			const std::array air{ecs::Components::Of<Atmosphere>()};
			const ecs::ClassId atmosphereClass = ecs::Classes::Register("Atmosphere", instance, air);
			const ecs::ClassId atmosphereComponentClass =
				ecs::Classes::Register("AtmosphereComponent", atmosphereClass, {});
			const std::array proceduralAir{ecs::Components::Of<AtmosphereProcedural>()};
			const ecs::ClassId atmosphereProceduralClass =
				ecs::Classes::Register("AtmosphereProcedural", atmosphereComponentClass, proceduralAir);

			// **One cloud class, two components.** `Clouds` is the look and the
			// one `Enabled`, `CloudCompute` the volume's shape; a `WeatherCompute`
			// derives from this class and carries both. They stay two components
			// because snapshots and the wire name components, so merging them
			// would refuse every saved world holding a cloud. The retired class
			// names load as this one.
			const std::array cloud{
				ecs::Components::Of<Clouds>(),
				ecs::Components::Of<CloudCompute>(),
				ecs::Components::Of<CloudSource>()
			};
			const ecs::ClassId cloudComputeClass = ecs::Classes::Register("CloudCompute", instance, cloud);
			ecs::Classes::Alias("Clouds", cloudComputeClass);
			ecs::Classes::Alias("CloudProcedural", cloudComputeClass);
			// Shadows are their own piece, authored as a child of the cloud that
			// draws a weather field (a `CloudCompute` linked through `Source`, or
			// the `WeatherCompute` itself), so they can be switched off or removed
			// without touching the cloud. They replace that weather's own shadow
			// settings.
			const std::array cloudShadows{ecs::Components::Of<CloudShadows>()};
			const ecs::ClassId cloudShadowsClass =
				ecs::Classes::Register("CloudShadows", instance, cloudShadows);

			const std::array volume{ecs::Components::Of<Volume>(), ecs::Components::Of<VolumeSource>()};
			const ecs::ClassId volumeClass = ecs::Classes::Register("Volume", pvInstance, volume);
			const std::array physicalParticles{
				ecs::Components::Of<PhysicalParticleField>(), ecs::Components::Of<PhysicalParticleSource>()
			};
			const ecs::ClassId physicalParticleClass =
				ecs::Classes::Register("PhysicalParticleField", pvInstance, physicalParticles);
			const std::array shaderLens{ecs::Components::Of<ShaderLens>()};
			const ecs::ClassId shaderLensClass = ecs::Classes::Register("ShaderLens", pvInstance, shaderLens);
			const ecs::ClassId gravitationalLensClass =
				ecs::Classes::Register("GravitationalLens", shaderLensClass, {});
			ShaderLens gravitationalLens;
			gravitationalLens.Shader = core::Name("gravitational-lens");
			gravitationalLens.Radius = 25.0f;
			gravitationalLens.InnerRadius = 6.0f;
			gravitationalLens.Strength = 0.8f;
			gravitationalLens.Spin = 0.25f;
			ecs::Classes::Default(gravitationalLensClass, gravitationalLens);

			const std::array skyboxTextures{ecs::Components::Of<SkyboxTextures>()};
			const ecs::ClassId skyboxTexturesClass =
				ecs::Classes::Register("SkyboxTextures", instance, skyboxTextures);
			const std::array skyboxCompute{ecs::Components::Of<SkyboxCompute>()};
			const ecs::ClassId skyboxComputeClass =
				ecs::Classes::Register("SkyboxCompute", instance, skyboxCompute);

			// **One component, six classes**, which is `Light`'s trade at
			// greater width and is the whole argument in `Constraints.hpp`: a
			// generic six-degree-of-freedom joint covers the family, and each
			// class says which member of it by setting the six motion modes as a
			// *prototype default*. A `Kind` field would be the second answer, and
			// it would make a `HingeConstraint` whose axes were all free
			// expressible.
			const std::array joined{ecs::Components::Of<Constraint>()};
			const ecs::ClassId constraintClass = ecs::Classes::Register("Constraint", instance, joined);
			ecs::Classes::SetCreatable(constraintClass, false);
			const ecs::ClassId ballClass =
				ecs::Classes::Register("BallSocketConstraint", constraintClass, {});
			const ecs::ClassId hingeClass = ecs::Classes::Register("HingeConstraint", constraintClass, {});
			const ecs::ClassId prismaticClass =
				ecs::Classes::Register("PrismaticConstraint", constraintClass, {});
			const ecs::ClassId cylindricalClass =
				ecs::Classes::Register("CylindricalConstraint", constraintClass, {});
			const ecs::ClassId ropeClass = ecs::Classes::Register("RopeConstraint", constraintClass, {});
			const ecs::ClassId springClass = ecs::Classes::Register("SpringConstraint", constraintClass, {});

			const std::array legacyJoint{ecs::Components::Of<JointInstance>()};
			const ecs::ClassId jointInstanceClass =
				ecs::Classes::Register("JointInstance", instance, legacyJoint);
			ecs::Classes::SetCreatable(jointInstanceClass, false);
			const ecs::ClassId weldClass = ecs::Classes::Register("Weld", jointInstanceClass, {});

			const std::array directWeld{ecs::Components::Of<WeldConstraint>()};
			const ecs::ClassId weldConstraintClass =
				ecs::Classes::Register("WeldConstraint", instance, directWeld);

			ecs::EnumTable::Register(
				"ConstraintMotion", std::array<std::string_view, 3>{"Locked", "Limited", "Free"}
			);

			// The prototypes. **X is the joint's axis throughout**, matching
			// `Attachment`'s own convention that a beam leaves along its X axis
			// and a light's cone opens about its face: one axis convention across
			// the module, so an author who has aimed one attachment has aimed all
			// of them.
			{
				Constraint ball;
				ball.Angular[0] = ConstraintMotion::Free;
				ball.Angular[1] = ConstraintMotion::Free;
				ball.Angular[2] = ConstraintMotion::Free;
				ecs::Classes::Default(ballClass, ball);

				Constraint hinge;
				hinge.Angular[0] = ConstraintMotion::Free;
				ecs::Classes::Default(hingeClass, hinge);

				Constraint prismatic;
				prismatic.Linear[0] = ConstraintMotion::Free;
				ecs::Classes::Default(prismaticClass, prismatic);

				Constraint cylindrical;
				cylindrical.Linear[0] = ConstraintMotion::Free;
				cylindrical.Angular[0] = ConstraintMotion::Free;
				ecs::Classes::Default(cylindricalClass, cylindrical);

				// A rope is a maximum length and nothing else: the three linear
				// axes are limited to a sphere about `Attachment0` and every
				// rotation is free. `Limited` with a lower bound of zero is what
				// separates it from a rod, which would set the lower bound to the
				// same number.
				Constraint rope;
				for (size_t axis = 0; axis < CONSTRAINT_AXES; axis++) {
					rope.Linear[axis] = ConstraintMotion::Limited;
					rope.Angular[axis] = ConstraintMotion::Free;
				}
				ecs::Classes::Default(ropeClass, rope);

				// A spring is a rope with a stiffness, which is exactly what the
				// one-component design is for: no new field, and the difference
				// between the two classes is a number an author can also type.
				Constraint spring = rope;
				spring.Stiffness = 1000.0f;
				spring.Damping = 50.0f;
				ecs::Classes::Default(springClass, spring);
			}
			(void)weldClass;

			const PartClasses classes{
				instance,
				pvInstance,
				vectorField2D,
				vectorField3D,
				gpuParticleFieldClass,
				basePart,
				part,
				spawnLocation,
				modelClass,
				breakGroupClass,
				worldRootClass,
				worldModelClass,
				toolClass,
				meshPart,
				skinnedMeshPart,
				cameraClass,
				worldCamera,
				surfaceCameraClass,
				portalClass,
				soundClass,
				attachmentClass,
				materialClass,
				humanoidClass,
				lightClass,
				spotLightClass,
				surfaceLightClass,
				valueBase,
				stringValue,
				localizationTable,
				boolValue,
				cframeValue,
				color3Value,
				intValue,
				numberValue,
				objectValue,
				vector3Value,
				boneClass,
				rigKeypointClass,
				animationBufferClass,
				animationClass,
				animatorClass,
				trackClass,
				atmosphereClass,
				atmosphereComponentClass,
				atmosphereProceduralClass,
				cloudComputeClass,
				cloudShadowsClass,
				volumeClass,
				physicalParticleClass,
				shaderLensClass,
				gravitationalLensClass,
				skyboxTexturesClass,
				skyboxComputeClass,
				constraintClass,
				ballClass,
				hingeClass,
				prismaticClass,
				cylindricalClass,
				ropeClass,
				springClass,
				jointInstanceClass,
				weldClass,
				weldConstraintClass
			};
			RegisterClassProperties(classes);
			RegisterForwardDeclaredProperties(classes);

			// These are independent authored children of Lighting. Registering them
			// after the existing tree retains the established class ids.
			const ecs::ClassId bloomEffect = ecs::Classes::Register(
				"BloomEffect", instance, std::array{ecs::Components::Of<BloomEffect>()}
			);
			const ecs::ClassId depthOfFieldEffect = ecs::Classes::Register(
				"DepthOfFieldEffect", instance, std::array{ecs::Components::Of<DepthOfFieldEffect>()}
			);
			const ecs::ClassId godRaysEffect = ecs::Classes::Register(
				"GodRaysEffect", instance, std::array{ecs::Components::Of<GodRaysEffect>()}
			);
			const ecs::ClassId fogEffect =
				ecs::Classes::Register("FogEffect", instance, std::array{ecs::Components::Of<FogEffect>()});

			ecs::Classes::ClampedProperty<&BloomEffect::Intensity, 0.0f, 100.0f>(bloomEffect, "Intensity");
			ecs::Classes::ClampedProperty<&BloomEffect::Threshold, 0.0f, 10000.0f>(bloomEffect, "Threshold");
			ecs::Classes::ClampedProperty<&BloomEffect::Radius, 0.0f, 128.0f>(bloomEffect, "Radius");
			ecs::Classes::Property<&BloomEffect::Enabled>(bloomEffect, "Enabled");
			ecs::Classes::ClampedProperty<&DepthOfFieldEffect::Intensity, 0.0f, 1.0f>(
				depthOfFieldEffect, "Intensity"
			);
			ecs::Classes::ClampedProperty<&DepthOfFieldEffect::FocusDistance, 0.0f, 100000.0f>(
				depthOfFieldEffect, "FocusDistance"
			);
			ecs::Classes::ClampedProperty<&DepthOfFieldEffect::FocusRange, 0.0f, 100000.0f>(
				depthOfFieldEffect, "FocusRange"
			);
			ecs::Classes::ClampedProperty<&DepthOfFieldEffect::Radius, 0.0f, 128.0f>(
				depthOfFieldEffect, "Radius"
			);
			ecs::Classes::Property<&DepthOfFieldEffect::Enabled>(depthOfFieldEffect, "Enabled");
			ecs::Classes::ClampedProperty<&GodRaysEffect::Intensity, 0.0f, 100.0f>(
				godRaysEffect, "Intensity"
			);
			ecs::Classes::ClampedProperty<&GodRaysEffect::Threshold, 0.0f, 10000.0f>(
				godRaysEffect, "Threshold"
			);
			ecs::Classes::ClampedProperty<&GodRaysEffect::Radius, 0.0f, 512.0f>(godRaysEffect, "Radius");
			ecs::Classes::Property<&GodRaysEffect::Enabled>(godRaysEffect, "Enabled");
			ecs::Classes::Property<&FogEffect::Color>(fogEffect, "Color");
			ecs::Classes::Property<&FogEffect::Start>(fogEffect, "Start");
			ecs::Classes::Property<&FogEffect::End>(fogEffect, "End");
			ecs::Classes::Property<&FogEffect::Enabled>(fogEffect, "Enabled");
			for (const ecs::ClassId effect : {bloomEffect, depthOfFieldEffect, godRaysEffect, fogEffect}) {
				TagDeclaredProperties(effect, core::Name("Rendering"));
				TagProperty(effect, "Enabled", "Behavior");
			}
			for (const std::string_view property : {"Color", "Start", "End"})
				TagProperty(fogEffect, property, "Lighting");

			// Append new classes so existing authored class ids retain their order.
			const ecs::ClassId weatherCompute = ecs::Classes::Register(
				"WeatherCompute", cloudComputeClass, std::array{ecs::Components::Of<WeatherCompute>()}
			);
			RegisterWeatherComputeProperties(weatherCompute);
			TagDeclaredProperties(weatherCompute, core::Name("Weather"));
			TagProperty(weatherCompute, "WeatherEnabled", "Behavior");

			const ecs::ClassId starsEffect = ecs::Classes::Register(
				"StarsEffect", instance, std::array{ecs::Components::Of<StarsEffect>()}
			);
			ecs::Classes::Property<&StarsEffect::Enabled>(starsEffect, "Enabled");
			ecs::Classes::ClampedProperty<&StarsEffect::Density, 0.0f, 1.0f>(starsEffect, "Density");
			ecs::Classes::ClampedProperty<&StarsEffect::Size, 0.5f, 8.0f>(starsEffect, "Size");
			ecs::Classes::ClampedProperty<&StarsEffect::Brightness, 0.0f, 20.0f>(starsEffect, "Brightness");
			ecs::Classes::Property<&StarsEffect::Color>(starsEffect, "Color");
			ecs::Classes::ClampedProperty<&StarsEffect::ColorVariation, 0.0f, 1.0f>(
				starsEffect, "ColorVariation"
			);
			ecs::Classes::ClampedProperty<&StarsEffect::Twinkle, 0.0f, 1.0f>(starsEffect, "Twinkle");
			ecs::Classes::ClampedProperty<&StarsEffect::HorizonFade, 0.0f, 0.5f>(starsEffect, "HorizonFade");
			ecs::Classes::Property<&StarsEffect::Seed>(starsEffect, "Seed");
			TagDeclaredProperties(starsEffect, core::Name("Rendering"));
			TagProperty(starsEffect, "Enabled", "Behavior");
			TagProperty(starsEffect, "Color", "Lighting");

			return part;
		}
	}

	ecs::ClassId PartClass() {
		static const ecs::ClassId part = (EnsureClassTree(), ecs::Classes::Find(core::Name("Part")));
		return part;
	}

	// **One registration, and the static that holds it lives here.** Two
	// function-local statics each calling `RegisterTree` would have raced to
	// register the same names, which `Classes::Register` tolerates - and then
	// the *order* of the two would decide which id each class got, which is
	// rule 4's hazard arriving inside one process. So there is one static, and
	// every accessor goes through it.
	void EnsureClassTree() {
		static const ecs::ClassId root = part_detail::RegisterTree();
		(void)root;
	}

	// The rest of the tree's accessors, and they share one shape.
	//
	// **The id is cached, and that is safe because of the above.** Only
	// `EnsureClassTree` calls `RegisterTree`, so a static here calls a static
	// there rather than racing beside it - and a class id is fixed for the life
	// of the process once registered. Without this each call was a `core::Name`
	// intern plus a lookup, paid every time somebody asked.
	ecs::ClassId SoundClass() {
		static const ecs::ClassId sound = (EnsureClassTree(), ecs::Classes::Find(core::Name("Sound")));
		return sound;
	}

	ecs::ClassId CameraClass() {
		static const ecs::ClassId camera = (EnsureClassTree(), ecs::Classes::Find(core::Name("Camera")));
		return camera;
	}

	ecs::ClassId WorldCameraClass() {
		static const ecs::ClassId camera = (EnsureClassTree(), ecs::Classes::Find(core::Name("WorldCamera")));
		return camera;
	}
}
