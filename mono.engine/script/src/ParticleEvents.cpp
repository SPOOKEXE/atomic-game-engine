#include <engine/ecs/Store.hpp>
#include <engine/scene/environment/PhysicalParticleField.hpp>
#include <engine/script/ParticleEvents.hpp>

namespace engine::script {
	namespace {
		ScriptValue Number(double value) {
			ScriptValue out(ValueTag::Number);
			out.Number = value;
			return out;
		}

		ScriptValue Vector(core::Vector3 value) {
			ScriptValue out(ValueTag::Vector3);
			out.Vector = value;
			return out;
		}

		ScriptValue Text(std::string text) {
			ScriptValue out(ValueTag::String);
			out.Text = std::move(text);
			return out;
		}

		const char *ResponseName(scene::ParticleResponse response) {
			switch (response) {
			case scene::ParticleResponse::Bounce:
				return "Bounce";
			case scene::ParticleResponse::Stick:
				return "Stick";
			case scene::ParticleResponse::Die:
				return "Die";
			}
			return "Bounce";
		}
	}

	std::vector<scene::ParticleCollision> TakeParticleCollisions(const ecs::Store &store, uint64_t &cursor) {
		std::vector<scene::ParticleCollision> out;
		const auto *log = store.Resource<scene::ParticleCollisionLog>();
		if (!log) {
			if (cursor == 0) cursor = 1;
			return out;
		}
		if (cursor == 0) {
			cursor = log->NextSequence;
			return out;
		}
		for (const scene::ParticleCollision &collision : log->Records) {
			if (collision.Sequence < cursor || !store.Alive(collision.Field)) continue;
			out.push_back(collision);
		}
		cursor = log->NextSequence;
		return out;
	}

	ScriptValue ParticleCollisionPayload(const scene::ParticleCollision &collision) {
		const float speed = collision.Velocity.Magnitude();
		const core::Vector3 direction = speed > 0.0f ? collision.Velocity / speed : core::Vector3{};

		ScriptValue colour(ValueTag::Color3);
		colour.Colour = collision.Colour;
		ScriptValue components(ValueTag::Map);
		components.Entries = {
			{"Radius", Number(collision.Radius)},
			{"Color", colour},
			{"Response", Text(ResponseName(collision.Response))},
		};

		ScriptValue out(ValueTag::Map);
		out.Entries = {
			{"Position", Vector(collision.Position)},
			{"Direction", Vector(direction)},
			{"Velocity", Vector(collision.Velocity)},
			{"Normal", Vector(collision.Normal)},
			{"Speed", Number(speed)},
			{"Age", Number(collision.AgeSeconds)},
			{"ParticleId", Number(static_cast<double>(collision.ParticleId))},
			{"ParticleComponents", components},
		};
		return out;
	}
}
