#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace engine::bake {

	// Roblox enum members by Roblox's own numbering, for the properties an
	// import can use.
	//
	// A file stores an enum as a bare number that indexes Roblox's table. This
	// spells that table out for a short, explicit list of properties, so the
	// importer hands up a member *name* and the engine's own `EnumTable` decides
	// whether it knows that name. Nothing here is derived from declaration order
	// on either side. A property or value missing from this list stays a lost
	// property, reported by name.
	struct RobloxEnumMember {
		uint32_t Value;
		std::string_view Name;
	};

	// One property whose enum this table knows. `ClassName` empty matches any class.
	struct RobloxEnumProperty {
		std::string_view ClassName;
		std::string_view PropertyName;
		std::span<const RobloxEnumMember> Members;
	};

	namespace roblox_enums {
		constexpr std::array<RobloxEnumMember, 5> PART_TYPE{{
			{0, "Ball"},
			{1, "Block"},
			{2, "Cylinder"},
			{3, "Wedge"},
			{4, "CornerWedge"},
		}};

		constexpr std::array<RobloxEnumMember, 37> MATERIAL{{
			{256, "Plastic"},	   {272, "SmoothPlastic"}, {288, "Neon"},		  {512, "Wood"},
			{528, "WoodPlanks"},   {784, "Marble"},		   {788, "Basalt"},		  {800, "Slate"},
			{804, "CrackedLava"},  {816, "Concrete"},	   {820, "Limestone"},	  {832, "Granite"},
			{836, "Pavement"},	   {848, "Brick"},		   {864, "Pebble"},		  {880, "Cobblestone"},
			{896, "Rock"},		   {912, "Sandstone"},	   {1040, "CorrodedMetal"}, {1056, "DiamondPlate"},
			{1072, "Foil"},		   {1088, "Metal"},		   {1280, "Grass"},		  {1284, "LeafyGrass"},
			{1296, "Sand"},		   {1312, "Fabric"},	   {1328, "Snow"},		  {1344, "Mud"},
			{1360, "Ground"},	   {1376, "Asphalt"},	   {1392, "Salt"},		  {1536, "Ice"},
			{1552, "Glacier"},	   {1568, "Glass"},		   {1584, "ForceField"},  {1792, "Air"},
			{2048, "Water"},
		}};

		constexpr std::array<RobloxEnumMember, 6> NORMAL_ID{{
			{0, "Right"}, {1, "Top"}, {2, "Back"}, {3, "Left"}, {4, "Bottom"}, {5, "Front"},
		}};

		constexpr std::array<RobloxEnumMember, 10> SURFACE_TYPE{{
			{0, "Smooth"}, {1, "Glue"}, {2, "Weld"}, {3, "Studs"}, {4, "Inlet"},
			{5, "Universal"}, {6, "Hinge"}, {7, "Motor"}, {8, "SteppingMotor"}, {10, "SmoothNoOutlines"},
		}};

		constexpr std::array<RobloxEnumMember, 3> TEXT_X_ALIGNMENT{{{0, "Left"}, {1, "Right"}, {2, "Center"}}};
		constexpr std::array<RobloxEnumMember, 3> TEXT_Y_ALIGNMENT{{{0, "Top"}, {1, "Center"}, {2, "Bottom"}}};
		constexpr std::array<RobloxEnumMember, 3> SORT_ORDER{{{0, "Name"}, {1, "Custom"}, {2, "LayoutOrder"}}};
		constexpr std::array<RobloxEnumMember, 2> FILL_DIRECTION{{{0, "Horizontal"}, {1, "Vertical"}}};
		constexpr std::array<RobloxEnumMember, 3> HORIZONTAL_ALIGNMENT{{{0, "Center"}, {1, "Left"}, {2, "Right"}}};
		constexpr std::array<RobloxEnumMember, 3> VERTICAL_ALIGNMENT{{{0, "Center"}, {1, "Top"}, {2, "Bottom"}}};
		constexpr std::array<RobloxEnumMember, 5> SCALE_TYPE{{
			{0, "Stretch"}, {1, "Slice"}, {2, "Tile"}, {3, "Fit"}, {4, "Crop"},
		}};
		constexpr std::array<RobloxEnumMember, 3> SIZE_CONSTRAINT{{
			{0, "RelativeXY"}, {1, "RelativeXX"}, {2, "RelativeYY"},
		}};
		constexpr std::array<RobloxEnumMember, 2> Z_INDEX_BEHAVIOR{{{0, "Global"}, {1, "Sibling"}}};
		constexpr std::array<RobloxEnumMember, 4> AUTOMATIC_SIZE{{{0, "None"}, {1, "X"}, {2, "Y"}, {3, "XY"}}};
		constexpr std::array<RobloxEnumMember, 3> BORDER_MODE{{{0, "Outline"}, {1, "Middle"}, {2, "Inset"}}};
		constexpr std::array<RobloxEnumMember, 3> TEXT_TRUNCATE{{{0, "None"}, {1, "AtEnd"}, {2, "SplitWord"}}};
		constexpr std::array<RobloxEnumMember, 4> START_CORNER{{
			{0, "TopLeft"}, {1, "TopRight"}, {2, "BottomLeft"}, {3, "BottomRight"},
		}};
		constexpr std::array<RobloxEnumMember, 2> APPLY_STROKE_MODE{{{0, "Contextual"}, {1, "Border"}}};
		constexpr std::array<RobloxEnumMember, 3> LINE_JOIN_MODE{{{0, "Round"}, {1, "Bevel"}, {2, "Miter"}}};
		constexpr std::array<RobloxEnumMember, 2> ASPECT_TYPE{{{0, "FitWithinMaxSize"}, {1, "ScaleWithParentSize"}}};
		constexpr std::array<RobloxEnumMember, 2> DOMINANT_AXIS{{{0, "Width"}, {1, "Height"}}};
		constexpr std::array<RobloxEnumMember, 3> SCROLLING_DIRECTION{{{1, "X"}, {2, "Y"}, {4, "XY"}}};
		constexpr std::array<RobloxEnumMember, 3> SCROLL_BAR_INSET{{{0, "None"}, {1, "ScrollBar"}, {2, "Always"}}};
		constexpr std::array<RobloxEnumMember, 2> VERTICAL_SCROLL_BAR_POSITION{{{0, "Right"}, {1, "Left"}}};
		constexpr std::array<RobloxEnumMember, 2> RESAMPLER_MODE{{{0, "Default"}, {1, "Pixelated"}}};
		constexpr std::array<RobloxEnumMember, 4> PARTICLE_ORIENTATION{{
			{0, "FacingCamera"}, {1, "FacingCameraWorldUp"}, {2, "VelocityParallel"}, {3, "VelocityPerpendicular"},
		}};
		constexpr std::array<RobloxEnumMember, 4> PARTICLE_SHAPE{{
			{0, "Box"}, {1, "Sphere"}, {2, "Cylinder"}, {3, "Disc"},
		}};
		constexpr std::array<RobloxEnumMember, 3> PARTICLE_SHAPE_IN_OUT{{
			{0, "Outward"}, {1, "Inward"}, {2, "InAndOut"},
		}};
		constexpr std::array<RobloxEnumMember, 2> PARTICLE_SHAPE_STYLE{{{0, "Volume"}, {1, "Surface"}}};
		constexpr std::array<RobloxEnumMember, 4> PARTICLE_FLIPBOOK_LAYOUT{{
			{0, "None"}, {1, "Grid2x2"}, {2, "Grid4x4"}, {3, "Grid8x8"},
		}};
		constexpr std::array<RobloxEnumMember, 4> PARTICLE_FLIPBOOK_MODE{{
			{0, "Loop"}, {1, "OneShot"}, {2, "PingPong"}, {3, "Random"},
		}};

		constexpr std::array<RobloxEnumProperty, 38> PROPERTIES{{
			{"Part", "Shape", PART_TYPE},
			{"", "Material", MATERIAL},
			{"", "Face", NORMAL_ID},
			{"", "FrontSurface", SURFACE_TYPE},
			{"", "BackSurface", SURFACE_TYPE},
			{"", "LeftSurface", SURFACE_TYPE},
			{"", "RightSurface", SURFACE_TYPE},
			{"", "TopSurface", SURFACE_TYPE},
			{"", "BottomSurface", SURFACE_TYPE},
			{"", "TextXAlignment", TEXT_X_ALIGNMENT},
			{"", "TextYAlignment", TEXT_Y_ALIGNMENT},
			{"", "SortOrder", SORT_ORDER},
			{"", "FillDirection", FILL_DIRECTION},
			{"", "HorizontalAlignment", HORIZONTAL_ALIGNMENT},
			{"", "VerticalAlignment", VERTICAL_ALIGNMENT},
			{"", "ScaleType", SCALE_TYPE},
			{"", "SizeConstraint", SIZE_CONSTRAINT},
			{"", "ZIndexBehavior", Z_INDEX_BEHAVIOR},
			{"", "AutomaticSize", AUTOMATIC_SIZE},
			{"", "BorderMode", BORDER_MODE},
			{"", "TextTruncate", TEXT_TRUNCATE},
			{"", "StartCorner", START_CORNER},
			{"", "ApplyStrokeMode", APPLY_STROKE_MODE},
			{"", "LineJoinMode", LINE_JOIN_MODE},
			{"", "AspectType", ASPECT_TYPE},
			{"", "DominantAxis", DOMINANT_AXIS},
			{"", "ScrollingDirection", SCROLLING_DIRECTION},
			{"", "ScrollBarInset", SCROLL_BAR_INSET},
			{"", "HorizontalScrollBarInset", SCROLL_BAR_INSET},
			{"", "VerticalScrollBarInset", SCROLL_BAR_INSET},
			{"", "VerticalScrollBarPosition", VERTICAL_SCROLL_BAR_POSITION},
			{"", "ResampleMode", RESAMPLER_MODE},
			{"ParticleEmitter", "Orientation", PARTICLE_ORIENTATION},
			{"ParticleEmitter", "Shape", PARTICLE_SHAPE},
			{"ParticleEmitter", "ShapeInOut", PARTICLE_SHAPE_IN_OUT},
			{"ParticleEmitter", "ShapeStyle", PARTICLE_SHAPE_STYLE},
			{"ParticleEmitter", "FlipbookLayout", PARTICLE_FLIPBOOK_LAYOUT},
			{"ParticleEmitter", "FlipbookMode", PARTICLE_FLIPBOOK_MODE},
		}};
	}

	// The member name Roblox gives `value` for this class's property, or empty
	// when the property or the value is not in the table above.
	inline std::string_view RobloxEnumName(std::string_view className, std::string_view propertyName, uint32_t value) {
		for (const RobloxEnumProperty &property : roblox_enums::PROPERTIES) {
			if (property.PropertyName != propertyName ||
				(!property.ClassName.empty() && property.ClassName != className)) {
				continue;
			}
			for (const RobloxEnumMember &member : property.Members) {
				if (member.Value == value) {
					return member.Name;
				}
			}
			return {};
		}
		return {};
	}
}
