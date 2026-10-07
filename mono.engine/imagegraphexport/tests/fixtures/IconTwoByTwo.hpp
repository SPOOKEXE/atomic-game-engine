#pragma once

#include <array>
#include <cstdint>

// Original 2x2 icon file fixture, laid out independently from the encoder.
// DIB and mask layout: https://learn.microsoft.com/en-us/windows/win32/menurc/resource-file-formats
inline constexpr std::array<uint8_t, 86> ICON_TWO_BY_TWO = {
	0,	 0,	  1,   0,	1, 0, 2,   2,	0, 0,	1, 0,	32,	 0, 64, 0, 0, 0, 22,  0, 0,	 0,
	40,	 0,	  0,   0,	2, 0, 0,   0,	4, 0,	0, 0,	1,	 0, 32, 0, 0, 0, 0,	  0, 24, 0,
	0,	 0,	  0,   0,	0, 0, 0,   0,	0, 0,	0, 0,	0,	 0, 0,	0, 0, 0, 255, 0, 0,	 0,
	255, 255, 255, 255, 0, 0, 255, 255, 0, 255, 0, 128, 128, 0, 0,	0, 0, 0, 0,	  0
};
