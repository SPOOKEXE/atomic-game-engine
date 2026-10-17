#include "Decoders.hpp"

#include <engine/assets/ContentHash.hpp>
#include <engine/assets/TexturePixel.hpp>
#include <engine/bake/Image.hpp>
#include <engine/bake/ImageGraph.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/imagegraph/Reference.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <new>
#include <optional>
#include <utility>

namespace engine::bake {
	namespace {
		uint32_t Unsigned(std::span<const std::byte> bytes, size_t at, bool bigEndian, size_t count = 4) {
			uint32_t value = 0;
			for (size_t index = 0; index < count; ++index) {
				const size_t offset = bigEndian ? index : count - 1 - index;
				value = (value << 8) | std::to_integer<uint8_t>(bytes[at + offset]);
			}
			return value;
		}
		bool Dimensions(uint32_t width, uint32_t height, std::string &failure) {
			if (width == 0 || height == 0 || width > imagegraph::Limits::MaximumDimension ||
				height > imagegraph::Limits::MaximumDimension ||
				uint64_t(width) * height * 4 > imagegraph::Limits::MaximumImageBytes) {
				failure = "image graph source dimensions exceed the image budget";
				return false;
			}
			return true;
		}
		// Inspect the decoder's dimension carriers before it allocates decoded pixels.
		// Full format validation remains with the existing decoder, including CRCs.
		bool RasterDimensions(std::span<const std::byte> bytes, ImageFormat format, std::string &failure) {
			switch (format) {
			case ImageFormat::Png: {
				size_t offset = 8;
				bool found = false;
				while (bytes.size() - std::min(offset, bytes.size()) >= 12) {
					const uint32_t length = Unsigned(bytes, offset, true);
					if (length > bytes.size() - offset - 12) break;
					const bool header =
						bytes[offset + 4] == std::byte{'I'} && bytes[offset + 5] == std::byte{'H'} &&
						bytes[offset + 6] == std::byte{'D'} && bytes[offset + 7] == std::byte{'R'};
					if (header) {
						if (length != 13) break;
						if (!Dimensions(
								Unsigned(bytes, offset + 8, true), Unsigned(bytes, offset + 12, true), failure
							))
							return false;
						found = true;
					}
					offset += size_t(length) + 12;
					if (offset == bytes.size() && found) return true;
				}
				break;
			}
			case ImageFormat::Bmp:
				if (bytes.size() >= 26) {
					const uint32_t storedHeight = Unsigned(bytes, 22, false);
					const uint32_t height = (storedHeight & 0x80000000u) ? 0u - storedHeight : storedHeight;
					return Dimensions(Unsigned(bytes, 18, false), height, failure);
				}
				break;
			case ImageFormat::Gif:
				if (bytes.size() >= 10)
					return Dimensions(Unsigned(bytes, 6, false, 2), Unsigned(bytes, 8, false, 2), failure);
				break;
			case ImageFormat::Jpeg: {
				size_t offset = 2;
				bool found = false;
				while (offset < bytes.size()) {
					if (bytes[offset++] != std::byte{0xff}) break;
					while (offset < bytes.size() && bytes[offset] == std::byte{0xff})
						++offset;
					if (offset == bytes.size()) break;
					const uint8_t marker = std::to_integer<uint8_t>(bytes[offset++]);
					if (marker == 0xda || marker == 0xd9) {
						if (found) return true;
						break;
					}
					if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
					if (bytes.size() - offset < 2) break;
					const uint32_t length = Unsigned(bytes, offset, true, 2);
					if (length < 2 || length > bytes.size() - offset) break;
					if (marker == 0xc0) {
						if (length < 8) break;
						if (!Dimensions(
								Unsigned(bytes, offset + 5, true, 2),
								Unsigned(bytes, offset + 3, true, 2),
								failure
							))
							return false;
						found = true;
					}
					offset += length;
				}
				break;
			}
			case ImageFormat::Unknown:
			case ImageFormat::Svg:
				failure = "image graph source is not a supported static raster";
				return false;
			}
			failure = "image graph source has no valid bounded raster dimensions";
			return false;
		}
	}

	bool DecodeImageGraphSource(
		std::string_view name,
		std::span<const std::byte> encoded,
		imagegraph::Image &out,
		std::string &failure
	) {
		return DecodeImageGraphSourceTyped(imagegraph::Source{std::string(name)}, encoded, out, failure);
	}

	bool DecodeImageGraphSourceTyped(
		const imagegraph::Source &source,
		std::span<const std::byte> encoded,
		imagegraph::Image &out,
		std::string &failure
	) {
		if (source.Interpretation != imagegraph::SourceInterpretation::Colour &&
			source.Interpretation != imagegraph::SourceInterpretation::Data) {
			failure = "image graph source interpretation is invalid";
			return false;
		}
		if (encoded.empty() || encoded.size() > imagegraph::Limits::MaximumImageBytes) {
			failure = "image graph encoded source exceeds the image budget or is empty";
			return false;
		}
		try {
			assets::TextureData decoded;
			if (encoded.size() >= 4 && Unsigned(encoded, 0, false) == assets::Texture::MAGIC) {
				if (encoded.size() < 15) {
					failure = "image graph source is a truncated texture asset";
					return false;
				}
				if (!Dimensions(Unsigned(encoded, 7, false), Unsigned(encoded, 11, false), failure))
					return false;
				core::ByteReader reader(encoded);
				if (!assets::Texture::Read(reader, decoded) || !reader.AtEnd()) {
					failure = "image graph source is a malformed texture asset";
					return false;
				}
			} else {
				const auto format = ImageFormatOfBytes(encoded);
				if (!RasterDimensions(encoded, format, failure)) return false;
				const bool imported = format == ImageFormat::Gif
										  ? ReadGifBounded(
												encoded,
												imagegraph::Limits::MaximumDimension,
												imagegraph::Limits::MaximumImageBytes / 4,
												decoded,
												failure
											)
										  : ReadImage(encoded, decoded, failure);
				if (!imported) return false;
			}
			if (!decoded.IsValid() || !Dimensions(decoded.Width, decoded.Height, failure)) return false;
			imagegraph::Image converted;
			converted.Width = decoded.Width;
			converted.Height = decoded.Height;
			const bool numeric = source.Interpretation == imagegraph::SourceInterpretation::Data;
			converted.Space = numeric ? imagegraph::OutputSpace::Linear : imagegraph::OutputSpace::SRGB;
			if (decoded.Format == assets::TextureFormat::RGBA8 ||
				(numeric && decoded.Format == assets::TextureFormat::RGBA8_LINEAR)) {
				converted.Pixels = std::move(decoded.Pixels);
			} else {
				converted.Pixels.resize(uint64_t(decoded.Width) * decoded.Height * 4);
				const size_t stride = assets::BytesPerPixel(decoded.Format);
				for (size_t index = 0; index < converted.Pixels.size() / 4; ++index) {
					assets::TexturePixel pixel{};
					const auto stored = std::span(decoded.Pixels).subspan(index * stride, stride);
					if (numeric) {
						if (!assets::LoadTexturePixel(decoded.Format, stored, pixel)) {
							failure = "image graph source contains an invalid data pixel";
							return false;
						}
					} else {
						std::array<float, 4> display{};
						if (!assets::LoadTexturePixelForDisplay(decoded.Format, stored, display)) {
							failure = "image graph source contains an invalid display pixel";
							return false;
						}
						pixel = {display[0], display[1], display[2], display[3]};
						// grug stores encoded colour. numeric formats arrive as linear samples.
						if (!assets::IsSRGB(decoded.Format)) {
							for (size_t channel = 0; channel < 3; ++channel) {
								const double linear = pixel[channel];
								pixel[channel] = linear <= 0.0031308
													 ? 12.92 * linear
													 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
							}
						}
					}
					if (!assets::StoreTexturePixel(
							assets::TextureFormat::RGBA8,
							pixel,
							std::span(converted.Pixels).subspan(index * 4, 4)
						)) {
						failure = "image graph source pixel conversion failed";
						return false;
					}
				}
			}
			out = std::move(converted);
			return true;
		} catch (const std::bad_alloc &) {
			failure = "image graph source allocation failed";
			return false;
		}
	}

	bool ImageGraphTexture(const imagegraph::Image &image, assets::TextureData &out, std::string &failure) {
		if (!image.IsValid()) {
			failure = "image graph result is not a valid bounded image";
			return false;
		}
		try {
			assets::TextureData converted;
			converted.Width = image.Width;
			converted.Height = image.Height;
			converted.Format = image.Space == imagegraph::OutputSpace::Linear
								   ? assets::TextureFormat::RGBA8_LINEAR
								   : assets::TextureFormat::RGBA8;
			converted.Pixels = image.Pixels;
			out = std::move(converted);
			return true;
		} catch (const std::bad_alloc &) {
			failure = "image graph texture allocation failed";
			return false;
		}
	}

	bool BakeImageGraphTyped(
		const imagegraph::Document &document,
		std::string_view output,
		const imagegraph::TypedSourceResolver &sources,
		assets::TextureData &out,
		imagegraph::Diagnostic &diagnostic
	) {
		imagegraph::Plan plan;
		imagegraph::Image image;
		if (!imagegraph::Compile(document, plan, diagnostic) ||
			!imagegraph::EvaluateTyped(document, plan, output, sources, image, diagnostic))
			return false;
		std::string failure;
		if (!ImageGraphTexture(image, out, failure)) {
			diagnostic = {{}, std::move(failure)};
			return false;
		}
		return true;
	}

	bool CookImageGraph(
		const imagegraph::Document &document,
		std::string_view graphAssetName,
		const imagegraph::TypedSourceResolver &sources,
		CookedImageGraph &out,
		imagegraph::Diagnostic &diagnostic
	) {
		imagegraph::Plan plan;
		if (!imagegraph::Compile(document, plan, diagnostic)) return false;
		if (!imagegraph::IsRuntimeAsset(graphAssetName) ||
			graphAssetName.starts_with("__imagegraph_sources/")) {
			diagnostic = {{}, "live image graph needs a portable .aimagegraph asset name"};
			return false;
		}

		for (const auto &output : document.Outputs) {
			if (!imagegraph::IsReferenceToken(output.Name)) {
				diagnostic = {output.Node, "live graph output needs a portable reference token"};
				return false;
			}
		}
		try {
			CookedImageGraph cooked;
			cooked.Name = graphAssetName;
			cooked.Graph = document;
			using SourceKey = std::pair<std::string, imagegraph::SourceInterpretation>;
			std::map<SourceKey, std::string> names;
			std::map<std::string, assets::TextureData> textures;
			size_t retainedBytes = 0;
			const auto normalize = [&](const imagegraph::Source &source,
									   std::string_view node,
									   std::string &name) {
				if (source.Path.starts_with("editable-image://")) {
					diagnostic = {
						std::string(node),
						"local editable image cannot be cooked; bind its ContentId at runtime"
					};
					return false;
				}
				const SourceKey key{source.Path, source.Interpretation};
				if (const auto found = names.find(key); found != names.end()) {
					name = found->second;
					return true;
				}
				imagegraph::Image image;
				std::string failure;
				if (!sources || !sources(source, image, failure) || !image.IsValid()) {
					diagnostic = {
						std::string(node),
						failure.empty() ? "live graph source unavailable or invalid" : failure
					};
					return false;
				}
				image.Space = source.Interpretation == imagegraph::SourceInterpretation::Data
								  ? imagegraph::OutputSpace::Linear
								  : imagegraph::OutputSpace::SRGB;
				assets::TextureData texture;
				if (!ImageGraphTexture(image, texture, failure)) {
					diagnostic = {std::string(node), std::move(failure)};
					return false;
				}
				core::ByteWriter bytes;
				if (!assets::Texture::Write(bytes, texture)) {
					diagnostic = {std::string(node), "live graph source cannot be normalized"};
					return false;
				}
				name = "__imagegraph_sources/" + assets::Hasher::Of(bytes.Bytes()).ToHex() + ".atex";
				if (!textures.contains(name)) {
					if (texture.Pixels.size() > imagegraph::Limits::MaximumRetainedBytes - retainedBytes) {
						diagnostic = {
							std::string(node), "live graph source closure exceeds retained byte budget"
						};
						return false;
					}
					retainedBytes += texture.Pixels.size();
					textures.emplace(name, std::move(texture));
				}
				names.emplace(key, name);
				return true;
			};
			for (auto &node : cooked.Graph.Nodes) {
				auto *source = std::get_if<imagegraph::Source>(&node.Value);
				if (!source) continue;
				std::string name;
				if (!normalize(*source, node.Id, name)) return false;
				source->Path = std::move(name);
			}
			for (auto &parameter : cooked.Graph.Parameters) {
				auto *path = std::get_if<std::string>(&parameter.Default);
				if (!path) continue;
				const bool sourcePath =
					std::any_of(document.Bindings.begin(), document.Bindings.end(), [&](const auto &binding) {
						if (binding.Input != parameter.Name || binding.Property != "path") return false;
						return std::any_of(
							document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
								return node.Id == binding.Node &&
									   std::holds_alternative<imagegraph::Source>(node.Value);
							}
						);
					});
				if (!sourcePath) continue;
				std::optional<imagegraph::SourceInterpretation> interpretation;
				std::string nodeName;
				for (const auto &binding : document.Bindings) {
					if (binding.Input != parameter.Name) continue;
					const auto node = std::find_if(
						document.Nodes.begin(), document.Nodes.end(), [&](const auto &candidate) {
							return candidate.Id == binding.Node;
						}
					);
					const auto *source = node == document.Nodes.end()
											 ? nullptr
											 : std::get_if<imagegraph::Source>(&node->Value);
					if (!source || binding.Property != "path" ||
						(interpretation && *interpretation != source->Interpretation)) {
						diagnostic = {
							binding.Node, "live graph path input must bind sources with one interpretation"
						};
						return false;
					}
					interpretation = source->Interpretation;
					nodeName = binding.Node;
				}
				if (!interpretation) continue;
				std::string name;
				if (!normalize(imagegraph::Source{*path, *interpretation}, nodeName, name)) return false;
				*path = std::move(name);
			}
			imagegraph::Document resolved;
			imagegraph::Plan resolvedPlan;
			if (!imagegraph::ResolveInputs(cooked.Graph, {}, resolved, diagnostic) ||
				!imagegraph::Compile(resolved, resolvedPlan, diagnostic))
				return false;
			std::vector<imagegraph::SourceExtent> extents;
			for (const auto &node : resolved.Nodes) {
				const auto *source = std::get_if<imagegraph::Source>(&node.Value);
				if (!source) continue;
				const auto texture = textures.find(source->Path);
				if (texture == textures.end()) {
					diagnostic = {node.Id, "live graph source default has no cooked texture"};
					return false;
				}
				extents.push_back({node.Id, texture->second.Width, texture->second.Height});
			}
			for (const auto &output : resolved.Outputs) {
				imagegraph::ExecutionPlan execution;
				if (!imagegraph::Prepare(resolved, resolvedPlan, output.Name, extents, execution, diagnostic))
					return false;
			}
			if (!imagegraph::Write(cooked.Graph, cooked.Text, diagnostic)) return false;
			for (auto &[name, texture] : textures)
				cooked.Sources.push_back({name, std::move(texture)});
			out = std::move(cooked);
			return true;
		} catch (const std::bad_alloc &) {
			diagnostic = {{}, "live image graph cook allocation failed"};
			return false;
		}
	}

	bool BakeImageGraph(
		const imagegraph::Document &document,
		std::string_view output,
		const imagegraph::SourceResolver &sources,
		assets::TextureData &out,
		imagegraph::Diagnostic &diagnostic
	) {
		imagegraph::Plan plan;
		imagegraph::Image image;
		if (!imagegraph::Compile(document, plan, diagnostic) ||
			!imagegraph::Evaluate(document, plan, output, sources, image, diagnostic))
			return false;
		assets::TextureData converted;
		converted.Width = image.Width;
		converted.Height = image.Height;
		converted.Format = image.Space == imagegraph::OutputSpace::Linear
							   ? assets::TextureFormat::RGBA8_LINEAR
							   : assets::TextureFormat::RGBA8;
		converted.Pixels = std::move(image.Pixels);
		out = std::move(converted);
		return true;
	}
}
