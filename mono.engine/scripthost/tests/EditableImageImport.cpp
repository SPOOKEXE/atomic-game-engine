#include <engine/ecs/Store.hpp>
#include <engine/imagecodec/Image.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.scripthost.editableimage.import")

namespace {
	constexpr std::string_view PNG = "iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAYAAAD0In+KAAAAEUlEQVR4nGNocFCo/8/"
									 "A8B8ADf8DXk5zibEAAAAASUVORK5CYII=";
	// Same grayscale quadrant fixture as bake/tests/Image.cpp, with known BT.601 luma.
	constexpr std::string_view JPEG =
		"/9j/4AAQSkZJRgABAQAAAQABAAD/"
		"2wBDAAIBAQEBAQIBAQECAgICAgQDAgICAgUEBAMEBgUGBgYFBgYGBwkIBgcJBwYGCAsICQoKCgoKBggLDAsKDAkKCgr/"
		"wAALCAAQABABAREA/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/"
		"8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3O"
		"Dk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx"
		"8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/9oACAEBAAA/APi+v0or8G6/sUr/2Q==";
	std::string BufferLiteral(engine::script::Language language, std::string_view base64) {
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::imagecodec::DecodeBase64(base64, bytes, failure));
		if (language == engine::script::Language::Luau) {
			std::string literal = "buffer.fromstring('";
			for (const auto byte : bytes) {
				const unsigned value = std::to_integer<unsigned>(byte);
				literal += '\\';
				literal += static_cast<char>('0' + value / 100);
				literal += static_cast<char>('0' + value / 10 % 10);
				literal += static_cast<char>('0' + value % 10);
			}
			return literal + "')";
		}
		std::string literal = "new Uint8Array([";
		for (const auto byte : bytes)
			literal += std::to_string(std::to_integer<unsigned>(byte)) + ',';
		return literal + "]).buffer";
	}
}

TEST_CASE(
	"both VMs import bounded raw PNG JPEG images without changing stable content names",
	"[scripting][editableimage][import]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	for (const auto language : {script::Language::Luau, script::Language::JavaScript}) {
		ecs::Store store("editable-image.import.vm");
		const auto runtime = script::MakeRuntime(store, language);
		std::string source = language == script::Language::Luau ? "local " : "const ";
		source += "png = '" + std::string(PNG) + "';\n";
		source += language == script::Language::Luau ? "local " : "const ";
		source += "jpeg = '" + std::string(JPEG) + "';\n";
		source += language == script::Language::Luau ? "local " : "const ";
		source += "pngBuffer = " + BufferLiteral(language, PNG) + ";\n";
		source += language == script::Language::Luau ? "local " : "const ";
		source += "jpegBuffer = " + BufferLiteral(language, JPEG) + ";\n";
		source += language == script::Language::Luau ? R"(
local image = Instance.new('EditableImage')
image.Name = 'Imported'
image.Parent = workspace
local content = image.ContentId
assert(image:Resize(2,2))
assert(image.ColorSpace == 'linear')
assert(image:FromBase64('AAECAwQFBgcICQoLDA0ODw==', 'rgba8'))
assert(image:ToBase64() == 'AAECAwQFBgcICQoLDA0ODw==')
assert(image:FromBase64(png, 'png'))
assert(image.Size == Vector2.new(2,1) and image.ColorSpace == 'srgb')
assert(image:ToBase64() == 'gEAgf/8AAP8=')
assert(image:FromBase64(jpeg, 'jpeg'))
assert(image.Size == Vector2.new(16,16) and image.ColorSpace == 'srgb')
local grey = image:ToBuffer()
assert(math.abs(buffer.readu8(grey, (4*16+4)*4)-76) <= 3)
assert(buffer.readu8(grey, (4*16+4)*4+3) == 255)
assert(image:FromEncodedBuffer(jpegBuffer, 'jpeg'))
assert(image:FromEncodedBuffer(pngBuffer, 'png'))
local saved = image:ToBase64()
assert(not image:FromBase64('AB==','rgba8'))
assert(not image:FromBase64('AA==junk','rgba8'))
assert(not image:FromBase64('AA==','rgba8'))
assert(not image:FromBase64(png,'jpeg'))
assert(not image:FromBase64(png,'unsupported'))
assert(not image:FromEncodedBuffer(pngBuffer,'rgba8'))
assert(not image:FromEncodedBuffer(buffer.create(8),'png'))
assert(not pcall(function() image:FromBase64(123,'rgba8') end))
assert(not pcall(function() image:FromEncodedBuffer('text','png') end))
assert(image:ToBase64() == saved and image.ContentId == content)
local copy = Instance.new('EditableImage')
assert(copy:Resize(2,1))
copy.ColorSpace = image.ColorSpace
assert(copy:FromBase64(saved,'rgba8'))
assert(copy.ColorSpace == 'srgb' and copy:ToBase64() == saved)
local wide = Instance.new('EditableImage')
assert(wide:Resize(1921,1))
assert(wide:ToBase64() == '' and not wide:FromBase64('AA==','rgba8'))
assert(wide.Size == Vector2.new(1921,1))
)"
													 : R"(
const image = Instance.new('EditableImage');
image.Name = 'Imported'; image.Parent = workspace;
const content = image.ContentId;
if (!image.Resize(2,2) || image.ColorSpace !== 'linear') throw new Error('default space');
if (!image.FromBase64('AAECAwQFBgcICQoLDA0ODw==','rgba8') || image.ToBase64() !== 'AAECAwQFBgcICQoLDA0ODw==') throw new Error('raw pixels');
if (!image.FromBase64(png,'png') || image.Size.X !== 2 || image.Size.Y !== 1 || image.ColorSpace !== 'srgb' || image.ToBase64() !== 'gEAgf/8AAP8=') throw new Error('PNG pixels');
if (!image.FromBase64(jpeg,'jpeg') || image.Size.X !== 16 || image.Size.Y !== 16 || image.ColorSpace !== 'srgb') throw new Error('JPEG dimensions');
const grey = new Uint8Array(image.ToBuffer());
if (Math.abs(grey[(4*16+4)*4]-76) > 3 || grey[(4*16+4)*4+3] !== 255) throw new Error('JPEG pixels');
if (!image.FromEncodedBuffer(jpegBuffer,'jpeg') || !image.FromEncodedBuffer(pngBuffer,'png')) throw new Error('encoded buffers');
const saved = image.ToBase64();
if (image.FromBase64('AB==','rgba8') || image.FromBase64('AA==junk','rgba8') || image.FromBase64('AA==','rgba8') || image.FromBase64(png,'jpeg') || image.FromBase64(png,'unsupported') || image.FromEncodedBuffer(pngBuffer,'rgba8') || image.FromEncodedBuffer(new ArrayBuffer(8),'png')) throw new Error('accepted malformed image');
let raised = false;
try { image.FromBase64(123,'rgba8'); } catch (_) { raised = true; }
if (!raised) throw new Error('accepted non-string');
raised = false;
try { image.FromEncodedBuffer('text','png'); } catch (_) { raised = true; }
if (!raised || image.ToBase64() !== saved || image.ContentId !== content) throw new Error('refusal changed image');
const copy = Instance.new('EditableImage');
if (!copy.Resize(2,1)) throw new Error('copy resize');
copy.ColorSpace = image.ColorSpace;
if (!copy.FromBase64(saved,'rgba8') || copy.ColorSpace !== 'srgb' || copy.ToBase64() !== saved) throw new Error('raw copy space');
const wide = Instance.new('EditableImage');
if (!wide.Resize(1921,1) || wide.ToBase64() !== '' || wide.FromBase64('AA==','rgba8') || wide.Size.X !== 1921) throw new Error('raw dimension refusal');
)";
		const bool ran = runtime->Run(source, "editable-image-import");
		INFO(runtime->LastError());
		REQUIRE(ran);
		const auto entity = store.FindFirstChild(scene::WorkspaceOf(store), "Imported");
		const auto *image = store.Get<scene::EditableImage>(entity);
		REQUIRE(image != nullptr);
		CHECK(image->Pixels == std::vector<uint8_t>{128, 64, 32, 127, 255, 0, 0, 255});
		CHECK(image->Space == scene::EditableImageSpace::SRGB);
		CHECK(image->Revision == 5);
		const auto before = *image;
		const auto receiver = language == script::Language::Luau
								  ? "local held = workspace:FindFirstChild('Imported')\n"
								  : "var held = workspace.FindFirstChild('Imported');\n";
		const auto method = language == script::Language::Luau ? "held:" : "held.";
		const auto hugeText =
			language == script::Language::Luau ? "string.rep('A',11059204)" : "'A'.repeat(11059204)";
		CHECK_FALSE(runtime->Run(
			std::string(receiver) + method + "FromBase64(" + hugeText + ",'rgba8')", "image-text-cap"
		));
		const auto hugeBuffer =
			language == script::Language::Luau ? "buffer.create(8294401)" : "new ArrayBuffer(8294401)";
		CHECK_FALSE(runtime->Run(
			std::string(receiver) + method + "FromEncodedBuffer(" + hugeBuffer + ",'png')", "image-buffer-cap"
		));
		store.SetAdoptOnly(true);
		const auto refusal = std::string(method) + "FromBase64('" + std::string(PNG) + "','png')";
		const auto authority = language == script::Language::Luau
								   ? "assert(not " + refusal + ")"
								   : "if (" + refusal + ") throw new Error('replica write');";
		REQUIRE(runtime->Run(std::string(receiver) + authority, "image-authority"));
		CHECK(image->Pixels == before.Pixels);
		CHECK(image->Revision == before.Revision);
		CHECK(image->Width == before.Width);
		CHECK(image->Height == before.Height);
		CHECK(image->Space == before.Space);
	}
}
