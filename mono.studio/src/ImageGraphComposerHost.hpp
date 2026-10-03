#pragma once
#include <engine/assets/LocalStore.hpp>
#include <engine/imagegraph/HostCapture.hpp>
#include <engine/render/ComposerSurface.hpp>
#include <engine/render/Renderer.hpp>

#include <algorithm>

namespace studio::detail {
	// Renderer ownership is borrowed only for the synchronous panel draw.
	struct ImageGraphComposerHost final : engine::imagegraph::HostNodeProvider {
		engine::render::Renderer &Renderer;
		engine::core::Name Owner;
		const engine::assets::LocalPaths &Paths;
		std::vector<engine::core::Name> *CaptureNames = nullptr;
		std::string_view CapturePrefix = "studio.image-composer/";
		bool Synchronous = false;
		bool Pending = false;
		bool HavePendingJobs = false;
		ImageGraphComposerHost(
			engine::render::Renderer &renderer,
			engine::core::Name owner,
			const engine::assets::LocalPaths &paths,
			std::vector<engine::core::Name> *captureNames = nullptr
		)
			: Renderer(renderer), Owner(owner), Paths(paths), CaptureNames(captureNames) {}
		bool Capture(
			const engine::imagegraph::HostNodeInvocation &invocation,
			engine::imagegraph::HostNodeCapture &output,
			std::string &failure
		) override {
			using namespace engine;
			if (!Owner.IsValid()) {
				failure = "Composer shader evaluation requires an active world owner";
				return false;
			}
			const bool camera = invocation.Authored.Type == "pc.3_d_camera" ||
								invocation.Authored.Type == "pc.3_d_camera_set";
			if (camera || (invocation.Authored.Type == "pc.3_d_transform_image" ||
						   invocation.Authored.Type == "image.transform_3d")) {
				const core::Name name(std::string(CapturePrefix) + invocation.Authored.Id);
				if (CaptureNames &&
					std::find(CaptureNames->begin(), CaptureNames->end(), name) == CaptureNames->end()) {
					if (CaptureNames->size() >= 16) {
						failure = "Transform preview exceeds capture limit";
						return false;
					}
					CaptureNames->push_back(name);
				}
				bool pending = false;
				const bool captured = camera ? Renderer.CaptureSourceCamera3DAsync(
												   invocation, Owner, name, output, failure, &pending
											   )
											 : Renderer.CaptureTransformImage3DAsync(
												   invocation, Owner, name, output, failure, &pending
											   );
				Pending = Pending || pending;
				HavePendingJobs = HavePendingJobs || pending;
				return captured;
			}
			if (invocation.Authored.Type != "pc.hlsl") {
				failure = "Composer renderer host accepts HLSL nodes only";
				return false;
			}
			const auto selector = std::find_if(
				invocation.Authored.SourceProperties.begin(),
				invocation.Authored.SourceProperties.end(),
				[](const auto &value) { return value.Port == render::hlsl::COOKED_SELECTOR; }
			);
			const auto *name = selector == invocation.Authored.SourceProperties.end()
								   ? nullptr
								   : std::get_if<std::string>(&selector->Data);
			if (!name || name->size() != 72 || !name->ends_with(".ashader") ||
				!assets::ContentHash::FromHex(std::string_view(*name).substr(0, 64))) {
				failure = "Cook the selected shader before preview or export";
				return false;
			}
			const core::Name asset(*name);
			if (Renderer.ComposerShaderRevision(Owner, asset) == 0) {
				assets::ShaderData artifact;
				if (const auto error = render::hlsl::ReadArtifact(
						Paths.Baked, asset, artifact, invocation.MaximumOperationBytes
					)) {
					failure = *error;
					return false;
				}
				const auto bytes =
					assets::ShaderRetainedPayloadBytes(artifact, invocation.MaximumOperationBytes / 2);
				if (!bytes) {
					failure = "Composer artifact admission exceeds the evaluation byte budget";
					return false;
				}
				if (const auto error = Renderer.InstallComposerShader(Owner, asset, artifact)) {
					failure = *error;
					return false;
				}
			}
			if (Synchronous) return Renderer.CaptureComposerSurface(invocation, Owner, output, failure);
			const core::Name captureName(std::string(CapturePrefix) + invocation.Authored.Id);
			if (CaptureNames &&
				std::find(CaptureNames->begin(), CaptureNames->end(), captureName) == CaptureNames->end()) {
				if (CaptureNames->size() >= 16) {
					failure = "Composer preview exceeds its in-flight capture limit";
					return false;
				}
				CaptureNames->push_back(captureName);
			}
			bool pending = false;
			const bool captured = Renderer.CaptureComposerSurfaceAsync(
				invocation, Owner, captureName, output, failure, &pending
			);
			Pending = Pending || pending;
			HavePendingJobs = HavePendingJobs || pending;
			return captured;
		}
	};
	// Separate preview and export identities share the renderer's bounded receipt cache.
	struct ImageGraphComposerExportScope {
		ImageGraphComposerHost *Host = nullptr;
		std::vector<engine::core::Name> *PreviousNames = nullptr;
		std::string_view PreviousPrefix;
		bool PreviousSynchronous = false;
		explicit ImageGraphComposerExportScope(
			engine::imagegraph::HostNodeProvider *provider, std::vector<engine::core::Name> &names
		)
			: Host(dynamic_cast<ImageGraphComposerHost *>(provider)) {
			if (!Host) return;
			PreviousNames = Host->CaptureNames;
			PreviousPrefix = Host->CapturePrefix;
			PreviousSynchronous = Host->Synchronous;
			Host->CaptureNames = &names;
			Host->CapturePrefix = "studio.image-composer/export/";
			Host->Synchronous = false;
			Host->Pending = false;
		}
		~ImageGraphComposerExportScope() {
			if (!Host) return;
			Host->CaptureNames = PreviousNames;
			Host->CapturePrefix = PreviousPrefix;
			Host->Synchronous = PreviousSynchronous;
		}
	};

	struct ImageGraphComposerSynchronousScope {
		ImageGraphComposerHost *Host = nullptr;
		bool Previous = false;
		explicit ImageGraphComposerSynchronousScope(engine::imagegraph::HostNodeProvider *provider)
			: Host(dynamic_cast<ImageGraphComposerHost *>(provider)) {
			if (Host) {
				Previous = Host->Synchronous;
				Host->Synchronous = true;
			}
		}
		~ImageGraphComposerSynchronousScope() {
			if (Host) Host->Synchronous = Previous;
		}
	};

	struct ImageGraphComposerHostScope {
		engine::imagegraph::HostNodeProvider *&Slot;
		engine::imagegraph::HostNodeProvider *Previous;
		ImageGraphComposerHostScope(
			engine::imagegraph::HostNodeProvider *&slot, engine::imagegraph::HostNodeProvider &provider
		)
			: Slot(slot), Previous(slot) {
			Slot = &provider;
		}
		~ImageGraphComposerHostScope() {
			Slot = Previous;
		}
	};
}
