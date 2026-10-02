#pragma once

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/ComposerLuaHost.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/imagegraphexport/GraphFileHost.hpp>

#include <algorithm>
#include <array>
#include <new>

namespace studio::detail {
	inline bool ImageGraphFileReadType(std::string_view type) {
		constexpr std::string_view types[] = {
			"pc.image",
			"pc.image_sequence",
			"pc.image_animated",
			"pc.csv_file_read",
			"pc.json_file_read",
			"pc.xml_file_read",
			"pc.text_file_read",
			"pc.byte_file_read",
			"pc.ora_file_read",
			"pc.krita_file_read",
			"pc.ase_file_read",
			"pc.ase_layer",
			"pc.ase_tag",
			"pc.ase_tileset",
			"pc.gmroom",
			"pc.3_d_mesh_obj",
			"pc.3_d_mesh_json"
		};
		return std::find(std::begin(types), std::end(types), type) != std::end(types);
	}

	inline bool ImageGraphFileUsesOwnedContent(std::string_view type) {
		return type == "pc.ase_layer" || type == "pc.ase_tag" || type == "pc.ase_tileset";
	}
	inline bool ImageGraphFileNeedsPrimary(std::string_view type) {
		return !ImageGraphFileUsesOwnedContent(type) && type != "pc.image_sequence" &&
			   type != "pc.image_animated";
	}

	inline std::optional<uint64_t>
	ImageGraphCaptureCloneBytes(const engine::imagegraph::HostNodeCapture &capture) {
		using namespace engine::imagegraph;
		uint64_t bytes = sizeof(HostNodeCapture);
		const auto add = [&](uint64_t amount) {
			if (amount > Limits::MaximumEvaluationBytes - bytes) return false;
			bytes += amount;
			return true;
		};
		const auto node = NodeClonePayloadBytes(capture.Authored);
		if (!node || !add(*node) || !add(capture.Failure.size() + 16)) return std::nullopt;
		for (const auto &values :
			 {std::span<const AuthoredValue>(capture.Inputs),
			  std::span<const AuthoredValue>(capture.Outputs)})
			for (const auto &value : values) {
				const auto data = ValueClonePayloadBytes(value.Data);
				if (!data || !add(sizeof(AuthoredValue) + value.Port.size() + 16) || !add(*data))
					return std::nullopt;
			}
		for (const auto &binding : capture.InputImages)
			if (!add(sizeof(HostImageBinding) + binding.Port.size() + 16)) return std::nullopt;
		for (const auto &image : capture.Images)
			if (!add(sizeof(HostCapturedImage) + image.Port.size() + 16) || !add(image.Data.Pixels.size()))
				return std::nullopt;
		for (const auto &array : capture.ImageArrays) {
			if (!add(sizeof(HostCapturedImageArray) + array.Port.size() + 16)) return std::nullopt;
			for (const auto &image : array.Frames)
				if (!add(sizeof(Image)) || !add(image.Pixels.size())) return std::nullopt;
		}
		return bytes;
	}

	// Granted file observations remain immutable until their controls change or the
	// host explicitly refreshes them.
	struct ImageGraphHost final : engine::imagegraph::HostNodeProvider {
		using CaptureRecord = engine::imagegraph::HostNodeCapture;
		struct CachedFile {
			CaptureRecord Capture;
			std::vector<engine::imagegraphexport::GraphFileGrant> Grants;
			uint64_t Bytes = 0;
			std::optional<engine::imagegraph::SurfaceFormat> OutputFormat;
			int64_t Interpolation = 1;
		};
		engine::imagegraph::ComposerLuaHost *Lua = nullptr;
		std::span<const engine::imagegraphexport::GraphFileGrant> Grants;
		std::array<std::optional<CachedFile>, 64> Files;
		uint64_t RetainedBytes = sizeof(Files);

		void ResetFiles() {
			for (auto &file : Files)
				file.reset();
			RetainedBytes = sizeof(Files);
		}
		void RefreshFile(std::string_view nodeId) {
			for (auto &file : Files)
				if (file && file->Capture.Authored.Id == nodeId) {
					RetainedBytes -= file->Bytes;
					file.reset();
				}
		}
		bool PcxMessages(
			std::string_view node,
			std::span<const engine::imagegraph::PcxMessage> messages,
			std::string &failure
		) override {
			if (Lua) return Lua->PcxMessages(node, messages, failure);
			failure = "PCX notifications require the Studio Lua host";
			return false;
		}
		bool Capture(
			const engine::imagegraph::HostNodeInvocation &invocation,
			CaptureRecord &output,
			std::string &failure
		) override try {
			using namespace engine::imagegraph;
			if (invocation.Authored.Type == "pc.lua_compute" ||
				invocation.Authored.Type == "pc.lua_surface") {
				if (Lua) {
					if (RetainedBytes >= invocation.MaximumOperationBytes) {
						failure = "Studio file observations leave no Lua evaluation budget";
						return false;
					}
					HostNodeInvocation bounded = invocation;
					bounded.MaximumOperationBytes -= RetainedBytes;
					return Lua->Capture(bounded, output, failure);
				}
				failure = "Studio Lua host is unavailable";
				return false;
			}
			if (!ImageGraphFileReadType(invocation.Authored.Type)) {
				failure = "Studio has no granted provider for this host node";
				return false;
			}
			ENGINE_PROFILE_CAT("image composer file host", engine::core::ProfileCategory::Engine);
			if (ImageGraphFileUsesOwnedContent(invocation.Authored.Type)) {
				if (RetainedBytes >= invocation.MaximumOperationBytes) {
					failure = "Studio file observations leave no derived sprite budget";
					return false;
				}
				HostNodeInvocation bounded = invocation;
				bounded.MaximumOperationBytes -= RetainedBytes;
				engine::imagegraphexport::GraphFileHost reader(
					Grants, engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle)
				);
				return reader.Capture(bounded, output, failure);
			}
			if (ImageGraphFileNeedsPrimary(invocation.Authored.Type) &&
				std::none_of(Grants.begin(), Grants.end(), [&](const auto &grant) {
					return grant.NodeId == invocation.Authored.Id && !grant.Write && grant.Resource.empty();
				})) {
				failure = "Grant the node's exact primary file before reading it";
				return false;
			}
			const auto policy = engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle);
			for (const auto &grant : Grants)
				if (grant.NodeId == invocation.Authored.Id &&
					(grant.Write || !policy.AllowsName(grant.File.string()))) {
					failure = "Studio file grant violates the current read policy";
					return false;
				}
			const auto sameGrants = [&](const CachedFile &file) {
				size_t index = 0;
				for (const auto &grant : Grants) {
					if (grant.NodeId != invocation.Authored.Id) continue;
					if (index >= file.Grants.size()) return false;
					const auto &recorded = file.Grants[index++];
					if (recorded.NodeId != grant.NodeId || recorded.File != grant.File ||
						recorded.Write != grant.Write || recorded.Resource != grant.Resource)
						return false;
				}
				return index == file.Grants.size();
			};
			const auto sameImages = [&](const CaptureRecord &capture) {
				if (capture.InputImages.size() != invocation.Images.size()) return false;
				for (size_t index = 0; index < invocation.Images.size(); ++index) {
					const auto &image = invocation.Images[index];
					if (!image.Data || capture.InputImages[index].Port != image.Port ||
						capture.InputImages[index].Hash != SurfaceHash(*image.Data))
						return false;
				}
				return true;
			};
			for (const auto &file : Files)
				if (file && sameGrants(*file) && file->OutputFormat == invocation.OutputFormat &&
					file->Interpolation == invocation.Interpolation &&
					(!(invocation.Authored.Type.starts_with("pc.ase_") ||
					   invocation.Authored.Type == "pc.image_animated") ||
					 (file->Capture.Tick == invocation.Request.Tick &&
					  file->Capture.Subframe == invocation.Request.Subframe &&
					  file->Capture.NegativeFrame == invocation.Request.NegativeFrame)) &&
					file->Capture.Authored == invocation.Authored &&
					std::ranges::equal(file->Capture.Inputs, invocation.Inputs) &&
					sameImages(file->Capture)) {
					if (RetainedBytes > invocation.MaximumOperationBytes ||
						file->Bytes > invocation.MaximumOperationBytes - RetainedBytes) {
						failure = "Studio file observation clone exceeds the evaluation budget";
						return false;
					}
					CaptureRecord staged = file->Capture;
					staged.Tick = invocation.Request.Tick;
					staged.Subframe = invocation.Request.Subframe;
					staged.NegativeFrame = invocation.Request.NegativeFrame;
					output = std::move(staged);
					failure.clear();
					return true;
				}
			auto slot = std::find_if(Files.begin(), Files.end(), [](const auto &file) { return !file; });
			if (slot == Files.end()) {
				failure = "Studio file observations exceed the 64 control-state limit; "
						  "refresh a node";
				return false;
			}
			if (RetainedBytes >= invocation.MaximumOperationBytes) {
				failure = "Studio file observations exceed the evaluation budget";
				return false;
			}
			uint64_t grantBytes = 0;
			size_t grantCount = 0;
			for (const auto &grant : Grants) {
				if (grant.NodeId != invocation.Authored.Id) continue;
				const auto pathBytes = grant.File.native().size();
				if (++grantCount > 128 || pathBytes > 4096 || grant.Resource.size() > 4096 ||
					grant.NodeId.size() > Limits::MaximumTextBytes) {
					failure = "Studio file grants exceed the per-node bounds";
					return false;
				}
				grantBytes +=
					sizeof(grant) + grant.NodeId.size() + pathBytes * 4 + grant.Resource.size() + 64;
			}
			const uint64_t available = invocation.MaximumOperationBytes - RetainedBytes;
			if (grantBytes > available / 3) {
				failure = "Studio file grants exceed the observation budget";
				return false;
			}
			std::vector<engine::imagegraphexport::GraphFileGrant> selectedGrants;
			selectedGrants.reserve(grantCount);
			for (const auto &grant : Grants)
				if (grant.NodeId == invocation.Authored.Id) selectedGrants.push_back(grant);
			HostNodeInvocation bounded = invocation;
			bounded.MaximumOperationBytes = available - grantBytes * 3;
			engine::imagegraphexport::GraphFileHost reader(Grants, policy);
			CaptureRecord captured;
			if (!reader.Capture(bounded, captured, failure)) return false;
			auto bytes = ImageGraphCaptureCloneBytes(captured);
			if (!bytes || grantBytes > Limits::MaximumEvaluationBytes - *bytes ||
				*bytes + grantBytes > available / 3) {
				failure = "Studio file snapshot exceeds the retained observation budget";
				return false;
			}
			*bytes += grantBytes;
			CachedFile retained{
				captured, std::move(selectedGrants), *bytes, invocation.OutputFormat, invocation.Interpolation
			};
			*slot = std::move(retained);
			RetainedBytes += *bytes;
			output = std::move(captured);
			failure.clear();
			return true;
		} catch (const std::bad_alloc &) {
			failure = "Studio file observation allocation failed";
			return false;
		}
	};
} // namespace studio::detail
