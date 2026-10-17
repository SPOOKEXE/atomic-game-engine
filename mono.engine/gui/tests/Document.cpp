#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Document.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Style.hpp>
#include <engine/gui/VirtualCollection.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.gui.document")

using namespace engine;
using namespace engine::gui;

namespace {
	DocumentNode Node(std::string id, std::string className, std::string name) {
		DocumentNode node;
		node.Id = std::move(id);
		node.Class = std::move(className);
		node.Name = std::move(name);
		return node;
	}

	void CollectClasses(const DocumentNode &node, std::vector<std::string> &classes) {
		classes.push_back(node.Class);
		for (const DocumentNode &child : node.Children) {
			CollectClasses(child, classes);
		}
	}
}

TEST_CASE("a GUI document exports stable classes properties and references", "[gui][document]") {
	ecs::Store source("gui_document.source");
	RegisterGuiClasses();
	const ecs::Entity frame = source.CreateInstance(GuiClass("Frame"), "Panel");
	const ecs::Entity label = source.CreateInstance(GuiClass("TextLabel"), "Title");
	source.SetParent(label, frame);
	Label text;
	text.Text = "Hello world";
	text.Size = 21;
	source.Set(label, text);
	Selection selection;
	selection.ImageObject = label;
	source.Set(frame, selection);

	UiDocument document;
	DocumentReport report;
	REQUIRE(ExportDocument(source, frame, document, report));
	REQUIRE(report.Ok());
	CHECK(document.Version == UI_DOCUMENT_VERSION);
	CHECK(document.Roots.front().Class == "Frame");

	const auto &properties = document.Roots.front().Properties;
	const auto selectionProperty =
		std::find_if(properties.begin(), properties.end(), [](const DocumentProperty &property) {
			return property.Name == "SelectionImageObject";
		});
	REQUIRE(selectionProperty != properties.end());
	CHECK(selectionProperty->Reference == document.Roots.front().Children.front().Id);
}

TEST_CASE("a GUI document imports a complete tree into an empty store", "[gui][document]") {
	ecs::Store source("gui_document.import_source");
	RegisterGuiClasses();
	const ecs::Entity frame = source.CreateInstance(GuiClass("Frame"), "Panel");
	const ecs::Entity label = source.CreateInstance(GuiClass("TextLabel"), "Title");
	source.SetParent(label, frame);
	Label text;
	text.Text = "Hello world";
	text.Size = 21;
	source.Set(label, text);
	Selection selection;
	selection.ImageObject = label;
	source.Set(frame, selection);

	UiDocument document;
	DocumentReport report;
	REQUIRE(ExportDocument(source, frame, document, report));

	ecs::Store destination("gui_document.import_destination");
	std::vector<ecs::Entity> roots;
	REQUIRE(ImportDocument(destination, document, roots, report));
	REQUIRE(report.Ok());
	REQUIRE(roots.size() == 1);
	CHECK(destination.ClassOf(roots.front()) == GuiClass("Frame"));
	CHECK(destination.InstanceNameOf(roots.front()).Text() == "Panel");

	ecs::Entity importedLabel = ecs::NULL_ENTITY;
	destination.EachChild(roots.front(), [&](ecs::Entity child) { importedLabel = child; });
	REQUIRE(importedLabel != ecs::NULL_ENTITY);
	CHECK(destination.ClassOf(importedLabel) == GuiClass("TextLabel"));
	CHECK(destination.InstanceNameOf(importedLabel).Text() == "Title");
	const Label *importedText = destination.Get<Label>(importedLabel);
	REQUIRE(importedText != nullptr);
	CHECK(importedText->Text == "Hello world");
	CHECK(importedText->Size == 21);
	const Selection *importedSelection = destination.Get<Selection>(roots.front());
	REQUIRE(importedSelection != nullptr);
	CHECK(importedSelection->ImageObject == importedLabel);
}

TEST_CASE("document import validates first and preserves a populated store", "[gui][document]") {
	UiDocument invalid;
	invalid.Roots.push_back(Node("bad", "NoSuchGui", "Bad"));
	ecs::Store empty("gui_document.import_invalid");
	std::vector<ecs::Entity> roots{ecs::Entity{17}};
	DocumentReport report;
	CHECK_FALSE(ImportDocument(empty, invalid, roots, report));
	CHECK(roots == std::vector<ecs::Entity>{ecs::Entity{17}});

	UiDocument valid;
	valid.Roots.push_back(Node("root", "Frame", "Root"));
	ecs::Store populated("gui_document.import_populated");
	RegisterGuiClasses();
	const ecs::Entity existing = populated.CreateInstance(GuiClass("Frame"), "Existing");
	REQUIRE(ImportDocument(populated, valid, roots, report));
	CHECK(populated.Alive(existing));
	CHECK(populated.InstanceNameOf(existing).Text() == "Existing");
	REQUIRE(roots.size() == 1);
	CHECK(roots.front() != existing);
	CHECK(populated.InstanceNameOf(roots.front()).Text() == "Root");
}

TEST_CASE("a GUI document retains supported modifier classes and their tree placement", "[gui][document]") {
	ecs::Store source("gui_document.modifiers_source");
	RegisterGuiClasses();
	const ecs::Entity screen = source.CreateInstance(GuiClass("ScreenGui"), "Screen");
	const ecs::Entity frame = source.CreateInstance(GuiClass("Frame"), "Panel");
	source.SetParent(frame, screen);
	for (const std::string_view className : std::array{
			 "UIListLayout",
			 "UIGridLayout",
			 "UITableLayout",
			 "UIPageLayout",
			 "UIAspectRatioConstraint",
			 "UISizeConstraint",
			 "UITextSizeConstraint",
			 "UIPadding",
			 "UICorner",
			 "UIScale",
			 "UIFlexItem",
			 "UIDragDetector",
			 "UIModalScope",
			 "UIAnimation",
			 "UIBinding"
		 }) {
		source.SetParent(source.CreateInstance(GuiClass(className), std::string(className)), frame);
	}
	const ecs::Entity stroke = source.CreateInstance(GuiClass("UIStroke"), "Stroke");
	source.SetParent(stroke, frame);
	source.SetParent(source.CreateInstance(GuiClass("UIGradient"), "Gradient"), stroke);
	const ecs::Entity scrolling = source.CreateInstance(GuiClass("ScrollingFrame"), "Rows");
	source.SetParent(scrolling, frame);
	const ecs::Entity collection = source.CreateInstance(GuiClass("UIVirtualCollection"), "Collection");
	source.SetParent(collection, scrolling);
	const ecs::Entity row = source.CreateInstance(GuiClass("Frame"), "Row");
	source.SetParent(row, collection);

	UiDocument document;
	DocumentReport report;
	REQUIRE(ExportDocument(source, screen, document, report));
	std::vector<std::string> classes;
	CollectClasses(document.Roots.front(), classes);
	for (const std::string_view className : std::array{
			 "UIListLayout",
			 "UIGridLayout",
			 "UITableLayout",
			 "UIPageLayout",
			 "UIAspectRatioConstraint",
			 "UISizeConstraint",
			 "UITextSizeConstraint",
			 "UIPadding",
			 "UICorner",
			 "UIStroke",
			 "UIScale",
			 "UIFlexItem",
			 "UIGradient",
			 "UIDragDetector",
			 "UIModalScope",
			 "UIAnimation",
			 "UIBinding",
			 "UIVirtualCollection"
		 }) {
		CHECK(std::find(classes.begin(), classes.end(), className) != classes.end());
	}

	ecs::Store destination("gui_document.modifiers_destination");
	std::vector<ecs::Entity> roots;
	REQUIRE(ImportDocument(destination, document, roots, report));
	UiDocument roundTrip;
	REQUIRE(ExportDocument(destination, roots.front(), roundTrip, report));
	std::vector<std::string> importedClasses;
	CollectClasses(roundTrip.Roots.front(), importedClasses);
	CHECK(importedClasses == classes);

	UiDocument misplaced;
	misplaced.Roots.push_back(Node("root", "Frame", "Root"));
	misplaced.Roots.front().Children.push_back(Node("collection", "UIVirtualCollection", "Collection"));
	CHECK_FALSE(ValidateDocument(misplaced, report));
	UiDocument rootModifier;
	rootModifier.Roots.push_back(Node("binding", "UIBinding", "Binding"));
	CHECK_FALSE(ValidateDocument(rootModifier, report));
}

TEST_CASE("validation rejects invalid data before a caller can mutate a store", "[gui][document]") {
	UiDocument document;
	document.Roots.push_back(Node("one", "Frame", "First"));
	document.Roots.push_back(Node("two", "TextLabel", "Second"));
	DocumentProperty invalid;
	invalid.Name = "TextSize";
	invalid.Type = ecs::PropertyType::Float;
	invalid.Value.Type = ecs::PropertyType::Float;
	invalid.Value.Float = std::numeric_limits<float>::quiet_NaN();
	document.Roots[1].Properties.push_back(invalid);
	DocumentReport report;
	CHECK_FALSE(ValidateDocument(document, report));
	CHECK_FALSE(report.Ok());
}

TEST_CASE("document validation rejects unknown classes bad references and hostile sizes", "[gui][document]") {
	UiDocument document;
	document.Roots.push_back(Node("one", "NoSuchGui", "Bad"));
	DocumentReport report;
	CHECK_FALSE(ValidateDocument(document, report));

	document.Roots.front().Class = "Frame";
	DocumentProperty reference;
	reference.Name = "NextSelectionDown";
	reference.Type = ecs::PropertyType::Reference;
	reference.Value.Type = ecs::PropertyType::Reference;
	reference.Reference = "missing";
	document.Roots.front().Properties.push_back(reference);
	CHECK_FALSE(ValidateDocument(document, report));

	document.Roots.front().Properties.clear();
	DocumentLimits limits;
	limits.MaximumStringBytes = 2;
	CHECK_FALSE(ValidateDocument(document, report, limits));

	document.Roots.front().Name = "\xC3";
	CHECK_FALSE(ValidateDocument(document, report));
}

TEST_CASE("document validation stops at deep and wide hostile subtrees", "[gui][document]") {
	UiDocument deep;
	deep.Roots.push_back(Node("root", "Frame", "Root"));
	DocumentNode *leaf = &deep.Roots.front();
	for (uint32_t index = 0; index < DocumentLimits::HARD_MAXIMUM_DEPTH; index++) {
		leaf->Children.push_back(Node("deep" + std::to_string(index), "Frame", "Node"));
		leaf = &leaf->Children.back();
	}
	DocumentReport report;
	CHECK_FALSE(ValidateDocument(deep, report));
	CHECK(report.Issues.size() == 1);
	CHECK(report.Issues.front().Message == "hierarchy depth limit exceeded");

	UiDocument wide;
	wide.Roots.push_back(Node("root", "Frame", "Root"));
	for (uint32_t index = 0; index <= DocumentLimits::HARD_MAXIMUM_CHILDREN; index++) {
		wide.Roots.front().Children.push_back(Node("wide" + std::to_string(index), "Frame", "Node"));
	}
	CHECK_FALSE(ValidateDocument(wide, report));
	CHECK(report.Issues.size() == 1);
	CHECK(report.Issues.front().Message == "child limit exceeded");
}

TEST_CASE("document reports are bounded and export refuses a wide GUI subtree", "[gui][document]") {
	UiDocument invalid;
	for (uint32_t index = 0; index <= DocumentLimits::HARD_MAXIMUM_ISSUES; index++) {
		invalid.Roots.push_back(Node("invalid" + std::to_string(index), "NoSuchGui", "Bad"));
	}
	DocumentReport report;
	CHECK_FALSE(ValidateDocument(invalid, report));
	CHECK(report.Truncated);
	CHECK(report.Issues.size() == DocumentLimits::HARD_MAXIMUM_ISSUES);

	ecs::Store store("gui_document.export_wide");
	RegisterGuiClasses();
	const ecs::Entity root = store.CreateInstance(GuiClass("Frame"), "Root");
	for (uint32_t index = 0; index <= DocumentLimits::HARD_MAXIMUM_CHILDREN; index++) {
		store.SetParent(store.CreateInstance(GuiClass("TextLabel"), "Child"), root);
	}
	UiDocument exported;
	CHECK_FALSE(ExportDocument(store, root, exported, report));
	CHECK(exported.Roots.empty());
}

TEST_CASE("a GUI document preserves visual property provenance", "[gui][document]") {
	ecs::Store source("gui_document.style_provenance_source");
	RegisterGuiClasses();
	const ecs::Entity frame = source.CreateInstance(GuiClass("Frame"), "Panel");
	const ecs::Entity label = source.CreateInstance(GuiClass("TextLabel"), "Title");
	source.SetParent(label, frame);

	UiDocument document;
	DocumentReport report;
	REQUIRE(ExportDocument(source, frame, document, report));
	const auto &defaultProperties = document.Roots.front().Children.front().Properties;
	CHECK(
		std::none_of(
			defaultProperties.begin(), defaultProperties.end(), [](const DocumentProperty &property) {
				return property.Name == "TextColor3" || property.Name == "TextTransparency";
			}
		)
	);

	const core::Color3 defaultColor = Label().Color;
	REQUIRE(source.SetProperty(label, core::Name("TextColor3"), &defaultColor, sizeof(defaultColor)));
	REQUIRE(ExportDocument(source, frame, document, report));
	const auto &explicitProperties = document.Roots.front().Children.front().Properties;
	CHECK(
		std::any_of(
			explicitProperties.begin(), explicitProperties.end(), [](const DocumentProperty &property) {
				return property.Name == "TextColor3";
			}
		)
	);

	ecs::Store destination("gui_document.style_provenance_destination");
	std::vector<ecs::Entity> roots;
	REQUIRE(ImportDocument(destination, document, roots, report));
	ecs::Entity imported = ecs::NULL_ENTITY;
	destination.EachChild(roots.front(), [&](ecs::Entity child) { imported = child; });
	const StyleDirect *direct = destination.Get<StyleDirect>(imported);
	REQUIRE(direct != nullptr);
	CHECK(direct->Has(StyleDirectProperty::TextColor));
	CHECK_FALSE(direct->Has(StyleDirectProperty::TextTransparency));
}

TEST_CASE("a GUI document retains an external collector theme", "[gui][document][style]") {
	ecs::Store source("gui_document.theme_source");
	RegisterGuiClasses();
	const ecs::Entity screen = source.CreateInstance(GuiClass("ScreenGui"), "Screen");
	const ecs::Entity theme = source.CreateInstance(GuiClass("UITheme"), "Palette");
	UITheme *tokens = source.GetMutable<UITheme>(theme);
	REQUIRE(tokens != nullptr);
	REQUIRE(tokens->Tokens.Set({core::Name("TextColor3"), StyleValue::FromColor({0.2f, 0.5f, 0.8f})}));
	ThemeBinding *binding = source.GetMutable<ThemeBinding>(screen);
	REQUIRE(binding != nullptr);
	binding->Theme = theme;

	UiDocument document;
	DocumentReport report;
	REQUIRE(ExportDocument(source, screen, document, report));
	REQUIRE(document.Themes.size() == 1);
	CHECK(document.Themes.front().Name == "Palette");
	REQUIRE(document.Themes.front().Tokens.Find(core::Name("TextColor3")) != nullptr);
	const auto themeProperty = std::find_if(
		document.Roots.front().Properties.begin(),
		document.Roots.front().Properties.end(),
		[](const DocumentProperty &property) { return property.Name == "Theme"; }
	);
	REQUIRE(themeProperty != document.Roots.front().Properties.end());
	CHECK(themeProperty->Reference == document.Themes.front().Id);

	ecs::Store destination("gui_document.theme_destination");
	std::vector<ecs::Entity> roots;
	std::vector<ecs::Entity> themes;
	REQUIRE(ImportDocument(destination, document, roots, report, {}, &themes));
	REQUIRE(roots.size() == 1);
	REQUIRE(themes.size() == 1);
	const ThemeBinding *importedBinding = destination.Get<ThemeBinding>(roots.front());
	REQUIRE(importedBinding != nullptr);
	const UITheme *importedTheme = destination.Get<UITheme>(importedBinding->Theme);
	REQUIRE(importedTheme != nullptr);
	CHECK(themes.front() == importedBinding->Theme);
	REQUIRE(importedTheme->Tokens.Find(core::Name("TextColor3")) != nullptr);
	CHECK(importedTheme->Tokens.Find(core::Name("TextColor3"))->Color.B == 0.8f);
}

TEST_CASE("a GUI document binary codec is canonical and imports through its decode seam", "[gui][document]") {
	ecs::Store source("gui_document.binary_source");
	RegisterGuiClasses();
	const ecs::Entity frame = source.CreateInstance(GuiClass("Frame"), "Panel");
	const ecs::Entity label = source.CreateInstance(GuiClass("TextLabel"), "Title");
	source.SetParent(label, frame);
	Label text;
	text.Text = "Hello binary";
	source.Set(label, text);

	UiDocument document;
	DocumentReport report;
	REQUIRE(ExportDocument(source, frame, document, report));
	core::ByteWriter first;
	REQUIRE(EncodeDocument(document, first, report));

	core::ByteReader reader(first.Bytes());
	UiDocument decoded;
	REQUIRE(DecodeDocument(reader, decoded, report));
	core::ByteWriter second;
	REQUIRE(EncodeDocument(decoded, second, report));
	CHECK(first.Bytes().size() == second.Bytes().size());
	CHECK(std::equal(first.Bytes().begin(), first.Bytes().end(), second.Bytes().begin()));

	ecs::Store destination("gui_document.binary_destination");
	std::vector<ecs::Entity> roots;
	core::ByteReader importReader(first.Bytes());
	REQUIRE(DecodeAndImportDocument(destination, importReader, roots, report));
	REQUIRE(roots.size() == 1);
	CHECK(destination.InstanceNameOf(roots.front()).Text() == "Panel");
}

TEST_CASE("a GUI document round-trips authored component state outside properties", "[gui][document]") {
	ecs::Store source("gui_document.component_payload_source");
	RegisterGuiClasses();
	const ecs::Entity frame = source.CreateInstance(GuiClass("Frame"), "Panel");
	StyleClass *classes = source.GetMutable<StyleClass>(frame);
	REQUIRE(classes != nullptr);
	REQUIRE(classes->Names.Add(core::Name("dialog")));

	const ecs::Entity style = source.CreateInstance(GuiClass("UIStyle"), "PanelStyle");
	source.SetParent(style, frame);
	UIStyle *rule = source.GetMutable<UIStyle>(style);
	REQUIRE(rule != nullptr);
	rule->Rule.Class = core::Name("dialog");
	REQUIRE(
		rule->Rule.Declarations.Set({core::Name("TextColor3"), StyleValue::FromColor({0.1f, 0.2f, 0.3f})})
	);

	const ecs::Entity scrolling = source.CreateInstance(GuiClass("ScrollingFrame"), "Rows");
	source.SetParent(scrolling, frame);
	const ecs::Entity collection = source.CreateInstance(GuiClass("UIVirtualCollection"), "Collection");
	source.SetParent(collection, scrolling);
	VirtualCollection *virtualCollection = source.GetMutable<VirtualCollection>(collection);
	REQUIRE(virtualCollection != nullptr);
	virtualCollection->ItemCount = 4;
	virtualCollection->FixedExtent = 31.0f;
	virtualCollection->Overscan = 5;
	virtualCollection->Revision = 9;
	virtualCollection->Page.First = 1;
	virtualCollection->Page.Records.push_back({"two", {}, 34.0f});
	REQUIRE(ValidateVirtualCollection(*virtualCollection));

	UiDocument document;
	DocumentReport report;
	REQUIRE(ExportDocument(source, frame, document, report));
	core::ByteWriter encoded;
	REQUIRE(EncodeDocument(document, encoded, report));
	CHECK_FALSE(document.Roots.front().Components.empty());

	ecs::Store destination("gui_document.component_payload_destination");
	std::vector<ecs::Entity> roots;
	REQUIRE(ImportDocument(destination, document, roots, report));
	const StyleClass *importedClasses = destination.Get<StyleClass>(roots.front());
	REQUIRE(importedClasses != nullptr);
	REQUIRE(importedClasses->Names.Names().size() == 1);
	CHECK(importedClasses->Names.Names().front() == core::Name("dialog"));

	ecs::Entity importedStyle = ecs::NULL_ENTITY;
	ecs::Entity importedScrolling = ecs::NULL_ENTITY;
	destination.EachChild(roots.front(), [&](ecs::Entity child) {
		if (destination.ClassOf(child) == GuiClass("UIStyle")) importedStyle = child;
		if (destination.ClassOf(child) == GuiClass("ScrollingFrame")) importedScrolling = child;
	});
	const UIStyle *importedRule = destination.Get<UIStyle>(importedStyle);
	REQUIRE(importedRule != nullptr);
	CHECK(importedRule->Rule.Class == core::Name("dialog"));
	REQUIRE(importedRule->Rule.Declarations.Find(core::Name("TextColor3")) != nullptr);
	CHECK(importedRule->Rule.Declarations.Find(core::Name("TextColor3"))->Color.B == 0.3f);

	ecs::Entity importedCollectionEntity = ecs::NULL_ENTITY;
	destination.EachChild(importedScrolling, [&](ecs::Entity child) { importedCollectionEntity = child; });
	const VirtualCollection *importedCollection =
		destination.Get<VirtualCollection>(importedCollectionEntity);
	REQUIRE(importedCollection != nullptr);
	CHECK(importedCollection->Page.First == 1);
	CHECK(importedCollection->Page.Records.front().Key == "two");
	CHECK(importedCollection->Overscan == 5);
}

TEST_CASE(
	"a GUI document binary codec rejects malformed bounded input without publishing output", "[gui][document]"
) {
	UiDocument valid;
	valid.Roots.push_back(Node("root", "Frame", "Root"));
	DocumentReport report;
	core::ByteWriter encoded;
	REQUIRE(EncodeDocument(valid, encoded, report));

	std::vector<std::byte> trailing(encoded.Bytes().begin(), encoded.Bytes().end());
	trailing.push_back(std::byte{0});
	UiDocument unchanged;
	unchanged.Roots.push_back(Node("keep", "Frame", "Keep"));
	core::ByteReader trailingReader(trailing);
	CHECK_FALSE(DecodeDocument(trailingReader, unchanged, report));
	CHECK(unchanged.Roots.front().Id == "keep");

	core::ByteWriter hostile;
	hostile.WriteUInt64(0x5549'444F'4355'4D45ull);
	hostile.WriteUInt32(UI_DOCUMENT_VERSION);
	hostile.WriteUInt32(0);
	hostile.WriteUInt32(1);
	hostile.WriteString("root");
	hostile.WriteString("Frame");
	hostile.WriteString("Root");
	hostile.WriteUInt32(1);
	hostile.WriteString("TextSize");
	hostile.WriteUInt8(static_cast<uint8_t>(ecs::PropertyType::NumberSequence));
	hostile.WriteUInt32(core::SEQUENCE_CAPACITY + 1);
	core::ByteReader hostileReader(hostile.Bytes());
	CHECK_FALSE(DecodeDocument(hostileReader, unchanged, report));
	CHECK(hostileReader.Failed());
	CHECK(unchanged.Roots.front().Id == "keep");

	ecs::Store destination("gui_document.binary_failure_destination");
	std::vector<ecs::Entity> roots{ecs::Entity{91}};
	core::ByteReader failedImport(hostile.Bytes());
	CHECK_FALSE(DecodeAndImportDocument(destination, failedImport, roots, report));
	CHECK(roots == std::vector<ecs::Entity>{ecs::Entity{91}});
}

TEST_CASE(
	"a GUI document decoder applies binary limits before replacing its destination", "[gui][document]"
) {
	UiDocument encoded;
	encoded.Roots.push_back(Node("one", "Frame", "One"));
	encoded.Roots.push_back(Node("two", "Frame", "Two"));
	DocumentReport report;
	core::ByteWriter writer;
	REQUIRE(EncodeDocument(encoded, writer, report));

	UiDocument unchanged;
	unchanged.Roots.push_back(Node("keep", "Frame", "Keep"));
	DocumentLimits countLimit;
	countLimit.MaximumNodes = 1;
	core::ByteReader countReader(writer.Bytes());
	CHECK_FALSE(DecodeDocument(countReader, unchanged, report, countLimit));
	CHECK(countReader.Failed());
	REQUIRE(unchanged.Roots.size() == 1);
	CHECK(unchanged.Roots.front().Id == "keep");

	UiDocument nested;
	nested.Roots.push_back(Node("root", "Frame", "Root"));
	nested.Roots.front().Children.push_back(Node("child", "Frame", "Child"));
	core::ByteWriter nestedWriter;
	REQUIRE(EncodeDocument(nested, nestedWriter, report));
	DocumentLimits depthLimit;
	depthLimit.MaximumDepth = 1;
	core::ByteReader depthReader(nestedWriter.Bytes());
	CHECK_FALSE(DecodeDocument(depthReader, unchanged, report, depthLimit));
	CHECK(depthReader.Failed());
	CHECK(unchanged.Roots.front().Id == "keep");

	core::ByteWriter invalidUtf8;
	invalidUtf8.WriteUInt64(0x5549'444F'4355'4D45ull);
	invalidUtf8.WriteUInt32(UI_DOCUMENT_VERSION);
	invalidUtf8.WriteUInt32(0);
	invalidUtf8.WriteUInt32(1);
	invalidUtf8.WriteString("\xC3");
	invalidUtf8.WriteString("Frame");
	invalidUtf8.WriteString("Root");
	invalidUtf8.WriteUInt32(0);
	invalidUtf8.WriteUInt32(0);
	invalidUtf8.WriteUInt32(0);
	invalidUtf8.WriteUInt32(0);
	core::ByteReader utf8Reader(invalidUtf8.Bytes());
	CHECK_FALSE(DecodeDocument(utf8Reader, unchanged, report));
	CHECK(unchanged.Roots.front().Id == "keep");
}
