#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ImageGraphBinding.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

TEST_SUITE_ID("engine.scene.imagegraphbinding")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::Name;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::scene::IMAGE_GRAPH_BINDING_MAXIMUM_NAME_BYTES;
using engine::scene::IMAGE_GRAPH_BINDING_MAXIMUM_SERIALISED_BYTES;
using engine::scene::ImageGraphBinding;
using engine::scene::ImageGraphTickPolicy;
using engine::scene::IsValidImageGraphBinding;
using engine::scene::RegisterSceneComponents;
using engine::scene::SetImageGraphBinding;

namespace {
	ImageGraphBinding Binding() {
		ImageGraphBinding binding;
		binding.Graph = Name("images.clouds");
		binding.Output = Name("output.main");
		binding.Texture = Name("texture.clouds");
		binding.Seed = 0xFEDCBA9876543210ull;
		binding.TickPolicy = ImageGraphTickPolicy::Fixed;
		binding.FixedTick = 47;
		binding.ColorSpace = engine::scene::ImageGraphColorSpace::Linear;
		return binding;
	}

	bool ContainsText(std::span<const std::byte> bytes, std::string_view text) {
		const auto needle = std::as_bytes(std::span(text.data(), text.size()));
		return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) != bytes.end();
	}
}

TEST_CASE(
	"ImageGraphBinding stores authored selectors and tick controls in ECS", "[scene][imagegraphbinding]"
) {
	RegisterSceneComponents();
	Store store("imagegraph-binding-authoring");
	const Entity sink = store.Create();
	ImageGraphBinding binding = Binding();

	REQUIRE(SetImageGraphBinding(store, sink, binding));
	const ImageGraphBinding *authored = store.Get<ImageGraphBinding>(sink);
	REQUIRE(authored != nullptr);
	CHECK(authored->Graph.Text() == "images.clouds");
	CHECK(authored->Output.Text() == "output.main");
	CHECK(authored->Texture.Text() == "texture.clouds");
	CHECK(authored->Seed == binding.Seed);
	CHECK(authored->TickPolicy == ImageGraphTickPolicy::Fixed);
	CHECK(authored->FixedTick == 47);
	CHECK(authored->ColorSpace == engine::scene::ImageGraphColorSpace::Linear);

	binding.Output = Name("output.glow");
	binding.Seed = 19;
	binding.TickPolicy = ImageGraphTickPolicy::World;
	REQUIRE(SetImageGraphBinding(store, sink, binding));
	authored = store.Get<ImageGraphBinding>(sink);
	REQUIRE(authored != nullptr);
	CHECK(authored->Output.Text() == "output.glow");
	CHECK(authored->Seed == 19);
	CHECK(authored->TickPolicy == ImageGraphTickPolicy::World);
	CHECK(authored->FixedTick == 47);
}

TEST_CASE(
	"ImageGraphBinding snapshots carry durable text and restore all authored values",
	"[scene][imagegraphbinding]"
) {
	RegisterSceneComponents();
	Store source("imagegraph-binding-source");
	const Entity sink = source.Create();
	const ImageGraphBinding binding = Binding();
	REQUIRE(SetImageGraphBinding(source, sink, binding));
	const Entity worldTickSink = source.Create();
	ImageGraphBinding worldTickBinding = Binding();
	worldTickBinding.Output = Name("output.world");
	worldTickBinding.TickPolicy = ImageGraphTickPolicy::World;
	worldTickBinding.FixedTick = 103;
	REQUIRE(SetImageGraphBinding(source, worldTickSink, worldTickBinding));

	ByteWriter writer;
	REQUIRE(source.Save(writer));
	CHECK(ContainsText(writer.Bytes(), binding.Graph.Text()));
	CHECK(ContainsText(writer.Bytes(), binding.Output.Text()));
	CHECK(ContainsText(writer.Bytes(), binding.Texture.Text()));

	Store restored("imagegraph-binding-restored");
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.LoadContents(reader));
	CHECK(reader.AtEnd());
	const ImageGraphBinding *roundTrip = restored.Get<ImageGraphBinding>(sink);
	REQUIRE(roundTrip != nullptr);
	CHECK(roundTrip->Graph.Text() == binding.Graph.Text());
	CHECK(roundTrip->Output.Text() == binding.Output.Text());
	CHECK(roundTrip->Texture.Text() == binding.Texture.Text());
	CHECK(roundTrip->Seed == binding.Seed);
	CHECK(roundTrip->TickPolicy == binding.TickPolicy);
	CHECK(roundTrip->FixedTick == binding.FixedTick);
	CHECK(roundTrip->ColorSpace == engine::scene::ImageGraphColorSpace::Linear);
	const ImageGraphBinding *roundTripWorldTick = restored.Get<ImageGraphBinding>(worldTickSink);
	REQUIRE(roundTripWorldTick != nullptr);
	CHECK(roundTripWorldTick->Output.Text() == worldTickBinding.Output.Text());
	CHECK(roundTripWorldTick->TickPolicy == ImageGraphTickPolicy::World);
	CHECK(roundTripWorldTick->FixedTick == worldTickBinding.FixedTick);

	const auto id = Components::Find(Name("scene.ImageGraphBinding"));
	REQUIRE(id.IsValid());
	const auto &descriptor = Components::Describe(id);
	CHECK(descriptor.Serialisable);
	CHECK_FALSE(descriptor.RawSerialisation);
	CHECK(descriptor.MaximumSerialisedBytes == IMAGE_GRAPH_BINDING_MAXIMUM_SERIALISED_BYTES);
}

TEST_CASE(
	"ImageGraphBinding default rows survive component audit round trips", "[scene][imagegraphbinding]"
) {
	RegisterSceneComponents();
	const auto id = Components::Find(Name("scene.ImageGraphBinding"));
	REQUIRE(id.IsValid());
	const auto &descriptor = Components::Describe(id);
	ImageGraphBinding empty[2];
	ByteWriter writer;
	descriptor.Write(writer, empty, 2);
	REQUIRE(writer.Bytes().size() == 2);
	ImageGraphBinding restored[2];
	ByteReader reader(writer.Bytes());
	descriptor.Read(reader, restored, 2);
	CHECK(reader.AtEnd());
	CHECK_FALSE(restored[0].Graph.IsValid());
	CHECK_FALSE(restored[1].Texture.IsValid());
}

TEST_CASE("version one image graph bindings load as display colour", "[scene][imagegraphbinding]") {
	RegisterSceneComponents();
	const auto id = Components::Find(Name("scene.ImageGraphBinding"));
	REQUIRE(id.IsValid());
	const auto &descriptor = Components::Describe(id);
	ByteWriter writer;
	writer.WriteUInt8(1);
	writer.WriteString("legacy-graph");
	writer.WriteString("final");
	writer.WriteString("legacy-texture");
	writer.WriteUInt64(17);
	writer.WriteString("fixed");
	writer.WriteUInt64(3);
	ImageGraphBinding restored;
	ByteReader reader(writer.Bytes());
	descriptor.Read(reader, &restored, 1);
	CHECK(reader.AtEnd());
	CHECK(restored.ColorSpace == engine::scene::ImageGraphColorSpace::Display);
	CHECK(restored.Seed == 17);
}

TEST_CASE(
	"ImageGraphBinding rejects invalid selectors, policy and reserved storage", "[scene][imagegraphbinding]"
) {
	ImageGraphBinding binding = Binding();
	CHECK(IsValidImageGraphBinding(binding));
	CHECK(std::is_trivially_copyable_v<ImageGraphBinding>);
	CHECK(
		sizeof(ImageGraphBinding) == 2 * sizeof(uint64_t) + 3 * sizeof(Name) + sizeof(ImageGraphTickPolicy) +
										 sizeof(binding.ColorSpace) + sizeof(binding.Reserved)
	);

	binding.Graph = Name();
	CHECK_FALSE(IsValidImageGraphBinding(binding));
	binding = Binding();
	binding.Output = Name(std::string(IMAGE_GRAPH_BINDING_MAXIMUM_NAME_BYTES, 'o'));
	CHECK(IsValidImageGraphBinding(binding));
	binding = Binding();
	binding.Output = Name(std::string(IMAGE_GRAPH_BINDING_MAXIMUM_NAME_BYTES + 1, 'o'));
	CHECK_FALSE(IsValidImageGraphBinding(binding));
	binding = Binding();
	binding.Texture = Name(std::string("texture\0invalid", 15));
	CHECK_FALSE(IsValidImageGraphBinding(binding));
	binding = Binding();
	binding.TickPolicy = static_cast<ImageGraphTickPolicy>(0xFF);
	CHECK_FALSE(IsValidImageGraphBinding(binding));
	binding = Binding();
	binding.ColorSpace = static_cast<engine::scene::ImageGraphColorSpace>(0xFF);
	CHECK_FALSE(IsValidImageGraphBinding(binding));
	binding = Binding();
	binding.Reserved[1] = 1;
	CHECK_FALSE(IsValidImageGraphBinding(binding));

	RegisterSceneComponents();
	Store store("imagegraph-binding-invalid-update");
	const Entity sink = store.Create();
	binding = Binding();
	REQUIRE(SetImageGraphBinding(store, sink, binding));
	ImageGraphBinding invalid = binding;
	invalid.Seed = 99;
	invalid.Texture = Name(std::string(IMAGE_GRAPH_BINDING_MAXIMUM_NAME_BYTES + 1, 't'));
	CHECK_FALSE(SetImageGraphBinding(store, sink, invalid));
	const ImageGraphBinding *unchanged = store.Get<ImageGraphBinding>(sink);
	REQUIRE(unchanged != nullptr);
	CHECK(unchanged->Seed == binding.Seed);

	Store malformedSource("imagegraph-binding-malformed");
	const Entity malformedSink = malformedSource.Create();
	malformedSource.Set<ImageGraphBinding>(malformedSink, invalid);
	ByteWriter malformedSnapshot;
	REQUIRE(malformedSource.Save(malformedSnapshot));
	Store rejected("imagegraph-binding-rejected");
	ByteReader malformedReader(malformedSnapshot.Bytes());
	CHECK_FALSE(rejected.LoadContents(malformedReader));

	const Entity retired = store.Create();
	store.Destroy(retired);
	CHECK_FALSE(SetImageGraphBinding(store, retired, binding));
}
