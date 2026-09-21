#include "PortalCaptureEntrance.hpp"

#include <engine/scene/Wire.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace engine::render {
	std::optional<int16_t>
	ResolvePortalEntrance(const PortalImageEntrance &entrance, std::span<const scene::PortalSeam> seams) {
		const glm::dvec3 centre{entrance.Centre[0], entrance.Centre[1], entrance.Centre[2]};
		const glm::dvec3 first{entrance.First[0], entrance.First[1], entrance.First[2]};
		const glm::dvec3 second{entrance.Second[0], entrance.Second[1], entrance.Second[2]};
		// Either endpoint may have passed through the scene wire codec. Its error
		// budget applies to corners as well as centres; ambiguous mouths are refused.
		const double tolerance =
			2 * std::sqrt(3.0) * scene::WIRE_POSITION_ERROR_METRES +
			2 * scene::WIRE_ROTATION_ERROR_RADIANS * (glm::length(first) + glm::length(second));
		const std::array corners{
			centre + first + second, centre + first - second, centre - first + second, centre - first - second
		};
		std::optional<int16_t> found;
		for (const auto &seam : seams) {
			if (!seam.Crosses || seam.Surface < 0 || seam.DestinationWorld.Text() != entrance.SourceWorld)
				continue;
			const glm::dvec3 origin{seam.Centre.X, seam.Centre.Y, seam.Centre.Z};
			const glm::dvec3 horizontal{seam.First.X, seam.First.Y, seam.First.Z};
			const glm::dvec3 vertical{seam.Second.X, seam.Second.Y, seam.Second.Z};
			const std::array candidates{
				origin + horizontal + vertical,
				origin + horizontal - vertical,
				origin - horizontal + vertical,
				origin - horizontal - vertical
			};
			std::array<bool, 4> matched{};
			bool equal = true;
			for (const auto &corner : corners) {
				bool present = false;
				for (size_t index = 0; index < candidates.size(); ++index) {
					if (!matched[index] && glm::length(corner - candidates[index]) <= tolerance) {
						matched[index] = true;
						present = true;
						break;
					}
				}
				equal = equal && present;
			}
			if (!equal) continue;
			if (found) return {};
			found = seam.Surface;
		}
		return found;
	}
}
