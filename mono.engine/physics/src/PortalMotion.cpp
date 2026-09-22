#include <engine/physics/PortalMotion.hpp>

namespace engine::physics {
	scene::Motion MapPortalMotion(
		const scene::SeamTransform &through,
		const core::Vector3 &sourcePosition,
		const scene::Motion &body,
		const PortalMouthMotion &source,
		const PortalMouthMotion &destination
	) {
		const core::Vector3 destinationPosition = through.Point(sourcePosition);
		const core::Vector3 sourceField =
			source.Linear + source.Angular.Cross(sourcePosition - source.Centre);
		const core::Vector3 destinationField =
			destination.Linear + destination.Angular.Cross(destinationPosition - destination.Centre);
		return {
			destinationField + through.Carry(body.Linear - sourceField),
			destination.Angular + through.Rotate(body.Angular - source.Angular)
		};
	}
}
