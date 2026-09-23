#include <engine/scene/CloudDensity.hpp>
#include <engine/scene/Storm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace engine::scene {

	namespace {
		float Saturate(float value) {
			return std::clamp(value, 0.0f, 1.0f);
		}

		float Noise(float x, float y, float z) {
			return std::sin(x * 1.37f + std::sin(y * 0.73f) + z * 0.41f) *
				   std::cos(z * 1.11f - x * 0.29f + y * 0.53f);
		}

		float Influence(const PreparedTornadoField &field, float radius) {
			return 1.0f /
				   (1.0f + std::pow(radius * field.InverseInfluenceRadius, field.Parameters.VortexTightness));
		}

		float EyeDecay(const PreparedTornadoField &field, float radius) {
			const float ratio = std::max(radius, 0.0f) * field.InverseCoreRadius / 0.34f;
			return std::exp(-(ratio * ratio));
		}

		float Tangential(const PreparedTornadoField &field, float radius, float influence) {
			const float x = std::max(radius, 0.0f) * field.InverseCoreRadius;
			return field.Parameters.PeakTangentialSpeed * field.EnergyScale * (2.0f * x) / (1.0f + x * x) *
				   influence;
		}

		float Inflow(const PreparedTornadoField &field, float radius, float influence) {
			const float x = std::max(radius, 0.0f) * field.InverseCoreRadius / 2.15f;
			return field.Parameters.PeakInflowSpeed * field.EnergyScale * field.PressureDrive * x *
				   std::exp(1.0f - x) * influence;
		}

		float Updraft(const PreparedTornadoField &field, float radius, float height, float eyeGate) {
			const float ratio = radius * field.InverseCoreRadius / 1.9f;
			const float radial = std::exp(-(ratio * ratio)) * eyeGate;
			const float ramp = 0.22f + 0.78f * Saturate(height / 18.0f);
			const float fraction = std::max(height, 0.0f) * field.InverseTopHeight;
			return field.Parameters.PeakUpdraftSpeed * field.EnergyScale * radial * ramp *
				   Saturate(1.0f - fraction * fraction * fraction);
		}

		float CoreSubsidence(const PreparedTornadoField &field, float height, float eyeDecay) {
			const float feed = Saturate((std::max(height, 0.0f) - 12.0f) / 45.0f);
			const float fraction = std::max(height, 0.0f) * field.InverseTopHeight;
			return field.Parameters.PeakDowndraftSpeed * field.EnergyScale * 0.18f * eyeDecay * feed *
				   Saturate(1.0f - fraction * fraction * fraction);
		}

		float
		DowndraftColumn(const PreparedTornadoField &field, float radius, float height, float influence) {
			const float center = field.Parameters.CoreRadius * 4.2f;
			const float width = field.Parameters.CoreRadius * 1.7f;
			const float ratio = (radius - center) / width;
			const float cloud =
				0.35f +
				0.65f * Saturate((Saturate(std::max(height, 0.0f) * field.InverseTopHeight) - 0.16f) / 0.56f);
			return field.Parameters.PeakDowndraftSpeed * field.EnergyScale * std::exp(-(ratio * ratio)) *
				   influence * cloud;
		}

		float Outflow(const PreparedTornadoField &field, float radius, float height, float influence) {
			const float center = field.Parameters.CoreRadius * 4.2f;
			const float width = field.Parameters.CoreRadius * 1.9f;
			const float ratio = (radius - center) / width;
			const float surface = 1.0f - Saturate((std::max(height, 0.0f) - 4.0f) / 34.0f);
			return field.Parameters.SurfaceOutflowSpeed * field.EnergyScale * std::exp(-(ratio * ratio)) *
				   influence * surface;
		}
	}

	TornadoParameters EfPreset(EfCategory category) {
		static constexpr std::array<TornadoParameters, 6> PRESETS{{
			{.Energy = .30f,
			 .CoreRadius = 22,
			 .InfluenceRadius = 170,
			 .PeakTangentialSpeed = 33.5f,
			 .PeakInflowSpeed = 12,
			 .PeakUpdraftSpeed = 25,
			 .PeakDowndraftSpeed = 14,
			 .SurfaceOutflowSpeed = 12,
			 .UpperWind = {10, 0, -3},
			 .PressureDrop = 25,
			 .Humidity = .65f,
			 .RainRate = .35f,
			 .Turbulence = 5,
			 .TranslationVelocity = {3, 0, 1},
			 .GroundFriction = .34f,
			 .DebrisDensity = .18f,
			 .VortexTightness = 1.45f,
			 .TopHeight = 240},
			{.Energy = .42f,
			 .CoreRadius = 26,
			 .InfluenceRadius = 200,
			 .PeakTangentialSpeed = 43.8f,
			 .PeakInflowSpeed = 17,
			 .PeakUpdraftSpeed = 32,
			 .PeakDowndraftSpeed = 18,
			 .SurfaceOutflowSpeed = 15,
			 .UpperWind = {13, 0, -4},
			 .PressureDrop = 38,
			 .Humidity = .70f,
			 .RainRate = .45f,
			 .Turbulence = 7,
			 .TranslationVelocity = {3.5f, 0, 1},
			 .GroundFriction = .32f,
			 .DebrisDensity = .28f,
			 .VortexTightness = 1.70f,
			 .TopHeight = 280},
			{.Energy = .56f,
			 .CoreRadius = 30,
			 .InfluenceRadius = 230,
			 .PeakTangentialSpeed = 55,
			 .PeakInflowSpeed = 23,
			 .PeakUpdraftSpeed = 42,
			 .PeakDowndraftSpeed = 23,
			 .SurfaceOutflowSpeed = 20,
			 .UpperWind = {16, 0, -5},
			 .PressureDrop = 54,
			 .Humidity = .76f,
			 .RainRate = .55f,
			 .Turbulence = 9.5f,
			 .TranslationVelocity = {4, 0, 1.2f},
			 .GroundFriction = .30f,
			 .DebrisDensity = .42f,
			 .VortexTightness = 1.95f,
			 .TopHeight = 320},
			{.Energy = .70f,
			 .CoreRadius = 34,
			 .InfluenceRadius = 270,
			 .PeakTangentialSpeed = 67.5f,
			 .PeakInflowSpeed = 30,
			 .PeakUpdraftSpeed = 55,
			 .PeakDowndraftSpeed = 30,
			 .SurfaceOutflowSpeed = 27,
			 .UpperWind = {20, 0, -7},
			 .PressureDrop = 72,
			 .Humidity = .82f,
			 .RainRate = .68f,
			 .Turbulence = 13,
			 .TranslationVelocity = {4.5f, 0, 1.5f},
			 .GroundFriction = .28f,
			 .DebrisDensity = .60f,
			 .VortexTightness = 2.25f,
			 .TopHeight = 370},
			{.Energy = .84f,
			 .CoreRadius = 39,
			 .InfluenceRadius = 320,
			 .PeakTangentialSpeed = 81.8f,
			 .PeakInflowSpeed = 39,
			 .PeakUpdraftSpeed = 69,
			 .PeakDowndraftSpeed = 38,
			 .SurfaceOutflowSpeed = 34,
			 .UpperWind = {25, 0, -9},
			 .PressureDrop = 92,
			 .Humidity = .88f,
			 .RainRate = .78f,
			 .Turbulence = 17,
			 .TranslationVelocity = {5.5f, 0, 1.8f},
			 .GroundFriction = .25f,
			 .DebrisDensity = .78f,
			 .VortexTightness = 2.65f,
			 .TopHeight = 430},
			{.Energy = 1,
			 .CoreRadius = 45,
			 .InfluenceRadius = 520,
			 .PeakTangentialSpeed = 98.3f,
			 .PeakInflowSpeed = 49,
			 .PeakUpdraftSpeed = 84,
			 .PeakDowndraftSpeed = 47,
			 .SurfaceOutflowSpeed = 42,
			 .UpperWind = {30, 0, -11},
			 .PressureDrop = 115,
			 .Humidity = .93f,
			 .RainRate = .88f,
			 .Turbulence = 22,
			 .TranslationVelocity = {7, 0, 2.2f},
			 .GroundFriction = .22f,
			 .DebrisDensity = .95f,
			 .VortexTightness = 3.10f,
			 .TopHeight = 500},
		}};
		return PRESETS[std::min(static_cast<size_t>(category), PRESETS.size() - 1)];
	}

	TornadoParameters QPreset(QCategory category) {
		static constexpr std::array<TornadoParameters, 6> PRESETS{{
			{.Energy = 1,
			 .CoreRadius = 60,
			 .InfluenceRadius = 650,
			 .PeakTangentialSpeed = 123,
			 .PeakInflowSpeed = 65,
			 .PeakUpdraftSpeed = 110,
			 .PeakDowndraftSpeed = 65,
			 .SurfaceOutflowSpeed = 58,
			 .UpperWind = {40, 0, -15},
			 .PressureDrop = 140,
			 .Humidity = .95f,
			 .RainRate = .90f,
			 .Turbulence = 30,
			 .TranslationVelocity = {10, 0, 3},
			 .GroundFriction = .20f,
			 .DebrisDensity = .96f,
			 .VortexTightness = 3.60f,
			 .TopHeight = 650},
			{.Energy = 1,
			 .CoreRadius = 80,
			 .InfluenceRadius = 800,
			 .PeakTangentialSpeed = 156.5f,
			 .PeakInflowSpeed = 85,
			 .PeakUpdraftSpeed = 145,
			 .PeakDowndraftSpeed = 85,
			 .SurfaceOutflowSpeed = 75,
			 .UpperWind = {52, 0, -20},
			 .PressureDrop = 175,
			 .Humidity = .96f,
			 .RainRate = .92f,
			 .Turbulence = 42,
			 .TranslationVelocity = {15, 0, 4},
			 .GroundFriction = .18f,
			 .DebrisDensity = .97f,
			 .VortexTightness = 4.20f,
			 .TopHeight = 800},
			{.Energy = 1,
			 .CoreRadius = 105,
			 .InfluenceRadius = 950,
			 .PeakTangentialSpeed = 201.2f,
			 .PeakInflowSpeed = 110,
			 .PeakUpdraftSpeed = 185,
			 .PeakDowndraftSpeed = 110,
			 .SurfaceOutflowSpeed = 95,
			 .UpperWind = {65, 0, -26},
			 .PressureDrop = 215,
			 .Humidity = .97f,
			 .RainRate = .94f,
			 .Turbulence = 55,
			 .TranslationVelocity = {20, 0, 6},
			 .GroundFriction = .16f,
			 .DebrisDensity = .98f,
			 .VortexTightness = 5,
			 .TopHeight = 1000},
			{.Energy = 1,
			 .CoreRadius = 140,
			 .InfluenceRadius = 1120,
			 .PeakTangentialSpeed = 257.1f,
			 .PeakInflowSpeed = 140,
			 .PeakUpdraftSpeed = 225,
			 .PeakDowndraftSpeed = 140,
			 .SurfaceOutflowSpeed = 120,
			 .UpperWind = {78, 0, -32},
			 .PressureDrop = 260,
			 .Humidity = .98f,
			 .RainRate = .96f,
			 .Turbulence = 70,
			 .TranslationVelocity = {28, 0, 8},
			 .GroundFriction = .14f,
			 .DebrisDensity = .99f,
			 .VortexTightness = 6.5f,
			 .TopHeight = 1220},
			{.Energy = 1,
			 .CoreRadius = 190,
			 .InfluenceRadius = 1340,
			 .PeakTangentialSpeed = 335.3f,
			 .PeakInflowSpeed = 175,
			 .PeakUpdraftSpeed = 265,
			 .PeakDowndraftSpeed = 175,
			 .SurfaceOutflowSpeed = 150,
			 .UpperWind = {92, 0, -38},
			 .PressureDrop = 310,
			 .Humidity = .99f,
			 .RainRate = .98f,
			 .Turbulence = 88,
			 .TranslationVelocity = {40, 0, 11},
			 .GroundFriction = .12f,
			 .DebrisDensity = 1,
			 .VortexTightness = 9,
			 .TopHeight = 1500},
			{.Energy = 1,
			 .CoreRadius = 250,
			 .InfluenceRadius = 1560,
			 .PeakTangentialSpeed = 447,
			 .PeakInflowSpeed = 210,
			 .PeakUpdraftSpeed = 300,
			 .PeakDowndraftSpeed = 210,
			 .SurfaceOutflowSpeed = 180,
			 .UpperWind = {105, 0, -45},
			 .PressureDrop = 360,
			 .Humidity = 1,
			 .RainRate = 1,
			 .Turbulence = 105,
			 .TranslationVelocity = {55, 0, 15},
			 .GroundFriction = .10f,
			 .DebrisDensity = 1,
			 .VortexTightness = 12,
			 .TopHeight = 1800},
		}};
		return PRESETS[std::min(static_cast<size_t>(category), PRESETS.size() - 1)];
	}

	TornadoParameters SanitizeTornadoParameters(TornadoParameters parameters) {
		parameters.Energy = Saturate(parameters.Energy);
		parameters.CoreRadius = std::max(parameters.CoreRadius, 1.0f);
		parameters.InfluenceRadius = std::max(parameters.InfluenceRadius, parameters.CoreRadius * 1.25f);
		parameters.PeakTangentialSpeed = std::max(parameters.PeakTangentialSpeed, 0.0f);
		parameters.PeakInflowSpeed = std::max(parameters.PeakInflowSpeed, 0.0f);
		parameters.PeakUpdraftSpeed = std::max(parameters.PeakUpdraftSpeed, 0.0f);
		parameters.PeakDowndraftSpeed = std::max(parameters.PeakDowndraftSpeed, 0.0f);
		parameters.SurfaceOutflowSpeed = std::max(parameters.SurfaceOutflowSpeed, 0.0f);
		parameters.UpperWind.Y = 0.0f;
		parameters.PressureDrop = std::max(parameters.PressureDrop, 0.0f);
		parameters.Humidity = Saturate(parameters.Humidity);
		parameters.RainRate = Saturate(parameters.RainRate);
		parameters.Turbulence = std::max(parameters.Turbulence, 0.0f);
		parameters.GroundFriction = Saturate(parameters.GroundFriction);
		parameters.DebrisDensity = Saturate(parameters.DebrisDensity);
		parameters.VortexTightness = std::clamp(parameters.VortexTightness, 0.75f, 18.0f);
		parameters.TopHeight = std::max(parameters.TopHeight, 50.0f);
		return parameters;
	}

	PreparedTornadoField PrepareTornadoField(const TornadoParameters &parameters) {
		PreparedTornadoField field;
		field.Parameters = SanitizeTornadoParameters(parameters);
		field.EnergyScale = std::sqrt(field.Parameters.Energy);
		const float pressure = field.Parameters.PressureDrop * (0.30f + 0.70f * field.Parameters.Energy);
		field.PressureDrive = std::clamp(0.55f + 0.45f * std::sqrt(pressure / 72.0f), 0.55f, 1.45f);
		field.RotationSign = field.Parameters.CounterClockwise ? 1.0f : -1.0f;
		field.InverseCoreRadius = 1.0f / field.Parameters.CoreRadius;
		field.InverseInfluenceRadius = 1.0f / field.Parameters.InfluenceRadius;
		field.InverseTopHeight = 1.0f / field.Parameters.TopHeight;
		field.UpperWindLength = std::hypot(field.Parameters.UpperWind.X, field.Parameters.UpperWind.Z);
		field.InverseUpperWindLength = field.UpperWindLength > 1.0e-5f ? 1.0f / field.UpperWindLength : 0.0f;
		return field;
	}

	float TangentialWind(const TornadoParameters &parameters, float radius) {
		const auto field = PrepareTornadoField(parameters);
		return Tangential(field, radius, Influence(field, radius));
	}
	float InflowWind(const TornadoParameters &parameters, float radius) {
		const auto field = PrepareTornadoField(parameters);
		return Inflow(field, radius, Influence(field, radius));
	}
	float UpdraftWind(const TornadoParameters &parameters, float radius, float height) {
		const auto field = PrepareTornadoField(parameters);
		return Updraft(field, radius, height, 1.0f - EyeDecay(field, radius));
	}
	float DowndraftWind(const TornadoParameters &parameters, float radius, float height) {
		const auto field = PrepareTornadoField(parameters);
		return DowndraftColumn(field, radius, height, Influence(field, radius)) *
				   Saturate(std::max(height, 0.0f) / 18.0f) +
			   CoreSubsidence(field, height, EyeDecay(field, radius));
	}
	float OutflowWind(const TornadoParameters &parameters, float radius, float height) {
		const auto field = PrepareTornadoField(parameters);
		return Outflow(field, radius, height, Influence(field, radius));
	}

	StormSample SampleTornadoField(
		const TornadoParameters &parameters,
		const core::Vector3 &tornadoPosition,
		const core::Vector3 &worldPosition,
		float timeSeconds
	) {
		return SampleTornadoField(
			PrepareTornadoField(parameters), tornadoPosition, worldPosition, timeSeconds
		);
	}

	StormSample SampleTornadoField(
		const PreparedTornadoField &field,
		const core::Vector3 &center,
		const core::Vector3 &position,
		float timeSeconds
	) {
		const auto &parameters = field.Parameters;
		const core::Vector3 offset = position - center;
		const float radius = std::hypot(offset.X, offset.Z);
		const core::Vector3 radial = radius > 1.0e-5f
										 ? core::Vector3{offset.X / radius, 0.0f, offset.Z / radius}
										 : core::Vector3::XAxis;
		const core::Vector3 tangent{-radial.Z * field.RotationSign, 0.0f, radial.X * field.RotationSign};
		StormSample sample;
		sample.Influence = Influence(field, radius);
		const float pressureRatio = radius * field.InverseCoreRadius / 1.25f;
		const float eyeDecay = EyeDecay(field, radius);
		const float eyeGate = 1.0f - eyeDecay;
		sample.PressureDeficit = parameters.PressureDrop * parameters.Energy *
								 std::exp(-(pressureRatio * pressureRatio)) * sample.Influence;
		sample.TangentialSpeed = Tangential(field, radius, sample.Influence);
		sample.InflowSpeed = Inflow(field, radius, sample.Influence);
		sample.UpdraftSpeed = Updraft(field, radius, offset.Y, eyeGate);
		float flank = 1.0f;
		if (field.UpperWindLength > 1.0e-5f) {
			const float alignment = (radial.X * parameters.UpperWind.X + radial.Z * parameters.UpperWind.Z) *
									field.InverseUpperWindLength;
			const float downshear = Saturate(alignment * .5f + .5f);
			flank = .08f + .92f * downshear * downshear;
		}
		const float clearance = Saturate(std::max(offset.Y, 0.0f) / 18.0f);
		sample.DowndraftSpeed =
			DowndraftColumn(field, radius, offset.Y, sample.Influence) * clearance * flank +
			CoreSubsidence(field, offset.Y, eyeDecay);
		sample.OutflowSpeed = Outflow(field, radius, offset.Y, sample.Influence) * flank;
		sample.VerticalSpeed = sample.UpdraftSpeed - sample.DowndraftSpeed;
		const float height = Saturate(std::max(offset.Y, 0.0f) * field.InverseTopHeight);
		const float upper = Saturate((height - .72f) / .22f);
		const float upperSmooth = upper * upper * (3.0f - 2.0f * upper);
		sample.UpperOutflowSpeed = parameters.PeakUpdraftSpeed * field.EnergyScale * .65f * upperSmooth *
								   sample.Influence * (.35f + .65f * eyeGate);
		const float shear = Saturate((height - .18f) / .64f);
		const float shearSmooth = shear * shear * (3.0f - 2.0f * shear);
		sample.ShearVelocity = parameters.UpperWind * (field.EnergyScale * shearSmooth);
		sample.Condensation = Saturate(
			parameters.Humidity * (parameters.PressureDrop / 100.0f) *
			(sample.UpdraftSpeed / std::max(parameters.PeakUpdraftSpeed, 1.0f)) * 1.65f
		);
		const float destructive =
			sample.TangentialSpeed * sample.TangentialSpeed + sample.InflowSpeed * sample.InflowSpeed;
		sample.DamagePotential =
			Saturate((destructive / (105.0f * 105.0f)) * .86f + (sample.PressureDeficit / 120.0f) * .14f);
		const float phase = timeSeconds * 1.7f;
		const core::Vector3 turbulence{
			Noise(position.Z * .026f, position.Y * .026f, phase),
			Noise(position.X * .026f + 11.0f, position.Z * .026f, phase * .71f),
			Noise(position.Y * .026f, position.X * .026f - 7.0f, phase * 1.13f)
		};
		const float nearGround = 1.0f - parameters.GroundFriction * (1.0f - Saturate(offset.Y / 22.0f));
		const float coreTurbulence = .12f + .88f * eyeGate;
		const float boundary = Saturate(std::max(offset.Y, 0.0f) / 18.0f);
		const float variation =
			std::sin(position.X * .031f + timeSeconds * 1.7f) * std::cos(position.Z * .027f - timeSeconds);
		const float rollPhase = radius * .075f - timeSeconds * .9f + variation * 1.4f;
		const float rollSpeed =
			(parameters.Turbulence * sample.Influence * 1.10f + sample.OutflowSpeed * .65f) *
			(1.0f - boundary) * (1.0f - boundary);
		const float radialRoll =
			std::sin(rollPhase) * std::cos(std::numbers::pi_v<float> * boundary) * rollSpeed;
		const float verticalRoll =
			-std::cos(rollPhase) * std::sin(std::numbers::pi_v<float> * boundary) * rollSpeed * 1.25f;
		const float wallDepth = std::max(parameters.GroundFriction * 8.0f, .05f);
		const float wall = Saturate(std::max(offset.Y, 0.0f) / wallDepth);
		const float slip = wall * wall * (3.0f - 2.0f * wall);
		const core::Vector3 horizontal =
			(tangent * sample.TangentialSpeed - radial * sample.InflowSpeed) * nearGround +
			radial * (sample.OutflowSpeed + radialRoll + sample.UpperOutflowSpeed);
		sample.Velocity = horizontal * slip + core::Vector3{0.0f, sample.VerticalSpeed + verticalRoll, 0.0f} +
						  sample.ShearVelocity +
						  turbulence * (parameters.Turbulence * sample.Influence * coreTurbulence) +
						  parameters.TranslationVelocity;
		return sample;
	}

	float LifecycleEnergy(float elapsedSeconds) {
		const float phase = std::fmod(std::max(elapsedSeconds, 0.0f), 90.0f) / 90.0f;
		if (phase < .18f) {
			const float x = phase / .18f;
			return .12f + .63f * x * x * (3.0f - 2.0f * x);
		}
		if (phase < .68f) {
			const float x = (phase - .18f) / .50f;
			return .75f + .18f * std::sin(x * std::numbers::pi_v<float>);
		}
		const float x = (phase - .68f) / .32f;
		return .75f * (1.0f - x) + .08f * x;
	}

	void AdvanceStorm(StormState &storm, float tickSeconds) {
		const float tick = std::max(tickSeconds, 0.0f);
		storm.Position = storm.Position + storm.Parameters.TranslationVelocity * tick;
		storm.ElapsedSeconds += tick;
		if (storm.LifecycleEnabled) storm.Parameters.Energy = LifecycleEnergy(storm.ElapsedSeconds);
	}

	VisibilityResult QueryStormVisibility(const StormState &storm, const VisibilityQuery &query) {
		const float distance = std::clamp(query.ViewDistance, 0.0f, 5000.0f);
		const PreparedTornadoField prepared = PrepareTornadoField(storm.Parameters);
		const StormSample field =
			SampleTornadoField(prepared, storm.Position, query.Position, storm.ElapsedSeconds);
		const TornadoParameters parameters = SanitizeTornadoParameters(storm.Parameters);
		const float rain =
			Saturate(parameters.RainRate * parameters.Humidity * (.28f + field.Influence * .72f));
		const float cloud =
			WindLineCloudDensity(prepared, query.Position - storm.Position, storm.ElapsedSeconds);
		const float condensation = Saturate(
			field.Condensation * (.45f + field.Influence * .55f) + cloud * (.38f + field.Influence * .32f)
		);
		const float obscuration =
			Saturate((rain * .42f + condensation * .72f) * (1.0f - std::exp(-distance / 240.0f)));
		const float clarity = 1.0f - std::min(obscuration, .96f);
		return {clarity, distance * clarity, rain, condensation};
	}

	DamageResult QueryStormDamage(const StormState &storm, const core::Vector3 &position) {
		const StormSample field =
			SampleTornadoField(storm.Parameters, storm.Position, position, storm.ElapsedSeconds);
		const float pressure = .5f * 1.225f * field.Velocity.MagnitudeSquared();
		DamageBand band = DamageBand::Safe;
		if (field.DamagePotential >= .82f || pressure >= 6200.0f)
			band = DamageBand::Catastrophic;
		else if (field.DamagePotential >= .52f || pressure >= 3200.0f)
			band = DamageBand::Destructive;
		else if (field.DamagePotential >= .20f || pressure >= 900.0f)
			band = DamageBand::Caution;
		return {field.Velocity, pressure, field.DamagePotential, band};
	}
}
