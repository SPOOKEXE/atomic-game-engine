// Shared worlds, request and binding for the PortalImageRuntime test files.
#pragma once

#include "../RenderFixture.hpp"
#include "PortalRendererTerminalTrace.hpp"
#include "RendererTestHooks.hpp"
#include "portal/PortalCaptureTreeImport.hpp"
#include "portal/PortalResidentImages.hpp"
#include "portal/PortalShadowTransport.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/portal/PortalGeometry.hpp>
#include <engine/render/portal/PortalGeometryDraw.hpp>
#include <engine/render/portal/PortalImageDemand.hpp>
#include <engine/render/portal/PortalImageHost.hpp>
#include <engine/render/portal/PortalImageRuntime.hpp>
#include <engine/render/presentation/WorldPresentation.hpp>
#include <engine/render/resources/ShaderLibrary.hpp>
#include <engine/resources/Shaders.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/Interpolation.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/cameras/ActiveCamera.hpp>
#include <engine/scene/cameras/SurfaceCameras.hpp>
#include <engine/scene/cameras/Visibility.hpp>
#include <engine/scene/characters/Characters.hpp>
#include <engine/scene/content/Materials.hpp>
#include <engine/scene/content/ShaderLens.hpp>
#include <engine/scene/content/Shaders.hpp>
#include <engine/scene/editable/EditableImage.hpp>
#include <engine/scene/editable/EditableMesh.hpp>
#include <engine/scene/environment/Sunlight.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/HostLink.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <glm/packing.hpp>

#include <cstdlib>
#include <fstream>
#include <limits>

using namespace engine;
using namespace engine::render;

namespace portal_image_runtime_test {
	constexpr PortalImageInbox::Time START{};
	struct RuntimeWorlds {
		world::Universe Universe;
		world::WorldId Source = Universe.Create({.Name = core::Name("portal-source")});
		world::WorldId Destination = Universe.Create({.Name = core::Name("portal-destination")});
		world::PresentationAddress Replies;
		world::PresentationAddress Requests;
		RuntimeWorlds(uint32_t perEndpoint = 8) {
			world::PresentationLimits limits;
			limits.MessagesPerEndpoint = perEndpoint;
			REQUIRE(Universe.ConfigurePresentation(700, limits));
			Replies = Universe.OpenPresentation(Source, core::Name(PORTAL_REPLY_CHANNEL)).Address;
			Requests = Universe.OpenPresentation(Destination, core::Name(PORTAL_REQUEST_CHANNEL)).Address;
		}
	};
	inline PortalImageRequest Request() {
		PortalImageRequest request;
		request.Key.PortalKey = "Door";
		request.Key.CameraRevision = 1;
		request.Width = request.Height = 8;
		request.PixelBudget = 64;
		request.Frustum = {-0.1f, 0.1f, -0.1f, 0.1f, 0.1f, 100};
		request.ClipPlane = {0, 0, -1, -1};
		return request;
	}
	inline PortalImageBinding Binding() {
		PortalImageBinding binding;
		binding.World = 0;
		binding.WorldName = core::Name("portal-source");
		binding.Portal = core::Name("Door");
		return binding;
	}
}
using namespace portal_image_runtime_test;
