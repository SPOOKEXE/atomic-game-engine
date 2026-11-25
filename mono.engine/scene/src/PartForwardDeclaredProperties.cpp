// The properties `RegisterTree` declares on the forward-declared classes
// (bones, animation, constraints, environment) and their property tags.

#include "PartInternal.hpp"

namespace engine::scene::part_detail {

	template <class Component, auto Member, const core::Name &(*EnumName)()>
	PropertyDescriptor EnumFieldProperty(std::string_view name) {
		PropertyDescriptor property;
		property.Name = core::Name(name);
		property.Type = PropertyType::Enum;
		property.EnumName = EnumName();
		property.Size = sizeof(core::Name);
		property.Kind = PropertyKind::Field;
		property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Component>()});
		property.Writes = property.Reads;
		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Component *component = store.Get<Component>(instance);
			if (component == nullptr) {
				return false;
			}
			*static_cast<core::Name *>(out) =
				ecs::EnumTable::MemberAt(EnumName(), static_cast<size_t>(component->*Member));
			return true;
		};
		property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
			Component *component = store.GetMutable<Component>(instance);
			if (component == nullptr) {
				return false;
			}
			size_t ordinal = 0;
			if (!ecs::EnumTable::OrdinalOf(EnumName(), *static_cast<const core::Name *>(value), ordinal)) {
				return false;
			}
			using Field = std::remove_cvref_t<decltype(component->*Member)>;
			component->*Member = static_cast<Field>(ordinal);
			return true;
		};
		return property;
	}

	template <class Link> PropertyDescriptor RigidLinkActiveProperty() {
		PropertyDescriptor property;
		property.Name = core::Name("Active");
		property.Type = PropertyType::Bool;
		property.Size = sizeof(bool);
		property.Kind = PropertyKind::Computed;
		property.Reads =
			&ecs::ComponentSet::Intern({ecs::Components::Of<Link>(), ecs::Components::Of<ecs::Hierarchy>()});
		property.Writes = &ecs::ComponentSet::Intern({});
		property.Writable = false;
		property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
			const Link *link = store.Get<Link>(instance);
			if (link == nullptr) {
				return false;
			}
			const ecs::Entity workspace = WorkspaceOf(store);
			const bool active = link->Enabled && link->Part0 != ecs::NULL_ENTITY &&
								link->Part1 != ecs::NULL_ENTITY && link->Part0 != link->Part1 &&
								workspace != ecs::NULL_ENTITY && store.IsDescendantOf(instance, workspace) &&
								store.IsDescendantOf(link->Part0, workspace) &&
								store.IsDescendantOf(link->Part1, workspace);
			*static_cast<bool *>(out) = active;
			return true;
		};
		return property;
	}

	// Declares the forward-declared classes' properties and their tags.
	void RegisterForwardDeclaredProperties(const PartClasses &classes) {
		const ecs::ClassId pvInstance = classes.pvInstance;
		const ecs::ClassId vectorField2D = classes.vectorField2D;
		const ecs::ClassId vectorField3D = classes.vectorField3D;
		const ecs::ClassId gpuParticleFieldClass = classes.gpuParticleFieldClass;
		const ecs::ClassId basePart = classes.basePart;
		const ecs::ClassId part = classes.part;
		const ecs::ClassId spawnLocation = classes.spawnLocation;
		const ecs::ClassId toolClass = classes.toolClass;
		const ecs::ClassId meshPart = classes.meshPart;
		const ecs::ClassId skinnedMeshPart = classes.skinnedMeshPart;
		const ecs::ClassId cameraClass = classes.cameraClass;
		const ecs::ClassId surfaceCameraClass = classes.surfaceCameraClass;
		const ecs::ClassId portalClass = classes.portalClass;
		const ecs::ClassId soundClass = classes.soundClass;
		const ecs::ClassId attachmentClass = classes.attachmentClass;
		const ecs::ClassId materialClass = classes.materialClass;
		const ecs::ClassId humanoidClass = classes.humanoidClass;
		const ecs::ClassId lightClass = classes.lightClass;
		const ecs::ClassId spotLightClass = classes.spotLightClass;
		const ecs::ClassId surfaceLightClass = classes.surfaceLightClass;
		const ecs::ClassId valueBase = classes.valueBase;
		const ecs::ClassId stringValue = classes.stringValue;
		const ecs::ClassId localizationTable = classes.localizationTable;
		const ecs::ClassId boolValue = classes.boolValue;
		const ecs::ClassId cframeValue = classes.cframeValue;
		const ecs::ClassId color3Value = classes.color3Value;
		const ecs::ClassId intValue = classes.intValue;
		const ecs::ClassId numberValue = classes.numberValue;
		const ecs::ClassId objectValue = classes.objectValue;
		const ecs::ClassId vector3Value = classes.vector3Value;
		const ecs::ClassId boneClass = classes.boneClass;
		const ecs::ClassId rigKeypointClass = classes.rigKeypointClass;
		const ecs::ClassId animationBufferClass = classes.animationBufferClass;
		const ecs::ClassId animationClass = classes.animationClass;
		const ecs::ClassId animatorClass = classes.animatorClass;
		const ecs::ClassId trackClass = classes.trackClass;
		const ecs::ClassId atmosphereClass = classes.atmosphereClass;
		const ecs::ClassId atmosphereComponentClass = classes.atmosphereComponentClass;
		const ecs::ClassId atmosphereProceduralClass = classes.atmosphereProceduralClass;
		const ecs::ClassId cloudComputeClass = classes.cloudComputeClass;
		const ecs::ClassId cloudShadowsClass = classes.cloudShadowsClass;
		const ecs::ClassId volumeClass = classes.volumeClass;
		const ecs::ClassId physicalParticleClass = classes.physicalParticleClass;
		const ecs::ClassId shaderLensClass = classes.shaderLensClass;
		const ecs::ClassId gravitationalLensClass = classes.gravitationalLensClass;
		const ecs::ClassId skyboxTexturesClass = classes.skyboxTexturesClass;
		const ecs::ClassId skyboxComputeClass = classes.skyboxComputeClass;
		const ecs::ClassId constraintClass = classes.constraintClass;
		const ecs::ClassId ballClass = classes.ballClass;
		const ecs::ClassId hingeClass = classes.hingeClass;
		const ecs::ClassId prismaticClass = classes.prismaticClass;
		const ecs::ClassId cylindricalClass = classes.cylindricalClass;
		const ecs::ClassId ropeClass = classes.ropeClass;
		const ecs::ClassId springClass = classes.springClass;
		const ecs::ClassId jointInstanceClass = classes.jointInstanceClass;
		const ecs::ClassId weldClass = classes.weldClass;
		const ecs::ClassId weldConstraintClass = classes.weldConstraintClass;

		// --- the forward-declared classes' properties ---------------------

		// **`Transform` and not `CFrame`, which is Roblox's spelling and is
		// the more useful one anyway**: the writable field on a bone is what
		// an animation *adds* to the rest pose, so a script that clears it
		// puts the rig back where it started. `RestCFrame` is the pose it goes
		// back to, writable because a rig assembled by a script has to be able
		// to say where its joints sit.
		ecs::Classes::Property<&Bone::Transform>(boneClass, "Transform");
		ecs::Classes::Property<&Bone::Rest>(boneClass, "RestCFrame");
		ecs::Classes::Property<&Bone::InverseBind>(boneClass, "InverseBindCFrame");
		ecs::Classes::Computed(boneClass, BoneWorldProperty());
		ecs::Classes::Property<&RigKeypoint::Keypoint>(rigKeypointClass, "KeypointId");
		ecs::Classes::Property<&RigKeypoint::Frame>(rigKeypointClass, "CFrame");
		ecs::Classes::Computed(rigKeypointClass, RigKeypointJointProperty());

		// The skeleton is guaranteed by this class's component set, unlike the
		// optional component on an imported ordinary `MeshPart`.
		ecs::Classes::Property<&Skeleton::Rig>(skinnedMeshPart, "RigId");
		ecs::Classes::Computed(skinnedMeshPart, SkeletonJointCountProperty());

		ecs::Classes::Computed(animationBufferClass, AnimationBufferSizeProperty());
		ecs::Classes::Computed(animationBufferClass, AnimationBufferKeyframeBytesProperty());

		// The clip source and rig. **`RigId` beside `AnimationId`**, because a
		// clip authored against one skeleton means nothing on another and
		// `ClipFitsRig` is what refuses it - an author who cannot set the
		// second cannot make that refusal work.
		ecs::Classes::Property<&AnimationClip::Asset>(animationClass, "AnimationId");
		ecs::Classes::Property<&AnimationClip::Rig>(animationClass, "RigId");
		ecs::Classes::Property<&AnimationClip::Buffer>(animationClass, "AnimationBuffer");

		ecs::Classes::Property<&Animator::Rig>(animatorClass, "Rig");
		ecs::Classes::Property<&Animator::RootMotion>(animatorClass, "RootMotion");
		ecs::Classes::ClampedProperty<&Animator::RootMotionWeight, 0.0f, 1.0f>(
			animatorClass, "RootMotionWeight"
		);
		ecs::Classes::Property<&Animator::EvaluationThrottled>(animatorClass, "EvaluationThrottled");

		// **`WeightCurrent` is writable here and read-only in Roblox**, which
		// is the black-box difference stated once more: a fade is the ordinary
		// way to reach a weight and `WeightTarget` is how you ask for one, but
		// a game building its own blend tree has to be able to set the weight
		// outright and there is nothing in the storage that stops it.
		ecs::Classes::Property<&AnimationTrack::Clip>(trackClass, "Animation");
		ecs::Classes::Property<&AnimationTrack::TimePosition>(trackClass, "TimePosition");
		ecs::Classes::Property<&AnimationTrack::Speed>(trackClass, "Speed");
		ecs::Classes::ClampedProperty<&AnimationTrack::Weight, 0.0f, 1.0f>(trackClass, "WeightCurrent");
		ecs::Classes::ClampedProperty<&AnimationTrack::WeightTarget, 0.0f, 1.0f>(trackClass, "WeightTarget");
		ecs::Classes::Property<&AnimationTrack::FadeTime>(trackClass, "FadeTime");
		ecs::Classes::Property<&AnimationTrack::Looped>(trackClass, "Looped");
		ecs::Classes::Property<&AnimationTrack::Playing>(trackClass, "IsPlaying");
		ecs::Classes::Computed(trackClass, TrackPriorityProperty());

		// The sky. **`Color` and not `Colour`**, matching `Light.Color` and
		// every other colour on this surface: the spelling is Roblox's because
		// a script written against that engine has to keep working, and the
		// field behind it is this repository's because the code is not.
		ecs::Classes::Property<&Atmosphere::Colour>(atmosphereClass, "Color");
		ecs::Classes::Property<&Atmosphere::Decay>(atmosphereClass, "Decay");
		ecs::Classes::ClampedProperty<&Atmosphere::Density, 0.0f, 1.0f>(atmosphereClass, "Density");
		ecs::Classes::ClampedProperty<&Atmosphere::Offset, -1.0f, 1.0f>(atmosphereClass, "Offset");
		ecs::Classes::ClampedProperty<&Atmosphere::Glare, 0.0f, 10.0f>(atmosphereClass, "Glare");
		ecs::Classes::ClampedProperty<&Atmosphere::Haze, 0.0f, 10.0f>(atmosphereClass, "Haze");

		ecs::Classes::Property<&Clouds::Colour>(cloudComputeClass, "Color");
		ecs::Classes::ClampedProperty<&Clouds::Cover, 0.0f, 1.0f>(cloudComputeClass, "Cover");
		ecs::Classes::ClampedProperty<&Clouds::Density, 0.0f, 1.0f>(cloudComputeClass, "Density");
		ecs::Classes::Property<&Clouds::WindSpeed>(cloudComputeClass, "WindSpeed");
		ecs::Classes::Property<&Clouds::WindDirection>(cloudComputeClass, "WindDirection");
		ecs::Classes::Property<&Clouds::Enabled>(cloudComputeClass, "Enabled");

		ecs::Classes::Property<&SkyboxTextures::Front>(skyboxTexturesClass, "Front");
		ecs::Classes::Property<&SkyboxTextures::Back>(skyboxTexturesClass, "Back");
		ecs::Classes::Property<&SkyboxTextures::Left>(skyboxTexturesClass, "Left");
		ecs::Classes::Property<&SkyboxTextures::Right>(skyboxTexturesClass, "Right");
		ecs::Classes::Property<&SkyboxTextures::Up>(skyboxTexturesClass, "Up");
		ecs::Classes::Property<&SkyboxTextures::Down>(skyboxTexturesClass, "Down");
		ecs::Classes::Property<&SkyboxTextures::Enabled>(skyboxTexturesClass, "Enabled");

		ecs::Classes::Property<&SkyboxCompute::Zenith>(skyboxComputeClass, "ZenithColor");
		ecs::Classes::Property<&SkyboxCompute::Horizon>(skyboxComputeClass, "HorizonColor");
		ecs::Classes::Property<&SkyboxCompute::Ground>(skyboxComputeClass, "GroundColor");
		ecs::Classes::ClampedProperty<&SkyboxCompute::SunSize, 0.001f, 0.25f>(skyboxComputeClass, "SunSize");
		ecs::Classes::Property<&SkyboxCompute::Seed>(skyboxComputeClass, "Seed");
		ecs::Classes::Computed(
			skyboxComputeClass,
			EnumFieldProperty<SkyboxCompute, &SkyboxCompute::Shader, SkyboxComputeShaderEnum>("Shader")
		);
		ecs::Classes::Property<&SkyboxCompute::Enabled>(skyboxComputeClass, "Enabled");

		ecs::Classes::ClampedProperty<&CloudCompute::CellSize, 0.001f, 4.0f>(cloudComputeClass, "CellSize");
		ecs::Classes::ClampedProperty<&CloudCompute::Detail, 0.0f, 1.0f>(cloudComputeClass, "Detail");
		ecs::Classes::ClampedProperty<&CloudCompute::Height, 0.0f, 1.0f>(cloudComputeClass, "Height");
		ecs::Classes::ClampedProperty<&CloudCompute::Thickness, 0.001f, 1.0f>(cloudComputeClass, "Thickness");
		ecs::Classes::Property<&CloudCompute::Seed>(cloudComputeClass, "Seed");
		ecs::Classes::Property<&CloudCompute::Steps>(cloudComputeClass, "Steps");
		ecs::Classes::Computed(
			cloudComputeClass,
			EnumFieldProperty<CloudCompute, &CloudCompute::Shader, CloudComputeShaderEnum>("Shader")
		);
		ecs::Classes::Property<&CloudSource::Source>(cloudComputeClass, "Source");

		ecs::Classes::ClampedProperty<&CloudShadows::Strength, 0.0f, 1.0f>(cloudShadowsClass, "Strength");
		ecs::Classes::Property<&CloudShadows::DistanceMetres>(cloudShadowsClass, "DistanceMetres");
		ecs::Classes::Property<&CloudShadows::Resolution>(cloudShadowsClass, "Resolution");
		ecs::Classes::Property<&CloudShadows::Cascades>(cloudShadowsClass, "Cascades");
		ecs::Classes::Property<&CloudShadows::UpdateSeconds>(cloudShadowsClass, "UpdateSeconds");
		ecs::Classes::Property<&CloudShadows::Enabled>(cloudShadowsClass, "Enabled");

		ecs::Classes::Property<&Volume::Colour>(volumeClass, "Color");
		ecs::Classes::Property<&Volume::HalfExtent>(volumeClass, "Bounds");
		ecs::Classes::Property<&Volume::Density>(volumeClass, "Density");
		ecs::Classes::Property<&Volume::Extinction>(volumeClass, "Extinction");
		ecs::Classes::ClampedProperty<&Volume::Falloff, 0.0f, 1.0f>(volumeClass, "Falloff");
		ecs::Classes::Property<&Volume::NoiseScale>(volumeClass, "NoiseScale");
		ecs::Classes::ClampedProperty<&Volume::NoiseStrength, 0.0f, 1.0f>(volumeClass, "NoiseStrength");
		ecs::Classes::Property<&Volume::Steps>(volumeClass, "Steps");
		ecs::Classes::Property<&Volume::ShadowSteps>(volumeClass, "ShadowSteps");
		ecs::Classes::Property<&Volume::Seed>(volumeClass, "Seed");
		ecs::Classes::Computed(
			volumeClass, EnumFieldProperty<Volume, &Volume::Shape, VolumeShapeEnum>("Shape")
		);
		ecs::Classes::Property<&VolumeSource::Source>(volumeClass, "Source");
		ecs::Classes::Property<&VolumeSource::Scale>(volumeClass, "SourceScale");
		ecs::Classes::Computed(
			volumeClass,
			EnumFieldProperty<VolumeSource, &VolumeSource::Channel, VolumeChannelEnum>("SourceChannel")
		);
		ecs::Classes::Property<&Volume::Enabled>(volumeClass, "Enabled");

		using Particles = PhysicalParticleField;
		ecs::Classes::Property<&Particles::Colour>(physicalParticleClass, "Color");
		ecs::Classes::Property<&Particles::HalfExtent>(physicalParticleClass, "Bounds");
		ecs::Classes::Property<&Particles::Velocity>(physicalParticleClass, "Velocity");
		ecs::Classes::Property<&Particles::Acceleration>(physicalParticleClass, "Acceleration");
		ecs::Classes::ClampedProperty<&Particles::Rate, 0.0f, 100000.0f>(physicalParticleClass, "Rate");
		ecs::Classes::ClampedProperty<&Particles::Lifetime, 0.0f, MAX_PHYSICAL_PARTICLE_LIFETIME>(
			physicalParticleClass, "Lifetime"
		);
		ecs::Classes::ClampedProperty<&Particles::Radius, 0.001f, 4.0f>(physicalParticleClass, "Radius");
		ecs::Classes::ClampedProperty<&Particles::Restitution, 0.0f, 1.0f>(
			physicalParticleClass, "Restitution"
		);
		ecs::Classes::ClampedProperty<&Particles::Jitter, 0.0f, 1000.0f>(physicalParticleClass, "Jitter");
		ecs::Classes::Property<&Particles::MaxParticles>(physicalParticleClass, "MaxParticles");
		ecs::Classes::Property<&Particles::Seed>(physicalParticleClass, "Seed");
		ecs::Classes::Property<&Particles::MaxEventsPerStep>(physicalParticleClass, "MaxEventsPerStep");
		ecs::Classes::Computed(
			physicalParticleClass,
			EnumFieldProperty<Particles, &Particles::Response, ParticleResponseEnum>("Response")
		);
		ecs::Classes::Property<&Particles::Enabled>(physicalParticleClass, "Enabled");
		ecs::Classes::Property<&PhysicalParticleSource::Source>(physicalParticleClass, "Source");
		ecs::Classes::Property<&PhysicalParticleSource::Scale>(physicalParticleClass, "SourceScale");
		ecs::Classes::Computed(
			physicalParticleClass,
			EnumFieldProperty<PhysicalParticleSource, &PhysicalParticleSource::Channel, ParticleChannelEnum>(
				"SourceChannel"
			)
		);

		ecs::Classes::Property<&ShaderLens::Shader>(shaderLensClass, "Shader");
		ecs::Classes::ClampedProperty<&ShaderLens::Radius, 0.001f, 1000000.0f>(shaderLensClass, "Radius");
		ecs::Classes::ClampedProperty<&ShaderLens::InnerRadius, 0.0f, 1000000.0f>(
			shaderLensClass, "InnerRadius"
		);
		ecs::Classes::ClampedProperty<&ShaderLens::Falloff, 0.0f, 1.0f>(shaderLensClass, "Falloff");
		ecs::Classes::Property<&ShaderLens::Strength>(shaderLensClass, "Strength");
		ecs::Classes::Property<&ShaderLens::Spin>(shaderLensClass, "Spin");
		ecs::Classes::Property<&ShaderLens::Priority>(shaderLensClass, "Priority");
		ecs::Classes::Computed(
			shaderLensClass, EnumFieldProperty<ShaderLens, &ShaderLens::Shape, LensShapeEnum>("Shape")
		);
		ecs::Classes::Property<&ShaderLens::Enabled>(shaderLensClass, "Enabled");
		ecs::Classes::Property<&ShaderLens::InnerRadius>(gravitationalLensClass, "HorizonRadius");
		ecs::Classes::Property<&ShaderLens::Radius>(gravitationalLensClass, "WarpRadius");

		ecs::Classes::Property<&AtmosphereProcedural::PlanetRadius>(
			atmosphereProceduralClass, "PlanetRadius"
		);
		ecs::Classes::Property<&AtmosphereProcedural::Height>(atmosphereProceduralClass, "AtmosphereHeight");
		ecs::Classes::Property<&AtmosphereProcedural::Rayleigh>(atmosphereProceduralClass, "Rayleigh");
		ecs::Classes::Property<&AtmosphereProcedural::Mie>(atmosphereProceduralClass, "Mie");
		ecs::Classes::Property<&AtmosphereProcedural::Samples>(atmosphereProceduralClass, "Samples");
		ecs::Classes::Computed(
			atmosphereProceduralClass,
			EnumFieldProperty<
				AtmosphereProcedural,
				&AtmosphereProcedural::Shader,
				AtmosphereProceduralShaderEnum>("Shader")
		);
		ecs::Classes::Property<&AtmosphereProcedural::Enabled>(
			atmosphereProceduralClass, "ProceduralEnabled"
		);

		// **Every constraint property is on the base class**, which is the
		// other half of the one-component design: the six classes differ by
		// a prototype row and nothing else, so declaring a property per class
		// would be the same declaration seven times.
		//
		// **`Enum.ConstraintMotion` is registered and no property projects
		// it, deliberately.** The six modes are what the class already said,
		// and a per-axis property would let an author turn a
		// `HingeConstraint` into something that is not a hinge while it still
		// answered `IsA("HingeConstraint")`.
		ecs::Classes::Computed(constraintClass, Attachment0Property());
		ecs::Classes::Computed(constraintClass, Attachment1Property());
		ecs::Classes::Property<&Constraint::Enabled>(constraintClass, "Enabled");
		ecs::Classes::Property<&Constraint::Target>(constraintClass, "Target");
		ecs::Classes::Property<&Constraint::Stiffness>(constraintClass, "Stiffness");
		ecs::Classes::Property<&Constraint::Damping>(constraintClass, "Damping");
		ecs::Classes::Property<&Constraint::MaxForce>(constraintClass, "MaxForce");
		ecs::Classes::Property<&Constraint::MaxTorque>(constraintClass, "MaxTorque");
		ecs::Classes::Computed(constraintClass, LinearLimitProperty(false));
		ecs::Classes::Computed(constraintClass, LinearLimitProperty(true));
		ecs::Classes::Computed(constraintClass, AngularLimitProperty(false));
		ecs::Classes::Computed(constraintClass, AngularLimitProperty(true));

		ecs::Classes::Property<&JointInstance::Part0>(jointInstanceClass, "Part0");
		ecs::Classes::Property<&JointInstance::Part1>(jointInstanceClass, "Part1");
		ecs::Classes::Property<&JointInstance::C0>(jointInstanceClass, "C0");
		ecs::Classes::Property<&JointInstance::C1>(jointInstanceClass, "C1");
		ecs::Classes::Property<&JointInstance::Enabled>(jointInstanceClass, "Enabled");
		ecs::Classes::Computed(jointInstanceClass, RigidLinkActiveProperty<JointInstance>());

		ecs::Classes::Property<&WeldConstraint::Part0>(weldConstraintClass, "Part0");
		ecs::Classes::Property<&WeldConstraint::Part1>(weldConstraintClass, "Part1");
		ecs::Classes::Property<&WeldConstraint::Enabled>(weldConstraintClass, "Enabled");
		ecs::Classes::Computed(weldConstraintClass, RigidLinkActiveProperty<WeldConstraint>());

		// Still not declared, and for a reason rather than an oversight:
		// **`Surface::Material`**, which is what a part *feels* like. The
		// `Material` class above is what it looks like, and the two are
		// separate facts that share a word - a mirror-finish floor and a
		// rubber floor may share a surface and never a material. Binding both
		// under one script name would make one of them unreachable and the
		// other ambiguous.
		TagDeclaredProperties(pvInstance, core::Name("Transform"));
		for (const ecs::ClassId owner : {basePart, part, meshPart, skinnedMeshPart, spawnLocation})
			TagDeclaredProperties(owner, core::Name("Geometry"));
		TagDeclaredProperties(skinnedMeshPart, core::Name("Animation"));
		TagDeclaredProperties(cameraClass, core::Name("Camera"));
		TagDeclaredProperties(surfaceCameraClass, core::Name("Rendering"));
		TagDeclaredProperties(portalClass, core::Name("Geometry"));
		TagDeclaredProperties(soundClass, core::Name("Audio"));
		TagDeclaredProperties(attachmentClass, core::Name("Transform"));
		TagDeclaredProperties(materialClass, core::Name("PBR"));
		TagDeclaredProperties(humanoidClass, core::Name("Character"));
		TagDeclaredProperties(toolClass, core::Name("Behavior"));
		for (const ecs::ClassId owner : {lightClass, spotLightClass, surfaceLightClass})
			TagDeclaredProperties(owner, core::Name("Lighting"));
		for (const ecs::ClassId owner :
			 {valueBase,
			  stringValue,
			  localizationTable,
			  boolValue,
			  cframeValue,
			  color3Value,
			  intValue,
			  numberValue,
			  objectValue,
			  vector3Value})
			TagDeclaredProperties(owner, core::Name("Data"));
		for (const ecs::ClassId owner :
			 {boneClass, rigKeypointClass, animationBufferClass, animationClass, animatorClass, trackClass})
			TagDeclaredProperties(owner, core::Name("Animation"));
		for (const ecs::ClassId owner :
			 {atmosphereClass,
			  atmosphereComponentClass,
			  atmosphereProceduralClass,
			  cloudComputeClass,
			  skyboxTexturesClass,
			  skyboxComputeClass})
			TagDeclaredProperties(owner, core::Name("Environment"));
		for (const ecs::ClassId owner : {volumeClass, shaderLensClass, gravitationalLensClass})
			TagDeclaredProperties(owner, core::Name("Rendering"));
		for (const ecs::ClassId owner :
			 {constraintClass,
			  ballClass,
			  hingeClass,
			  prismaticClass,
			  cylindricalClass,
			  ropeClass,
			  springClass,
			  jointInstanceClass,
			  weldClass,
			  weldConstraintClass})
			TagDeclaredProperties(owner, core::Name("Physics"));
		TagDeclaredProperties(vectorField2D, core::Name("Physics"));
		TagDeclaredProperties(vectorField3D, core::Name("Physics"));
		TagDeclaredProperties(gpuParticleFieldClass, core::Name("Particles"));

		for (const std::string_view property : {"CFrame", "Position", "Orientation", "PivotOffset"})
			TagProperty(pvInstance, property, "Transform");
		TagProperty(basePart, "Size", "Geometry");
		for (const std::string_view property : {"Color", "Transparency", "LocalTransparency"})
			TagProperty(basePart, property, "Appearance");
		for (const std::string_view property :
			 {"AffordanceId", "AffordanceKind", "AffordanceEnabled", "Locked"})
			TagProperty(basePart, property, "Behavior");
		for (const std::string_view property :
			 {"Visible",
			  "RenderFeatureEnableMask",
			  "RenderFeatureDisableMask",
			  "ComputeEffectNode",
			  "PostProcessEffectNode",
			  "CastShadow"})
			TagProperty(basePart, property, "Rendering");
		for (const std::string_view property :
			 {"CanCollide",
			  "CanTouch",
			  "CanQuery",
			  "Anchored",
			  "Kinematic",
			  "CollisionGroup",
			  "CollisionShape",
			  "CollisionGeometry",
			  "Mass",
			  "CustomPhysicalProperties",
			  "Density",
			  "Friction",
			  "Elasticity",
			  "LinearDamping",
			  "AngularDamping"})
			TagProperty(basePart, property, "Physics");
		for (const std::string_view property :
			 {"AlphaCutoff", "AlphaMode", "SurfaceColor", "EmissiveTint", "EmissiveStrength"})
			TagProperty(basePart, property, "PBR");
		TagProperty(basePart, "ResampleMode", "Rendering");

		for (const std::string_view property :
			 {"TextureID",
			  "NormalMap",
			  "RoughnessMap",
			  "MetalnessMap",
			  "OcclusionMap",
			  "HeightMap",
			  "EmissiveMap",
			  "PackedPbrMap",
			  "SpecularFactor",
			  "TransmissionFactor",
			  "IndexOfRefraction",
			  "Thickness",
			  "PackedRoughnessChannel",
			  "PackedOcclusionChannel",
			  "PackedHeightChannel",
			  "PackedMetalnessChannel"})
			TagProperty(meshPart, property, "PBR");
		for (const std::string_view property :
			 {"SkinShading",
			  "SkinShadingMode",
			  "SubsurfaceProfile",
			  "ScatterRadius",
			  "ThicknessMap",
			  "SecondaryRoughnessMap",
			  "SecondaryRoughness",
			  "SecondarySpecular",
			  "PackedSecondarySpecularChannel",
			  "Clearcoat",
			  "ClearcoatRoughness",
			  "PackedClearcoatChannel"})
			TagProperty(meshPart, property, "Skin");
		for (const std::string_view property : {"MeshId", "TrianglesCount"})
			TagProperty(meshPart, property, "Geometry");
		for (const std::string_view property :
			 {"Lod1MeshId",
			  "Lod2MeshId",
			  "Lod3MeshId",
			  "Lod1Ratio",
			  "Lod2Ratio",
			  "Lod3Ratio",
			  "LodTargetQuadArea",
			  "LodBillboardTexture",
			  "Lod1Distance",
			  "Lod2Distance",
			  "Lod3Distance",
			  "AutoLod1MeshId",
			  "AutoLod2MeshId",
			  "AutoLod3MeshId",
			  "AutoLod1Ratio",
			  "AutoLod2Ratio",
			  "AutoLod3Ratio",
			  "AutoLodTargetQuadArea",
			  "AutoLodBillboardTexture",
			  "AutoLodLevels",
			  "AutoLodStrategy",
			  "CustomLod1MeshId",
			  "CustomLod2MeshId",
			  "CustomLod3MeshId",
			  "CustomLod1Ratio",
			  "CustomLod2Ratio",
			  "CustomLod3Ratio",
			  "CustomLodTargetQuadArea",
			  "CustomLodBillboardTexture",
			  "CustomLodLevels"})
			TagProperty(meshPart, property, "LOD");
		for (const std::string_view property : {"TeamColor", "Forced"})
			TagProperty(spawnLocation, property, "Player");
		for (const std::string_view property : {"Neutral", "Enabled"})
			TagProperty(spawnLocation, property, "Behavior");

		for (const std::string_view property :
			 {"CameraSubject",
			  "CameraType",
			  "CameraSubjectAutomatic",
			  "NearPlaneZ",
			  "FarPlaneZ",
			  "MaxImageWidth",
			  "MaxImageHeight",
			  "ImageWidth",
			  "ImageHeight"})
			TagProperty(cameraClass, property, "Camera");
		for (const std::string_view property : {"RenderFeatureEnableMask", "RenderFeatureDisableMask"})
			TagProperty(cameraClass, property, "Rendering");
		TagProperty(cameraClass, "SurfaceSize", "Geometry");
		TagProperty(surfaceCameraClass, "ImageTransparency", "Appearance");
		TagProperty(surfaceCameraClass, "FPS", "Rendering");
		for (const std::string_view property : {"Face", "Effect"})
			TagProperty(surfaceCameraClass, property, "Rendering");
		TagProperty(surfaceCameraClass, "TagFilter", "Behavior");
		for (const std::string_view property : {"Destination", "DestinationWorld"})
			TagProperty(portalClass, property, "Identity");
		for (const std::string_view property : {"Enabled", "Bidirectional"})
			TagProperty(portalClass, property, "Behavior");
		TagProperty(materialClass, "MaterialId", "Identity");
		TagProperty(materialClass, "Shader", "Rendering");
		for (const std::string_view property : {"Enabled", "Shadows"})
			TagProperty(lightClass, property, "Behavior");
		for (const std::string_view property : {"Angle", "Face"}) {
			TagProperty(spotLightClass, property, "Lighting");
			TagProperty(surfaceLightClass, property, "Lighting");
		}
		TagProperty(toolClass, "Grip", "Transform");
		for (const std::string_view property : {"RigId", "JointCount"})
			TagProperty(skinnedMeshPart, property, "Animation");
		for (const std::string_view property : {"Color"}) {
			TagProperty(atmosphereClass, property, "Lighting");
			TagProperty(cloudComputeClass, property, "Lighting");
		}
		for (const std::string_view property : {"Enabled"}) {
			TagProperty(cloudComputeClass, property, "Behavior");
			TagProperty(skyboxTexturesClass, property, "Behavior");
			TagProperty(skyboxComputeClass, property, "Behavior");
			TagProperty(volumeClass, property, "Behavior");
			TagProperty(shaderLensClass, property, "Behavior");
			TagProperty(atmosphereProceduralClass, "ProceduralEnabled", "Behavior");
		}
		for (const std::string_view property : {"ZenithColor", "HorizonColor", "GroundColor"})
			TagProperty(skyboxComputeClass, property, "Lighting");
		TagProperty(volumeClass, "Bounds", "Geometry");
	}

}
