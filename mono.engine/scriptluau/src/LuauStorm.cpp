// The script-facing door to the reusable analytical storm resource.
//
// Storm state stays in physics. This service only marshals authored values and
// query results, so a UI and the fixed-step force system cannot drift apart.

#include "LuauBindings.hpp"

#include <engine/physics/Storm.hpp>
#include <engine/scene/Storm.hpp>

#include <cstring>
#include <string_view>

namespace engine::script {

	namespace {

		using core::Vector3;
		using physics::Storm;
		using scene::TornadoParameters;

		Storm CurrentStorm(lua_State *state) {
			const Storm *storm = physics::StormOf(*ContextOf(state).World);
			return storm == nullptr ? Storm{} : *storm;
		}

		bool RequireAuthority(lua_State *state) {
			if (ContextOf(state).Role.Server) return true;
			luaL_errorL(state, "Storm authoring is only available to an authoritative server script");
			return false;
		}

		void ReadFloat(lua_State *state, int table, const char *name, float &value) {
			lua_getfield(state, table, name);
			if (!lua_isnil(state, -1)) value = static_cast<float>(luaL_checknumber(state, -1));
			lua_pop(state, 1);
		}

		void ReadBool(lua_State *state, int table, const char *name, bool &value) {
			lua_getfield(state, table, name);
			if (!lua_isnil(state, -1)) value = lua_toboolean(state, -1) != 0;
			lua_pop(state, 1);
		}

		void ReadVector(lua_State *state, int table, const char *name, Vector3 &value) {
			lua_getfield(state, table, name);
			if (!lua_isnil(state, -1)) value = CheckVector3(state, -1);
			lua_pop(state, 1);
		}

		void ReadParameters(lua_State *state, int table, TornadoParameters &p) {
			ReadFloat(state, table, "Energy", p.Energy);
			ReadFloat(state, table, "CoreRadius", p.CoreRadius);
			ReadFloat(state, table, "InfluenceRadius", p.InfluenceRadius);
			ReadFloat(state, table, "PeakTangentialSpeed", p.PeakTangentialSpeed);
			ReadFloat(state, table, "PeakInflowSpeed", p.PeakInflowSpeed);
			ReadFloat(state, table, "PeakUpdraftSpeed", p.PeakUpdraftSpeed);
			ReadFloat(state, table, "PeakDowndraftSpeed", p.PeakDowndraftSpeed);
			ReadFloat(state, table, "SurfaceOutflowSpeed", p.SurfaceOutflowSpeed);
			ReadFloat(state, table, "PressureDrop", p.PressureDrop);
			ReadFloat(state, table, "Humidity", p.Humidity);
			ReadFloat(state, table, "RainRate", p.RainRate);
			ReadFloat(state, table, "Turbulence", p.Turbulence);
			ReadFloat(state, table, "GroundFriction", p.GroundFriction);
			ReadFloat(state, table, "DebrisDensity", p.DebrisDensity);
			ReadFloat(state, table, "VortexTightness", p.VortexTightness);
			ReadFloat(state, table, "TopHeight", p.TopHeight);
			ReadVector(state, table, "UpperWind", p.UpperWind);
			ReadVector(state, table, "TranslationVelocity", p.TranslationVelocity);
			ReadBool(state, table, "CounterClockwise", p.CounterClockwise);
		}

		void PushParameters(lua_State *state, const TornadoParameters &p) {
			lua_newtable(state);
			const auto number = [&](const char *name, float value) {
				lua_pushnumber(state, value);
				lua_setfield(state, -2, name);
			};
			number("Energy", p.Energy);
			number("CoreRadius", p.CoreRadius);
			number("InfluenceRadius", p.InfluenceRadius);
			number("PeakTangentialSpeed", p.PeakTangentialSpeed);
			number("PeakInflowSpeed", p.PeakInflowSpeed);
			number("PeakUpdraftSpeed", p.PeakUpdraftSpeed);
			number("PeakDowndraftSpeed", p.PeakDowndraftSpeed);
			number("SurfaceOutflowSpeed", p.SurfaceOutflowSpeed);
			number("PressureDrop", p.PressureDrop);
			number("Humidity", p.Humidity);
			number("RainRate", p.RainRate);
			number("Turbulence", p.Turbulence);
			number("GroundFriction", p.GroundFriction);
			number("DebrisDensity", p.DebrisDensity);
			number("VortexTightness", p.VortexTightness);
			number("TopHeight", p.TopHeight);
			*PushVector3(state) = p.UpperWind;
			lua_setfield(state, -2, "UpperWind");
			*PushVector3(state) = p.TranslationVelocity;
			lua_setfield(state, -2, "TranslationVelocity");
			lua_pushboolean(state, p.CounterClockwise);
			lua_setfield(state, -2, "CounterClockwise");
		}

		int Configure(lua_State *state) {
			if (!RequireAuthority(state)) return 0;
			luaL_checktype(state, 1, LUA_TTABLE);
			Storm storm = CurrentStorm(state);
			ReadParameters(state, 1, storm.State.Parameters);
			ReadVector(state, 1, "Position", storm.State.Position);
			ReadFloat(state, 1, "ElapsedSeconds", storm.State.ElapsedSeconds);
			ReadBool(state, 1, "LifecycleEnabled", storm.State.LifecycleEnabled);
			ReadBool(state, 1, "Enabled", storm.Enabled);
			physics::SetStorm(*ContextOf(state).World, storm);
			return 0;
		}

		int Preset(lua_State *state) {
			if (!RequireAuthority(state)) return 0;
			const std::string_view name(luaL_checkstring(state, 1));
			Storm storm = CurrentStorm(state);
			if (name.size() == 3 && name[0] == 'E' && name[1] == 'F' && name[2] >= '0' && name[2] <= '5') {
				storm.State.Parameters = scene::EfPreset(static_cast<scene::EfCategory>(name[2] - '0'));
			} else if (name.size() == 2 && name[0] == 'Q' && name[1] >= '0' && name[1] <= '5') {
				storm.State.Parameters = scene::QPreset(static_cast<scene::QCategory>(name[1] - '0'));
			} else {
				luaL_errorL(state, "Storm.Preset expects EF0 through EF5 or Q0 through Q5");
				return 0;
			}
			physics::SetStorm(*ContextOf(state).World, storm);
			return 0;
		}

		int Sample(lua_State *state) {
			const Storm storm = CurrentStorm(state);
			const Vector3 position = CheckVector3(state, 1);
			const scene::StormSample sample = scene::SampleTornadoField(
				storm.State.Parameters, storm.State.Position, position, storm.State.ElapsedSeconds
			);
			lua_newtable(state);
			*PushVector3(state) = sample.Velocity;
			lua_setfield(state, -2, "Velocity");
			lua_pushnumber(state, sample.Influence);
			lua_setfield(state, -2, "Influence");
			lua_pushnumber(state, sample.DamagePotential);
			lua_setfield(state, -2, "DamagePotential");
			lua_pushnumber(state, sample.Condensation);
			lua_setfield(state, -2, "Condensation");
			return 1;
		}

		int Snapshot(lua_State *state) {
			const Storm storm = CurrentStorm(state);
			lua_newtable(state);
			*PushVector3(state) = storm.State.Position;
			lua_setfield(state, -2, "Position");
			lua_pushnumber(state, storm.State.ElapsedSeconds);
			lua_setfield(state, -2, "ElapsedSeconds");
			lua_pushboolean(state, storm.State.LifecycleEnabled);
			lua_setfield(state, -2, "LifecycleEnabled");
			lua_pushboolean(state, storm.Enabled);
			lua_setfield(state, -2, "Enabled");
			PushParameters(state, storm.State.Parameters);
			lua_setfield(state, -2, "Parameters");
			return 1;
		}

		int Visibility(lua_State *state) {
			const Storm storm = CurrentStorm(state);
			const scene::VisibilityResult result = scene::QueryStormVisibility(
				storm.State, {CheckVector3(state, 1), static_cast<float>(luaL_checknumber(state, 2))}
			);
			lua_newtable(state);
			lua_pushnumber(state, result.Clarity);
			lua_setfield(state, -2, "Clarity");
			lua_pushnumber(state, result.EffectiveDistance);
			lua_setfield(state, -2, "EffectiveDistance");
			lua_pushnumber(state, result.RainObscuration);
			lua_setfield(state, -2, "Rain");
			lua_pushnumber(state, result.CondensationObscuration);
			lua_setfield(state, -2, "Condensation");
			return 1;
		}

		int Damage(lua_State *state) {
			const Storm storm = CurrentStorm(state);
			const scene::DamageResult result = scene::QueryStormDamage(storm.State, CheckVector3(state, 1));
			lua_newtable(state);
			*PushVector3(state) = result.WindVelocity;
			lua_setfield(state, -2, "Wind");
			lua_pushnumber(state, result.AerodynamicPressure);
			lua_setfield(state, -2, "DynamicPressure");
			lua_pushnumber(state, result.Potential);
			lua_setfield(state, -2, "Potential");
			lua_pushinteger(state, static_cast<int>(result.Band));
			lua_setfield(state, -2, "Band");
			return 1;
		}
	}

	void OpenStorm(lua_State *state) {
		lua_newtable(state);
		lua_pushcfunction(state, Configure, "Configure");
		lua_setfield(state, -2, "Configure");
		lua_pushcfunction(state, Preset, "Preset");
		lua_setfield(state, -2, "Preset");
		lua_pushcfunction(state, Sample, "Sample");
		lua_setfield(state, -2, "Sample");
		lua_pushcfunction(state, Snapshot, "Snapshot");
		lua_setfield(state, -2, "Snapshot");
		lua_pushcfunction(state, Visibility, "Visibility");
		lua_setfield(state, -2, "Visibility");
		lua_pushcfunction(state, Damage, "Damage");
		lua_setfield(state, -2, "Damage");
		lua_setglobal(state, "Storm");
	}
}
