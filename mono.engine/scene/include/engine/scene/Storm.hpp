#pragma once

// A reusable analytical tornado field.
//
// The field is a value, not an ECS component or a physics object. Every later
// consumer samples the same prepared equations: rigid-body forces, particles,
// cloud density, sound, visibility, and damage. Keeping that seam pure makes a
// fixed-tick storm replayable and lets callers retain a prepared field for a
// whole tick without holding a world pointer.
//
// @tier L7 · shared

#include <engine/core/types/Vector3.hpp>

#include <cstdint>

namespace engine::scene {

	// Authored values for a tornado's analytical field.
	struct TornadoParameters {
		float Energy = 0.72f;								 // Normalized intensity.
		float CoreRadius = 34.0f;							 // Radius of peak rotation.
		float InfluenceRadius = 260.0f;						 // Radius where the field decays.
		float PeakTangentialSpeed = 92.0f;					 // Maximum circular wind speed.
		float PeakInflowSpeed = 34.0f;						 // Maximum inward wind speed.
		float PeakUpdraftSpeed = 58.0f;						 // Maximum eyewall updraft speed.
		float PeakDowndraftSpeed = 32.0f;					 // Maximum rain-band downdraft speed.
		float SurfaceOutflowSpeed = 28.0f;					 // Maximum near-ground outward wind speed.
		core::Vector3 UpperWind{18.0f, 0.0f, -6.0f};		 // Horizontal upper-level shear.
		float PressureDrop = 72.0f;							 // Central pressure-deficit scale.
		float Humidity = 0.82f;								 // Normalized atmospheric humidity.
		float RainRate = 0.68f;								 // Normalized precipitation rate.
		float Turbulence = 13.0f;							 // Deterministic turbulence amplitude.
		core::Vector3 TranslationVelocity{4.0f, 0.0f, 1.5f}; // Storm movement in world units per second.
		float GroundFriction = 0.28f;						 // Near-ground wind attenuation.
		float DebrisDensity = 0.65f;						 // Normalized debris availability.
		float VortexTightness = 2.25f;						 // Falloff exponent outside the core.
		float TopHeight = 360.0f;							 // Vertical extent of the storm.
		bool CounterClockwise = true;						 // Rotation direction when viewed from above.
	};

	// Enhanced Fujita severity presets.
	enum class EfCategory : uint8_t { EF0, EF1, EF2, EF3, EF4, EF5 };
	// Quadratic severity presets above the EF scale.
	enum class QCategory : uint8_t { Q0, Q1, Q2, Q3, Q4, Q5 };

	// Wind and derived quantities at one world position.
	struct StormSample {
		core::Vector3 Velocity;			// Combined local wind velocity.
		float PressureDeficit = 0.0f;	// Local pressure reduction.
		float TangentialSpeed = 0.0f;	// Circular wind component.
		float InflowSpeed = 0.0f;		// Inward radial wind component.
		float UpdraftSpeed = 0.0f;		// Rising eyewall component.
		float DowndraftSpeed = 0.0f;	// Descending rain-band component.
		float OutflowSpeed = 0.0f;		// Near-ground radial outflow.
		float UpperOutflowSpeed = 0.0f; // High-altitude radial outflow.
		float VerticalSpeed = 0.0f;		// Net vertical velocity.
		core::Vector3 ShearVelocity;	// High-altitude ambient shear contribution.
		float Influence = 0.0f;			// Normalized radial field influence.
		float Condensation = 0.0f;		// Normalized visible condensation.
		float DamagePotential = 0.0f;	// Normalized structural damage potential.
	};

	// Sanitized parameters with inverses and values shared by every sample in a tick.
	struct PreparedTornadoField {
		TornadoParameters Parameters;		 // Sanitized authored values.
		float EnergyScale = 0.0f;			 // Square-root intensity scale.
		float PressureDrive = 0.0f;			 // Pressure-derived inflow scale.
		float RotationSign = 1.0f;			 // Sign for circular rotation.
		float InverseCoreRadius = 1.0f;		 // Cached reciprocal core radius.
		float InverseInfluenceRadius = 1.0f; // Cached reciprocal influence radius.
		float InverseTopHeight = 1.0f;		 // Cached reciprocal storm height.
		float UpperWindLength = 0.0f;		 // Horizontal upper-wind magnitude.
		float InverseUpperWindLength = 0.0f; // Zero-safe reciprocal upper-wind magnitude.
	};

	// A mutable storm trajectory. `AdvanceStorm` is the fixed-tick owner.
	struct StormState {
		TornadoParameters Parameters;  // Authored values sampled by queries.
		core::Vector3 Position;		   // Current storm center in world space.
		float ElapsedSeconds = 0.0f;   // Elapsed lifecycle time.
		bool LifecycleEnabled = false; // Whether each tick sets lifecycle energy.
	};

	// Inputs for a local storm visibility query.
	struct VisibilityQuery {
		core::Vector3 Position;		 // Viewer world position.
		float ViewDistance = 100.0f; // Unobstructed view distance.
	};

	// Visibility reduced by rain and condensation.
	struct VisibilityResult {
		float Clarity = 1.0f;				  // Fraction of clear sight remaining.
		float EffectiveDistance = 100.0f;	  // Sight distance after obscuration.
		float RainObscuration = 0.0f;		  // Rain contribution before distance integration.
		float CondensationObscuration = 0.0f; // Condensation contribution before distance integration.
	};

	// Discrete damage interpretation of a field sample.
	enum class DamageBand : uint8_t { Safe, Caution, Destructive, Catastrophic };

	// Damage-relevant wind values at a world position.
	struct DamageResult {
		core::Vector3 WindVelocity;			// Local wind used for force calculations.
		float AerodynamicPressure = 0.0f;	// Dynamic pressure in simulation units.
		float Potential = 0.0f;				// Normalized structural damage potential.
		DamageBand Band = DamageBand::Safe; // Discrete gameplay damage band.
	};

	// Clamps authored values to the field's stable domain.
	//
	// @return A safe copy of `parameters`.
	[[nodiscard]] TornadoParameters SanitizeTornadoParameters(TornadoParameters parameters);
	// Returns an Enhanced Fujita severity preset.
	//
	// @return The requested preset, clamped to EF5.
	[[nodiscard]] TornadoParameters EfPreset(EfCategory category);
	// Returns a quadratic severity preset.
	//
	// @return The requested preset, clamped to Q5.
	[[nodiscard]] TornadoParameters QPreset(QCategory category);
	// Prepares a field for repeated samples within one tick.
	//
	// @return Sanitized parameters with cached shared quantities.
	[[nodiscard]] PreparedTornadoField PrepareTornadoField(const TornadoParameters &parameters);
	// Samples a prepared field at a world position and time.
	//
	// @return Local wind and derived field quantities.
	[[nodiscard]] StormSample SampleTornadoField(
		const PreparedTornadoField &field,
		const core::Vector3 &tornadoPosition,
		const core::Vector3 &worldPosition,
		float timeSeconds
	);
	// Prepares and samples a field at a world position and time.
	//
	// @return Local wind and derived field quantities.
	[[nodiscard]] StormSample SampleTornadoField(
		const TornadoParameters &parameters,
		const core::Vector3 &tornadoPosition,
		const core::Vector3 &worldPosition,
		float timeSeconds
	);

	// Returns the circular wind speed at a radial distance.
	//
	// @return Nonnegative tangential wind speed.
	[[nodiscard]] float TangentialWind(const TornadoParameters &parameters, float radius);
	// Returns the inward wind speed at a radial distance.
	//
	// @return Nonnegative inflow wind speed.
	[[nodiscard]] float InflowWind(const TornadoParameters &parameters, float radius);
	// Returns the eyewall updraft speed at radius and height.
	//
	// @return Nonnegative updraft wind speed.
	[[nodiscard]] float UpdraftWind(const TornadoParameters &parameters, float radius, float height);
	// Returns the combined core and rain-band downdraft speed.
	//
	// @return Nonnegative downdraft wind speed.
	[[nodiscard]] float DowndraftWind(const TornadoParameters &parameters, float radius, float height);
	// Returns the near-ground radial outflow speed.
	//
	// @return Nonnegative outflow wind speed.
	[[nodiscard]] float OutflowWind(const TornadoParameters &parameters, float radius, float height);

	// Returns the repeating ninety-second lifecycle intensity.
	//
	// @return Normalized lifecycle energy.
	[[nodiscard]] float LifecycleEnergy(float elapsedSeconds);
	// Advances a storm trajectory by a nonnegative tick duration.
	void AdvanceStorm(StormState &storm, float tickSeconds);
	// Computes sight loss through the local storm field.
	//
	// @return Rain and condensation visibility reduction.
	[[nodiscard]] VisibilityResult
	QueryStormVisibility(const StormState &storm, const VisibilityQuery &query);
	// Computes gameplay damage data from the local storm field.
	//
	// @return Wind, pressure, potential, and a discrete damage band.
	[[nodiscard]] DamageResult QueryStormDamage(const StormState &storm, const core::Vector3 &position);
}
