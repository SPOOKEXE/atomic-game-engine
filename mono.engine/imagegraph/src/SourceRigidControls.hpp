#pragma once
#include "NodeExecutors.hpp"

#include <engine/imagegraph/SourceRigid.hpp>

namespace engine::imagegraph::detail {
	struct RigidControlReader {
		NodeContext &Context;
		Vector2 Dimension;
		double Scale;
		Vector2 Pixels(std::string_view port, Vector2 fallback = {}) const;
		double PixelScalar(std::string_view port, double fallback = 0) const;
		SourceRigidWorld World() const;
		SourceRigidFrame Frame() const;
		SourceRigidJoint Joint(std::string id, std::string bodyA, std::string bodyB, bool motor) const;
		SourceRigidBody Wall(std::string id, uint32_t side) const;
		SourceRigidBody
		Fragment(std::string id, Vector2 sourcePosition, std::span<const Vector2> points) const;
		SourceRigidBody Object(std::string id, uint32_t width, uint32_t height) const;
		SourceRigidForce Force(std::string bodyId) const;
		SourceRigidExplosion Explosion(std::span<const std::string> bodyIds, bool forceApply) const;
		SourceRigidChange Override(std::string bodyId) const;
		SourceRigidStep Step(bool playing) const;
		SourceRigidBody Segment(std::string id, Vector2 start, Vector2 end) const;
		SourceRigidBody Sensor(std::string id) const;
	};
} // namespace engine::imagegraph::detail
