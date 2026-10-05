#include "ImagePreviewPanel.hpp"

#include "ImageGraphComposerHost.hpp"
#include "ImageGraphGroupHost.hpp"
#include "ImageGraphPreview.hpp"
#include "ImageGraphPreviewResult.hpp"

#include <engine/assets/Texture.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/render/Renderer.hpp>

#include <algorithm>
#include <imgui.h>

namespace studio::detail {
	using namespace engine::imagegraph;
	namespace {
		constexpr uint64_t MaximumTextures = 16 * 1024 * 1024;
		constexpr uint64_t MaximumRetained = Limits::MaximumEvaluationBytes / 4;
		constexpr size_t MaximumPreviews = 16;
		struct BoundedPreviewProvider final : HostNodeProvider {
			ImageGraphPreviewProvider &Provider;
			uint64_t Maximum;
			bool PcxMessages(
				std::string_view node, std::span<const PcxMessage> messages, std::string &failure
			) override {
				return Provider.PcxMessages(node, messages, failure);
			}
			BoundedPreviewProvider(ImageGraphPreviewProvider &provider, uint64_t maximum)
				: Provider(provider), Maximum(maximum) {}
			bool Capture(
				const HostNodeInvocation &invocation, HostNodeCapture &capture, std::string &failure
			) override {
				auto bounded = invocation;
				bounded.MaximumOperationBytes = std::min(bounded.MaximumOperationBytes, Maximum);
				return Provider.Capture(bounded, capture, failure);
			}
		};
	}
	bool MakeImagePreviewDocument(const Document &source, std::string_view node, Document &preview) {
		const auto found = std::find_if(source.Nodes.begin(), source.Nodes.end(), [&](const Node &value) {
			return value.Id == node && value.Type == "pc.graph_preview";
		});
		if (found == source.Nodes.end()) return false;
		const auto linked = std::find_if(source.Links.begin(), source.Links.end(), [&](const Link &link) {
			return link.ToNode == node && link.ToPort == "surface";
		});
		if (linked == source.Links.end()) return false;
		preview = source;
		preview.Outputs = {{"embedded-preview", linked->FromNode, linked->FromPort}};
		return true;
	}
	void ImagePreviewPanel::Attach(nodegraph::Canvas &canvas, const ImageGraphCanvasIds &ids) {
		const auto measure = canvas.Signals.MeasureBody;
		const auto draw = canvas.Signals.DrawBody;
		canvas.Signals.MeasureBody = [measure](const nodegraph::Node &node) {
			return node.Type == "pc.graph_preview" ? nodegraph::BodySize{192, 144}
				   : measure					   ? measure(node)
												   : nodegraph::BodySize{};
		};
		canvas.Signals.DrawBody = [this,
								   &ids,
								   draw](const nodegraph::Node &node, const nodegraph::BodyFrame &frame) {
			if (node.Type != "pc.graph_preview") {
				if (draw) draw(node, frame);
				return;
			}
			const auto id = ids.ToDocument.find(node.Id);
			const auto row = id == ids.ToDocument.end() ? Rows.end() : Rows.find(id->second);
			auto *list = ImGui::GetWindowDrawList();
			const ImVec2 minimum{frame.X, frame.Y}, maximum{frame.X + frame.Width, frame.Y + frame.Height};
			list->AddRectFilled(minimum, maximum, IM_COL32(0, 0, 0, 255));
			list->PushClipRect(minimum, maximum, true);
			if (row != Rows.end() && row->second.Handle && row->second.Width && row->second.Height) {
				const auto &value = row->second;
				const float scale = std::min(frame.Width / value.Width, frame.Height / value.Height);
				const ImVec2 near{
					frame.X + (frame.Width - value.Width * scale) * .5f,
					frame.Y + (frame.Height - value.Height * scale) * .5f
				};
				list->AddImage(
					reinterpret_cast<ImTextureID>(value.Handle),
					near,
					{near.x + value.Width * scale, near.y + value.Height * scale}
				);
			} else {
				const char *message = row != Rows.end() && !row->second.Message.empty()
										  ? row->second.Message.c_str()
									  : Rows.size() >= MaximumPreviews ? "Node preview limit reached"
																	   : "Connect an image";
				list->AddText(
					nullptr,
					ImGui::GetFontSize() * frame.Scale,
					{frame.X + 8 * frame.Scale, frame.Y + 8 * frame.Scale},
					IM_COL32(255, 255, 255, 255),
					message,
					nullptr,
					frame.Width - 16 * frame.Scale
				);
			}
			list->PopClipRect();
		};
	}
	void ImagePreviewPanel::Refresh(
		const Document &document,
		EvaluationRequest sourceRequest,
		ImageGraphHost &host,
		engine::render::Renderer &renderer,
		uint64_t revision,
		uint64_t inputs,
		uint8_t playback
	) {
		ENGINE_PROFILE_CAT("image composer node previews", engine::core::ProfileCategory::Render);
		for (auto it = Rows.begin(); it != Rows.end();) {
			auto &row = it->second;
			if (row.Retired.IsValid()) {
				renderer.DropTexture(row.Retired);
				row.Retired = {};
				row.RetiredBytes = 0;
			}
			if (std::none_of(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
					return node.Id == it->first && node.Type == "pc.graph_preview";
				})) {
				if (row.Texture.IsValid()) renderer.DropTexture(row.Texture);
				it = Rows.erase(it);
			} else
				++it;
		}
		const ImagePreviewKey key{revision, inputs, RefreshGeneration, GetFrameTime(sourceRequest), playback};
		const auto retainedBytes = [&] {
			uint64_t bytes = Groups.Replay.RetainedBytes();
			for (const auto &[id, row] : Rows)
				bytes += row.Feedback.RetainedBytes() +
						 row.Observations.Receipts.RetainedPayloadBytes().value_or(MaximumRetained);
			return bytes;
		};
		uint64_t retained = retainedBytes(), textureBytes = 0;
		for (const auto &[id, row] : Rows) {
			textureBytes += row.TextureBytes + row.RetiredBytes;
		}
		for (const Node &node : document.Nodes) {
			if (node.Type != "pc.graph_preview") continue;
			if (!Rows.contains(node.Id) && Rows.size() >= MaximumPreviews) continue;
			auto &row = Rows[node.Id];
			row.Handle =
				row.Texture.IsValid() && row.Message.empty() ? renderer.TextureHandle(row.Texture) : nullptr;
			ImagePreviewKey target = key;
			if (row.Pending && row.Key.Revision == key.Revision && row.Key.Inputs == key.Inputs &&
				row.Key.Refresh == key.Refresh && row.Key.Playback == key.Playback)
				target.Frame = row.Key.Frame;
			if (row.Evaluated && row.Key == target && !row.Pending) continue;
			if (row.Key != target) {
				row.Observations.Clear();
				row.Pending = false;
			}
			row.Key = target;
			row.Evaluated = true;
			EvaluationRequest request = sourceRequest;
			(void)SetFrameTime(request, target.Frame);
			if (auto *composer = dynamic_cast<ImageGraphComposerHost *>(host.Composer))
				composer->Pending = false;
			Document preview;
			if (!MakeImagePreviewDocument(document, node.Id, preview)) {
				row.Message = "Connect an image";
				row.Handle = nullptr;
				continue;
			}
			Plan plan;
			Diagnostic diagnostic;
			if (Compile(preview, plan, diagnostic) != Status::Ok) {
				row.Message = diagnostic.Message;
				row.Handle = nullptr;
				continue;
			}
			row.Observations.Begin(GetFrameTime(request));
			retained = retainedBytes();
			ImageGraphPreviewProvider provider(host, row.Observations, request);
			const uint64_t receiptBytes =
				row.Observations.Receipts.RetainedPayloadBytes().value_or(MaximumRetained);
			BoundedPreviewProvider bounded(
				provider, MaximumRetained - std::min(retained - receiptBytes, MaximumRetained)
			);
			request.HostProvider = &bounded;
			request.MaximumImageDimension = IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION;
			const uint64_t maximum = MaximumRetained;
			const uint64_t old = row.Feedback.RetainedBytes();
			uint64_t allowance = retained - old < maximum ? maximum - (retained - old) : 0;
			ImageGraphPreviewValue result;
			Groups.BorrowedBytes =
				Limits::MaximumEvaluationBytes - MaximumRetained + retained - Groups.Replay.RetainedBytes();
			const bool prepared = Groups.Prepare(preview, plan, revision, request, diagnostic);
			retained = retainedBytes();
			const uint64_t headroom = maximum - std::min(retained, maximum);
			bounded.Maximum = std::min(bounded.Maximum, receiptBytes + headroom / 2);
			allowance = old + headroom / 2;
			const auto status = prepared ? PrepareImageGraphPreviewResult(
											   preview,
											   plan,
											   "embedded-preview",
											   request,
											   row.Feedback,
											   revision,
											   inputs,
											   result,
											   diagnostic,
											   allowance
										   )
										 : diagnostic.Code;
			retained = retainedBytes();
			if (status != Status::Ok) {
				row.Message = diagnostic.Message;
				const auto *composer = dynamic_cast<ImageGraphComposerHost *>(host.Composer);
				row.Pending = composer && composer->Pending;
				row.Handle = nullptr;
				continue;
			}
			row.Pending = false;
			row.Observations.Clear();
			retained = retainedBytes();
			const auto *image = std::get_if<Image>(&result);
			if (!image) {
				row.Message = "Image input required";
				row.Handle = nullptr;
				continue;
			}
			const auto hash = SurfaceHash(*image);
			if (row.Texture.IsValid() && row.Hash == hash && row.Width == image->Width &&
				row.Height == image->Height) {
				row.Message.clear();
				row.Handle = renderer.TextureHandle(row.Texture);
				continue;
			}
			engine::assets::TextureData texture;
			texture.Width = image->Width;
			texture.Height = image->Height;
			texture.Format = engine::assets::TextureFormat::RGBA8;
			if (!PrepareImageGraphPreviewRgba8(*image, texture.Pixels)) {
				row.Message = "Image cannot be displayed";
				row.Handle = nullptr;
				continue;
			}
			if (texture.Pixels.size() > MaximumTextures - std::min<uint64_t>(textureBytes, MaximumTextures)) {
				row.Message = "Node preview texture budget exceeded";
				row.Handle = nullptr;
				continue;
			}
			const engine::core::Name name(
				"studio.imagecomposer.node-preview/" + node.Id + "/" + std::to_string(row.Slot)
			);
			if (!renderer.AddTexture(name, texture)) {
				row.Message = "Preview upload failed";
				row.Handle = nullptr;
				continue;
			}
			textureBytes += texture.Pixels.size();
			row.RetiredBytes = row.TextureBytes;
			row.TextureBytes = texture.Pixels.size();
			row.Retired = row.Texture;
			row.Texture = name;
			row.Slot = row.Slot == 1 ? 2 : 1;
			row.Hash = hash;
			row.Width = image->Width;
			row.Height = image->Height;
			row.Handle = renderer.TextureHandle(name);
			row.Message.clear();
		}
	}
	void ImagePreviewPanel::Close(engine::render::Renderer &renderer) {
		for (auto &[id, row] : Rows) {
			if (row.Texture.IsValid()) renderer.DropTexture(row.Texture);
			if (row.Retired.IsValid()) renderer.DropTexture(row.Retired);
		}
		Rows.clear();
		Groups.Clear();
	}
}
