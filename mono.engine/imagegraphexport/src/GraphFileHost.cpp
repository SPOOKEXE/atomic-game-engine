#include "GraphFileHostIO.hpp"
#include "GraphRasterHost.hpp"
#include "GraphSpriteHost.hpp"
#include "GraphTextFileHost.hpp"
#include "GraphTileHost.hpp"

#include <engine/bake/LayeredImage.hpp>
#include <engine/imagegraph/WavExport.hpp>
#include <engine/imagegraphexport/GraphDirectoryHost.hpp>
#include <engine/imagegraphexport/GraphFileHost.hpp>
#include <engine/imagegraphexport/GraphMeshHost.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>

namespace engine::imagegraphexport {
	namespace {
		using namespace engine::imagegraph;
		const Value *Input(const HostNodeInvocation &invocation, std::string_view port) {
			const auto found =
				std::find_if(invocation.Inputs.begin(), invocation.Inputs.end(), [&](const auto &value) {
					return value.Port == port;
				});
			return found == invocation.Inputs.end() ? nullptr : &found->Data;
		}
		Image Surface(const engine::assets::TextureData &source) {
			Image result;
			result.Width = source.Width;
			result.Height = source.Height;
			result.Pixels.reserve(source.Pixels.size());
			for (auto pixel : source.Pixels)
				result.Pixels.push_back(std::to_integer<uint8_t>(pixel));
			result.Hash = SurfaceHash(result);
			return result;
		}
		StructValue LayerContent(engine::bake::LayeredImage &source) {
			StructValue result;
			result.Data.emplace();
			auto &fields = result.Data->Fields;
			fields.emplace_back("width", int64_t{source.Width});
			fields.emplace_back("height", int64_t{source.Height});
			ArrayValue layers;
			layers.ElementType = ValueType::Struct;
			for (auto &layer : source.Layers) {
				StructValue value;
				value.Data.emplace();
				value.Data->Fields = {
					{"name", layer.Name},
					{"x", int64_t{layer.X}},
					{"y", int64_t{layer.Y}},
					{"image", SurfaceValue{Surface(layer.Pixels)}}
				};
				layers.Elements.emplace_back(std::move(value));
			}
			fields.emplace_back("layerData", std::move(layers));
			return result;
		}
		bool
		WriteFile(const std::filesystem::path &path, std::span<const uint8_t> bytes, std::string &failure) {
			static std::atomic<uint64_t> sequence{0};
			const auto directory =
				path.parent_path() /
				(".graph-file-" +
				 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
				 std::to_string(sequence.fetch_add(1)));
			std::error_code error;
			if (!std::filesystem::create_directory(directory, error) || error) {
				failure = "cannot create exclusive host write staging directory";
				return false;
			}
			struct Cleanup {
				std::filesystem::path Path;
				~Cleanup() {
					std::error_code e;
					std::filesystem::remove_all(Path, e);
				}
			} cleanup{directory};
			const auto target = directory / "content";
			std::ofstream stream(target, std::ios::binary);
			stream.write(
				reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())
			);
			stream.close();
			if (!stream) {
				failure = "cannot write complete host file";
				return false;
			}
			std::filesystem::rename(target, path, error);
			if (error) {
				failure = "cannot publish complete host file";
				return false;
			}
			return true;
		}
	}
	bool WriteGraphHostFile(
		const std::filesystem::path &path, std::span<const uint8_t> bytes, std::string &failure
	) {
		return WriteFile(path, bytes, failure);
	}
	bool PublishGraphHostFile(
		const GraphFileGrant &grant,
		const engine::assets::ContentPolicy &policy,
		std::span<const std::byte> bytes,
		uint64_t maximumBytes,
		std::string &failure
	) {
		if (!grant.Write || !grant.Resource.empty() || !policy.AllowsName(grant.File.string()) ||
			bytes.size() > maximumBytes ||
			maximumBytes > engine::imagegraph::Limits::MaximumEvaluationBytes) {
			failure = "host publication requires an exact write grant and bounded bytes";
			return false;
		}
		try {
			return WriteFile(
				grant.File, {reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()}, failure
			);
		} catch (const std::filesystem::filesystem_error &) {
			failure = "host publication filesystem operation failed";
			return false;
		}
	}
	GraphFileHost::GraphFileHost(
		std::span<const GraphFileGrant> grants,
		engine::assets::ContentPolicy policy,
		std::span<const GraphDirectoryGrant> directories,
		std::span<const GraphImageCacheLayoutObservation> caches
	)
		: Grants(grants), Directories(directories), ImageCaches(caches), Policy(policy) {}
	bool GraphFileHost::Capture(
		const HostNodeInvocation &invocation, HostNodeCapture &output, std::string &failure
	) {
		if (invocation.Authored.Type == "pc.directory_search") {
			GraphDirectoryHost reader(Directories, Grants, Policy);
			return reader.Capture(invocation, output, failure);
		}
		if (invocation.Authored.Type == "pc.image" || invocation.Authored.Type == "pc.image_sequence" ||
			invocation.Authored.Type == "pc.image_animated")
			return CaptureGraphRaster(invocation, Grants, Policy, output, failure, nullptr, ImageCaches);
		if (invocation.Authored.Type == "pc.3_d_mesh_obj" || invocation.Authored.Type == "pc.3_d_mesh_json" ||
			invocation.Authored.Type == "pc.3_d_mesh_export")
			return CaptureGraphMeshFile(Grants, Policy, invocation, output, failure);
		if (invocation.Authored.Type == "pc.tile_tilemap_export")
			return CaptureGraphTileFile(invocation, Grants, Policy, output, failure);
		if (IsGraphTextFileHost(invocation.Authored.Type))
			return CaptureGraphTextFile(Grants, Policy, invocation, output, failure);
		if (IsGraphSpriteHost(invocation.Authored.Type))
			return CaptureGraphSprite(invocation, Grants, Policy, output, failure);
		const GraphFileGrant *grant = nullptr;
		for (const auto &candidate : Grants)
			if (candidate.NodeId == invocation.Authored.Id && candidate.Resource.empty()) {
				if (grant) {
					failure = "file host grant is duplicated";
					return false;
				}
				grant = &candidate;
			}
		if (!grant) {
			failure = "file host capability was not granted for node " + invocation.Authored.Id;
			return false;
		}
		if (invocation.Authored.Type == "pc.wav_file_write") {
			const auto authoredBytes = NodeClonePayloadBytes(invocation.Authored);
			if (!authoredBytes || *authoredBytes > invocation.MaximumOperationBytes) {
				failure = "WAV capture authored payload exceeds byte budget";
				return false;
			}
			uint64_t captureBytes = *authoredBytes;
			for (const auto &input : invocation.Inputs) {
				const auto valueBytes = ValueClonePayloadBytes(input.Data);
				const uint64_t overhead = sizeof(AuthoredValue) + input.Port.size() + 1;
				if (!valueBytes || overhead > invocation.MaximumOperationBytes - captureBytes ||
					*valueBytes > invocation.MaximumOperationBytes - captureBytes - overhead) {
					failure = "WAV capture resolved controls exceed byte budget";
					return false;
				}
				captureBytes += overhead + *valueBytes;
			}
			WavExport wav;
			Diagnostic diagnostic;
			if (PrepareResolvedWavExport(
					invocation.Inputs, invocation.MaximumOperationBytes - captureBytes, wav, diagnostic
				) != Status::Ok) {
				failure = diagnostic.Message;
				return false;
			}
			if (wav.Path != grant->File.string()) {
				failure = "resolved WAV destination differs from its exact write grant";
				return false;
			}
			HostNodeCapture capture;
			capture.Authored = invocation.Authored;
			capture.Tick = invocation.Request.Tick;
			capture.Subframe = invocation.Request.Subframe;
			capture.NegativeFrame = invocation.Request.NegativeFrame;
			capture.Inputs.assign(invocation.Inputs.begin(), invocation.Inputs.end());
			if (!PublishGraphHostFile(*grant, Policy, wav.Bytes, invocation.MaximumOperationBytes, failure))
				return false;
			output = std::move(capture);
			return true;
		}
		const Value *pathValue = Input(invocation, "path");
		const auto *path = pathValue ? std::get_if<std::string>(pathValue) : nullptr;
		if (!path || *path != grant->File.string() || !Policy.AllowsName(*path)) {
			failure = "resolved graph path differs from its exact host file grant or violates content policy";
			return false;
		}
		const auto &kind = invocation.Authored.Type;
		const bool write = kind == "pc.text_file_write" || kind == "pc.byte_file_write";
		const bool read = kind == "pc.text_file_read" || kind == "pc.byte_file_read" ||
						  kind == "pc.ora_file_read" || kind == "pc.krita_file_read";
		if ((!read && !write) || write != grant->Write) {
			failure = "file grant operation or node type is unsupported";
			return false;
		}
		HostNodeCapture capture;
		capture.Authored = invocation.Authored;
		capture.Tick = invocation.Request.Tick;
		capture.Subframe = invocation.Request.Subframe;
		capture.NegativeFrame = invocation.Request.NegativeFrame;
		capture.Inputs.assign(invocation.Inputs.begin(), invocation.Inputs.end());
		for (const auto &image : invocation.Images) {
			if (!image.Data) {
				failure = "host input image is absent";
				return false;
			}
			capture.InputImages.push_back({std::string(image.Port), SurfaceHash(*image.Data)});
		}
		if (write) {
			const auto *input = Input(invocation, kind == "pc.byte_file_write" ? "input_1" : "content");
			std::span<const uint8_t> bytes;
			if (const auto *buffer = input ? std::get_if<BufferValue>(input) : nullptr)
				bytes = buffer->Bytes;
			else if (const auto *text = input ? std::get_if<std::string>(input) : nullptr)
				bytes = {reinterpret_cast<const uint8_t *>(text->data()), text->size()};
			else {
				failure = "host file writer input has wrong type";
				return false;
			}
			if (bytes.size() > Limits::MaximumTextBytes || bytes.size() > invocation.MaximumOperationBytes) {
				failure = "host file write exceeds its byte budget";
				return false;
			}
			if (!WriteFile(grant->File, bytes, failure)) return false;
		} else {
			std::error_code error;
			const auto size = std::filesystem::file_size(grant->File, error);
			const bool layered = kind == "pc.ora_file_read" || kind == "pc.krita_file_read";
			const uint64_t limit = layered ? 64ull * 1024 * 1024 : Limits::MaximumTextBytes;
			if (error || size > limit || size > invocation.MaximumOperationBytes) {
				failure = "host file read exceeds its byte budget or cannot be inspected";
				return false;
			}
			std::vector<std::byte> bytes(static_cast<size_t>(size));
			std::ifstream stream(grant->File, std::ios::binary);
			stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			if (!stream || static_cast<size_t>(stream.gcount()) != bytes.size()) {
				failure = "cannot read complete host file";
				return false;
			}
			if (layered) {
				engine::bake::LayeredImage content;
				if (!engine::bake::ReadLayeredImage(
						bytes,
						kind == "pc.ora_file_read" ? engine::bake::LayeredImageFormat::OpenRaster
												   : engine::bake::LayeredImageFormat::Krita,
						content,
						failure,
						(invocation.MaximumOperationBytes - size) / 3
					))
					return false;
				uint64_t retained = content.Merged.Pixels.size();
				for (const auto &layer : content.Layers)
					retained += layer.Pixels.Pixels.size();
				if (retained > invocation.MaximumOperationBytes - size) {
					failure = "host layered decode exceeds its operation budget";
					return false;
				}
				capture.Images.push_back({"merged_image", Surface(content.Merged)});
				capture.Outputs.push_back({"content", LayerContent(content)});
			} else if (kind == "pc.byte_file_read") {
				BufferValue buffer;
				buffer.Bytes.reserve(bytes.size());
				for (auto byte : bytes)
					buffer.Bytes.push_back(std::to_integer<uint8_t>(byte));
				capture.Outputs.push_back({"content", std::move(buffer)});
			} else
				capture.Outputs.push_back(
					{"content", std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size())}
				);
			capture.Outputs.push_back({"path", *path});
		}
		output = std::move(capture);
		return true;
	}
}
