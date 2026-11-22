// Shared internals of the `Part` class tree, split across the Part*.cpp files:
// the property descriptors, the interned enum names and the class ids the
// registration passes between its phases. Private to this module.
#pragma once

#include "PropertyTags.hpp"
#include "environment/WeatherComputeRegistration.hpp"

#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Audio.hpp>
#include <engine/scene/AuthoredAffordance.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Constraints.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Tagging.hpp>
#include <engine/scene/Teams.hpp>
#include <engine/scene/Volume.hpp>
#include <engine/scene/cameras/ActiveCamera.hpp>
#include <engine/scene/characters/Accessories.hpp>
#include <engine/scene/characters/Animation.hpp>
#include <engine/scene/characters/Controls.hpp>
#include <engine/scene/characters/Input.hpp>
#include <engine/scene/characters/Skinning.hpp>
#include <engine/scene/characters/Tools.hpp>
#include <engine/scene/content/MeshCatalogue.hpp>
#include <engine/scene/content/ShaderLens.hpp>
#include <engine/scene/environment/Atmosphere.hpp>
#include <engine/scene/environment/GpuParticleField.hpp>
#include <engine/scene/environment/LightingEffects.hpp>
#include <engine/scene/environment/PhysicalParticleField.hpp>
#include <engine/scene/environment/VectorField.hpp>
#include <engine/spatial/CollisionGroups.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <numbers>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace engine::scene::part_detail {

	// --- the property surface -------------------------------------------
	//
	// What a script sees, projected onto what the components hold. Roblox's
	// names, because v0.6 binds `Instance.new` to this table and a surface
	// that differs from the one scripts expect is a migration nobody asked
	// for - the argument `RegisterTree` already makes for the class tree.
	//
	// **Four of these are plain fields and the rest are conversions**, which
	// is why `PropertyDescriptor` stopped being a component and an offset at
	// v0.5. `Size` is a doubled half-extent, `Position` is part of a
	// `CFrame`, `Orientation` is a quaternion in degrees and `Anchored` is
	// not stored anywhere at all.

	using ecs::PropertyDescriptor;
	using ecs::PropertyKind;
	using ecs::PropertyType;

	constexpr float DEGREES_PER_RADIAN = 180.0f / std::numbers::pi_v<float>;
	constexpr float RADIANS_PER_DEGREE = std::numbers::pi_v<float> / 180.0f;

	// SurfaceSize: the texture a surface camera renders into.
	//
	// **Structural, because the component's presence is the query.** A
	// camera with no `SurfaceCamera` is an ordinary camera and a consumer
	// walks past it; setting a non-zero size is what makes it one that
	// renders to a texture, and setting a zero size takes it back.
	//
	// A `Vector2` rather than two properties: a width without a height is
	// half a target, and two writes means a frame where the two disagree.
	// Which face of the parent part the surface camera projects off.
	//
	// An enum rather than a number, so `camera.Face = "Frnot"` is refused
	// where it was written instead of landing in the component as a face
	// nobody chose. Membership is `EnumTable`'s, and the storage is the
	// ordinal - Roblox's ordinal, so a game file carrying a number means the
	// same thing in both engines.
	// Interned once. `Name.hpp` states the rule this was breaking in as many
	// words - "not free, so do it once and keep the result rather than
	// constructing from a literal inside a loop" - and a property getter read
	// every frame by an immediate-mode properties panel is that loop. Each
	// construction took the global name-registry mutex and hashed a string,
	// on top of the `EnumTable` mutex the lookup already takes.
	inline const core::Name &NormalIdEnum() {
		static const core::Name name("NormalId");
		return name;
	}

	// The collider shape's enum name, interned for `NormalIdEnum`'s reason:
	// a properties panel reads this every frame and a `core::Name` built
	// from a literal takes the registry mutex and hashes a string each time.
	inline const core::Name &ShapeKindEnum() {
		static const core::Name name("ShapeKind");
		return name;
	}

	// The surface effect's enum name, interned for `NormalIdEnum`'s reason.
	inline const core::Name &SurfaceEffectEnum() {
		static const core::Name name("SurfaceEffect");
		return name;
	}

	inline const core::Name &SkyboxComputeShaderEnum() {
		static const core::Name name("SkyboxComputeShader");
		return name;
	}

	inline const core::Name &CloudComputeShaderEnum() {
		static const core::Name name("CloudComputeShader");
		return name;
	}

	inline const core::Name &AtmosphereProceduralShaderEnum() {
		static const core::Name name("AtmosphereProceduralShader");
		return name;
	}

	inline const core::Name &VolumeShapeEnum() {
		static const core::Name name("VolumeShape");
		return name;
	}

	inline const core::Name &VolumeChannelEnum() {
		static const core::Name name("VolumeChannel");
		return name;
	}

	inline const core::Name &ParticleResponseEnum() {
		static const core::Name name("ParticleResponse");
		return name;
	}

	inline const core::Name &ParticleChannelEnum() {
		static const core::Name name("ParticleChannel");
		return name;
	}

	inline const core::Name &LensShapeEnum() {
		static const core::Name name("LensShape");
		return name;
	}

	inline const core::Name &AutoMeshLodStrategyEnum() {
		static const core::Name name("AutoMeshLODStrategy");
		return name;
	}

	inline const core::Name &AuthoredAffordanceKindEnum() {
		static const core::Name name("AuthoredAffordanceKind");
		return name;
	}

	// Defaults remain readable without allocating gameplay metadata on ordinary parts.

	// How a surface's texture alpha is treated, as an enum member.
	//
	// Interned once, for `NormalIdEnum`'s reason: a properties panel reads
	// this every frame and constructing from a literal takes the global
	// name-registry mutex each time.
	inline const core::Name &AlphaModeEnum() {
		static const core::Name name("AlphaMode");
		return name;
	}

	inline const core::Name &ResamplerModeEnum() {
		static const core::Name name("ResamplerMode");
		return name;
	}

	// Places an instance; see `PlaceInstance` in PartPlacementProperties.cpp.
	bool PlaceInstance(ecs::Store &store, ecs::Entity instance, const core::CFrame &frame);

	// The descriptors the registration declares, one file per group.
	PropertyDescriptor CFrameProperty();
	PropertyDescriptor PositionProperty();
	PropertyDescriptor OrientationProperty();
	PropertyDescriptor PartSizeProperty();
	PropertyDescriptor CanCollideProperty();
	PropertyDescriptor CanTouchProperty();
	PropertyDescriptor MassProperty();
	PropertyDescriptor AnchoredProperty();
	PropertyDescriptor KinematicProperty();
	PropertyDescriptor CollisionGroupProperty();
	PropertyDescriptor LocalTransparencyProperty();
	PropertyDescriptor HumanoidRootPartProperty();
	PropertyDescriptor CameraSubjectProperty();
	PropertyDescriptor CameraTypeProperty();
	PropertyDescriptor FieldOfViewProperty();
	PropertyDescriptor GpuParticleLayersProperty();
	PropertyDescriptor SurfaceEffectProperty();
	PropertyDescriptor DestinationProperty();
	PropertyDescriptor DestinationWorldProperty();
	PropertyDescriptor FaceProperty();
	PropertyDescriptor AlphaModeProperty();
	PropertyDescriptor ResampleModeProperty();
	PropertyDescriptor TagFilterProperty();
	PropertyDescriptor AttachmentPositionProperty();
	PropertyDescriptor WorldCFrameProperty();
	PropertyDescriptor WorldPositionProperty();
	PropertyDescriptor CollisionShapeProperty();
	PropertyDescriptor CollisionGeometryProperty();
	PropertyDescriptor LightFaceProperty();
	PropertyDescriptor GroundedProperty();
	PropertyDescriptor HealthProperty();
	PropertyDescriptor MaxHealthProperty();
	PropertyDescriptor TrianglesCountProperty();
	PropertyDescriptor AutoLodStrategyProperty();
	PropertyDescriptor SurfaceSizeProperty();
	PropertyDescriptor BoneWorldProperty();
	PropertyDescriptor SkeletonJointCountProperty();
	PropertyDescriptor RigKeypointJointProperty();
	PropertyDescriptor AnimationBufferSizeProperty();
	PropertyDescriptor AnimationBufferKeyframeBytesProperty();
	PropertyDescriptor TrackPriorityProperty();
	PropertyDescriptor Attachment0Property();
	PropertyDescriptor Attachment1Property();
	PropertyDescriptor LinearLimitProperty(bool upper);
	PropertyDescriptor AngularLimitProperty(bool upper);

	// Every class id `RegisterTree` registers before it declares properties,
	// so the property phases in other files can name the classes they extend.
	struct PartClasses {
		ecs::ClassId instance;
		ecs::ClassId pvInstance;
		ecs::ClassId vectorField2D;
		ecs::ClassId vectorField3D;
		ecs::ClassId gpuParticleFieldClass;
		ecs::ClassId basePart;
		ecs::ClassId part;
		ecs::ClassId spawnLocation;
		ecs::ClassId modelClass;
		ecs::ClassId breakGroupClass;
		ecs::ClassId worldRootClass;
		ecs::ClassId worldModelClass;
		ecs::ClassId toolClass;
		ecs::ClassId meshPart;
		ecs::ClassId skinnedMeshPart;
		ecs::ClassId cameraClass;
		ecs::ClassId worldCamera;
		ecs::ClassId surfaceCameraClass;
		ecs::ClassId portalClass;
		ecs::ClassId soundClass;
		ecs::ClassId attachmentClass;
		ecs::ClassId materialClass;
		ecs::ClassId humanoidClass;
		ecs::ClassId lightClass;
		ecs::ClassId spotLightClass;
		ecs::ClassId surfaceLightClass;
		ecs::ClassId valueBase;
		ecs::ClassId stringValue;
		ecs::ClassId localizationTable;
		ecs::ClassId boolValue;
		ecs::ClassId cframeValue;
		ecs::ClassId color3Value;
		ecs::ClassId intValue;
		ecs::ClassId numberValue;
		ecs::ClassId objectValue;
		ecs::ClassId vector3Value;
		ecs::ClassId boneClass;
		ecs::ClassId rigKeypointClass;
		ecs::ClassId animationBufferClass;
		ecs::ClassId animationClass;
		ecs::ClassId animatorClass;
		ecs::ClassId trackClass;
		ecs::ClassId atmosphereClass;
		ecs::ClassId atmosphereComponentClass;
		ecs::ClassId atmosphereProceduralClass;
		ecs::ClassId cloudComputeClass;
		ecs::ClassId cloudShadowsClass;
		ecs::ClassId volumeClass;
		ecs::ClassId physicalParticleClass;
		ecs::ClassId shaderLensClass;
		ecs::ClassId gravitationalLensClass;
		ecs::ClassId skyboxTexturesClass;
		ecs::ClassId skyboxComputeClass;
		ecs::ClassId constraintClass;
		ecs::ClassId ballClass;
		ecs::ClassId hingeClass;
		ecs::ClassId prismaticClass;
		ecs::ClassId cylindricalClass;
		ecs::ClassId ropeClass;
		ecs::ClassId springClass;
		ecs::ClassId jointInstanceClass;
		ecs::ClassId weldClass;
		ecs::ClassId weldConstraintClass;
	};

	// The properties declared where each component arrives (PartClassProperties.cpp).
	void RegisterClassProperties(const PartClasses &classes);

	// The forward-declared classes' properties (PartForwardDeclaredProperties.cpp).
	void RegisterForwardDeclaredProperties(const PartClasses &classes);

	// Builds the class tree once; see PartClassTree.cpp.
	ecs::ClassId RegisterTree();

}
