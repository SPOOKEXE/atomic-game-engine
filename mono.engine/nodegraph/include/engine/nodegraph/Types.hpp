#pragma once

// The vocabulary: what a wire carries, and what a knob is.
//
// Nothing in this header knows about drawing, about ImGui or about a graph. It
// is the table every other module agrees on, and it is deliberately the only
// place a string means anything: a port's type is a **string id**, because that
// id survives a save file and an ordinal does not — reorder two registrations
// and every saved graph would connect different things.
//
// **A picture belongs to the wire, not to the node.** `DataType::Preview` is
// what lets a panel draw a node's *inputs*: an input's payload was made
// upstream by a type the reader has never heard of, and the only thing both
// ends agree on is what the wire carries.

#include <any>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace engine::nodegraph {

	// A colour, 0..1 per channel. Not an ImGui type, so the model layer stays
	// free of the toolkit.
	struct Colour {
		// The channels, 0..1. Straight values, not premultiplied.
		//@{
		float R = 1.0f;
		float G = 1.0f;
		float B = 1.0f;
		float A = 1.0f;
		//@}

		// 0xRRGGBB, alpha 1.
		static Colour Hex(uint32_t rgb);
	};

	// A small square picture of whatever a node produced.
	//
	// **The library never learns what a payload is, so somebody else says how to
	// draw one.** A height field becomes a grey ramp, a colour field becomes
	// itself, and a payload carrying a number has no picture at all — which is
	// the honest answer rather than a grey square.
	struct PreviewImage {
		// Pixels a side. Square, because a node's thumbnail slot is.
		uint32_t Side = 0;

		// `Side * Side * 4` bytes, red first, top row first — the layout every
		// texture upload this has met wants, so a host uploads it as it stands.
		std::vector<uint8_t> Rgba;

		// Whether the buffer matches the side it claims.
		//
		// @return `true` when there is a picture to draw.
		bool Valid() const {
			return Side > 0 && Rgba.size() == static_cast<size_t>(Side) * Side * 4;
		}
	};

	// A payload read as a square grid of heights.
	//
	// **The one thing a 3-D view needs and a picture cannot give it.** A
	// thumbnail of a height field is already shaded, so re-reading the shading
	// as elevation would put the lighting into the geometry. This is the
	// unshaded numbers.
	struct Surface {
		// Samples a side. Square, like the picture it came from.
		uint32_t Side = 0;

		// `Side * Side`, row major, top row first. Expected in 0..1; anything
		// else still draws, just taller.
		std::vector<float> Heights;

		// Whether the buffer matches the side it claims.
		//
		// **Two samples a side at minimum**, because one height is a point and
		// the renderer interpolates between neighbours.
		//
		// @return `true` when there is a surface to draw.
		bool Valid() const {
			return Side > 1 && Heights.size() == static_cast<size_t>(Side) * Side;
		}

		// One sample, with the edges clamped.
		//
		// **Clamped rather than wrapped**, because a surface is a patch and not
		// a tile: wrapping would join the far edge to the near one and put a
		// cliff across anything that is not seamless.
		//
		// @param x Column, clamped to the last.
		// @param y Row, clamped to the last.
		// @return The height there.
		float At(uint32_t x, uint32_t y) const {
			const uint32_t cx = x < Side ? x : Side - 1;
			const uint32_t cy = y < Side ? y : Side - 1;
			return Heights[static_cast<size_t>(cy) * Side + cx];
		}
	};

	// One kind of thing an edge may carry.
	struct DataType {
		// What a link compares. A **string** and not an ordinal: this crosses a
		// save file, and an id derived from registration order would connect
		// different things the moment two registrations swapped.
		std::string Id;

		// What a person reads on a port and in the palette.
		std::string Label;

		// The colour its ports and links are drawn in, so a wire's kind is
		// legible without reading anything.
		Colour Tint;

		// One sentence for the tooltip. Empty is allowed and shows nothing.
		std::string Description;

		// Turns a payload carried on a wire of this type into a picture.
		//
		// **On the data type rather than on the node**, which is what makes an
		// inspector able to show a node's *inputs*. An input's payload was made
		// upstream by a node type this one has never heard of; the only thing
		// both ends agree on is the wire, so the wire is where the knowledge
		// belongs. A node type may still override it — see `NodeType::Preview`
		// — for the case where two nodes on one wire type want different
		// pictures.
		std::function<bool(const std::any &, PreviewImage &)> Preview;

		// One line saying what a payload is: `"scalar field 256²"`, `"0.4213"`.
		//
		// Shown wherever a picture will not do, which is most of an inspector's
		// rows. Empty for a type nobody taught, and the inspector then says the
		// only true thing left — that something is there.
		std::function<std::string(const std::any &)> Describe;

		// Reads a payload as elevation, for the 3-D view. Empty for a wire that
		// is not carrying a landscape, which is most of them — and the inspector
		// then offers no 3-D button rather than a flat plane.
		std::function<bool(const std::any &, Surface &)> Heights;
	};

	// The wildcard, spelled once.
	inline constexpr const char *ANY_TYPE = "data.ANY";

	// Every registered data type.
	class DataTypes {
	  public:
		// Adds a type, or replaces the one already under its id.
		//
		// @param type The type to register.
		static void Register(const DataType &type);

		// The type under an id.
		//
		// @param id The id to look up.
		// @return The type, or null when nothing registered it — which is what
		//         a document naming a type this build does not have produces.
		static const DataType *Find(const std::string &id);

		// Identical ids, or either side being the wildcard.
		//
		// **Deliberately not a conversion table.** An implicit conversion is a
		// decision taken where nobody can see it, and "the drop went red" only
		// means something while the rule is a plain yes or no.
		static bool CanConnect(const std::string &from, const std::string &to);

		// Every registered type, in registration order.
		//
		// @return The types. Valid until the next `Register`.
		static const std::vector<DataType> &All();
	};

	// What a knob is, before anybody has drawn one.
	enum class WidgetKind : uint8_t { Slider, Number, Text, Toggle, Select, Colour };

	// One value a node carries. A tagged struct rather than a variant, so that
	// saving it is a switch rather than a visitor.
	struct Value {
		// Which of the four below carries the value. The tag, and the only
		// field that is always meaningful.
		WidgetKind Kind = WidgetKind::Number;

		// The number, for `Slider` and `Number` — and the chosen index for
		// `Select`, which is why a choice saves as its position and its options
		// are saved beside it.
		double Number = 0.0;

		// The state, for `Toggle`.
		bool Flag = false;

		// The string, for `Text`.
		std::string Text;

		// The colour, for `Colour`.
		engine::nodegraph::Colour Tint;

		// Whether both carry the same value.
		//
		// **Compares only the field `Kind` names**, so two values that differ in
		// a field neither is using are equal — which is what makes this usable
		// as the "did this knob move" test the undo log needs.
		bool operator==(const Value &other) const;
	};

	// A knob's declaration.
	//
	// **One schema, three consumers** — the painter, the hit test and any
	// inspector. A widget drawn where it cannot be clicked is the failure that
	// arrangement makes impossible.
	struct WidgetSpec {
		// Stable key. Saved, so it is a name and never an index.
		std::string Key;

		// What a person reads beside the knob.
		std::string Label;

		// Which knob to draw, and therefore which field of `Value` it writes.
		WidgetKind Kind = WidgetKind::Slider;

		// The range a `Slider` spans. Ignored by every other kind.
		//@{
		double Minimum = 0.0;
		double Maximum = 1.0;
		//@}

		// How far one drag notch moves a `Slider` or a `Number`.
		double Step = 0.01;

		// Decimal places shown. Display only — the value keeps its precision.
		int Precision = 2;

		// The choices a `Select` offers, in order. `Value::Number` indexes this.
		std::vector<std::string> Options;

		// What a freshly placed node starts with.
		Value Default;
	};

	// A port's declaration.
	struct PortSpec {
		// What a person reads on the port, and the key a link names it by.
		std::string Name;

		// Which `DataType::Id` may be connected here, or `ANY_TYPE`.
		std::string Type;
	};

	// Registration helpers, so a node type reads as a declaration.
	//@{
	PortSpec Port(std::string name, std::string type);
	WidgetSpec Slider(std::string key, std::string label, double minimum, double maximum, double value);
	WidgetSpec Toggle(std::string key, std::string label, bool value);
	WidgetSpec Select(std::string key, std::string label, std::vector<std::string> options, int chosen);
	WidgetSpec Number(std::string key, std::string label, double value);
	WidgetSpec Text(std::string key, std::string label, std::string value);
	//@}

	// The host's own chrome: the few colours and the one scale factor that
	// belong to the program this library is embedded in rather than to a graph.
	//
	// **Process-wide and not a field on `Style`**, because what reads it is a
	// popup, a section heading and an inspector row — chrome drawn *around* a
	// graph rather than any part of one. Dear ImGui's own style, which all of
	// them sit inside, is process-wide for exactly that reason. A second canvas
	// restyles its nodes through `Style`; it does not get a second font scale.
	struct Chrome {
		// A caption, a port name under a thumbnail, a stage that has not run.
		uint32_t Muted = 0xFF8A8A8A;

		// The stage a run is on now.
		uint32_t Accent = 0xFF4CA6FF;

		// A run that failed.
		uint32_t Warning = 0xFF3CB4FF;

		// Logical pixels to real ones. Every fixed size the library asks ImGui
		// for goes through this, so a popup is the same size on a display the
		// host has scaled as on one it has not.
		float Scale = 1.0f;
	};

	// The chrome this process draws with. Defaults are the standalone demo's;
	// a host with a theme assigns its own once, at startup.
	//
	// @return The table, mutable.
	Chrome &HostChrome();

	// A fixed size in the host's pixels — `HostChrome().Scale` applied, spelled
	// once so that no call site forgets it.
	//
	// @param value The size in logical pixels.
	// @return What to hand ImGui.
	inline float Scaled(float value) {
		return value * HostChrome().Scale;
	}
}
