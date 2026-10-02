#pragma once
#include <array>
#include <cstddef>
namespace engine::imagegraph::detail {
	// The source shader's ordered assignments allow later corner cases to replace
	// edges.
	inline int SourceTerrainIndex(int type, const std::array<int, 9> &neighbors) {
		const int i0 = neighbors[0], i1 = neighbors[1], i2 = neighbors[2], i3 = neighbors[3];
		const int i5 = neighbors[5], i6 = neighbors[6], i7 = neighbors[7], i8 = neighbors[8];
		constexpr std::array<int, 55> mapping{8,  10, 11, 0,  1,  6,  5,  3,  2,  34, 22, 20, 33, 35,
											  12, 28, 30, 29, 31, 46, 21, 22, 44, 45, 47, 24, 16, 18,
											  17, 19, 9,  43, 40, 37, 38, 39, 36, 25, 42, 41, 27, 26,
											  7,  4,  22, 22, 22, 22, 13, 23, 32, 15, 14, 22, 22};
		int index = 0;
		if (type == 0) {
			index = 4;

			if ((i1 == 0) && (i3 == 0) && (i5 == 1) && (i7 == 1)) index = 0;
			if ((i1 == 0) && (i3 == 1) && (i5 == 0) && (i7 == 1)) index = 2;
			if ((i1 == 1) && (i3 == 0) && (i5 == 1) && (i7 == 0)) index = 6;
			if ((i1 == 1) && (i3 == 1) && (i5 == 0) && (i7 == 0)) index = 8;

			if ((i1 == 0) && (i3 == 1) && (i5 == 1) && (i7 == 1)) index = 1;
			if ((i1 == 1) && (i3 == 0) && (i5 == 1) && (i7 == 1)) index = 3;
			if ((i1 == 1) && (i3 == 1) && (i5 == 0) && (i7 == 1)) index = 5;
			if ((i1 == 1) && (i3 == 1) && (i5 == 1) && (i7 == 0)) index = 7;

		} else if (type == 1) {
			index = 12;

			if (true && (i1 == 0) && true && (i3 == 1) && (i5 == 1) && true && (i7 == 1) && true) index = 2;

			if (true && (i1 == 1) && true && (i3 == 0) && (i5 == 1) && true && (i7 == 1) && true) index = 10;

			if (true && (i1 == 1) && true && (i3 == 1) && (i5 == 0) && true && (i7 == 1) && true) index = 14;

			if (true && (i1 == 1) && true && (i3 == 1) && (i5 == 1) && true && (i7 == 0) && true) index = 22;

			if (true && (i1 == 0) && true && (i3 == 0) && (i5 == 1) && true && (i7 == 1) && true) index = 1;

			if (true && (i1 == 0) && true && (i3 == 1) && (i5 == 0) && true && (i7 == 1) && true) index = 3;

			if (true && (i1 == 1) && true && (i3 == 0) && (i5 == 1) && true && (i7 == 0) && true) index = 21;

			if (true && (i1 == 1) && true && (i3 == 1) && (i5 == 0) && true && (i7 == 0) && true) index = 23;

			if ((i0 == 0) && (i1 == 1) && true && (i3 == 1) && (i5 == 1) && true && (i7 == 1) && true)
				index = 6;

			if (true && (i1 == 1) && (i2 == 0) && (i3 == 1) && (i5 == 1) && true && (i7 == 1) && true)
				index = 8;

			if (true && (i1 == 1) && true && (i3 == 1) && (i5 == 1) && (i6 == 0) && (i7 == 1) && true)
				index = 16;

			if (true && (i1 == 1) && true && (i3 == 1) && (i5 == 1) && true && (i7 == 1) && (i8 == 0))
				index = 18;

		} else if (type == 2) {
			index = 6;

			if ((i1 == 0) && (i3 == 0) && (i5 == 1) && (i7 == 1)) index = 0;
			if ((i1 == 0) && (i5 == 0) && (i3 == 1) && (i7 == 1)) index = 2;
			if ((i3 == 0) && (i7 == 0) && (i1 == 1) && (i5 == 1)) index = 10;
			if ((i5 == 0) && (i7 == 0) && (i1 == 1) && (i3 == 1)) index = 12;

			if ((i1 == 0) && (i3 == 1) && (i5 == 1) && (i7 == 1)) index = 1;
			if ((i3 == 0) && (i1 == 1) && (i5 == 1) && (i7 == 1)) index = 5;
			if ((i5 == 0) && (i3 == 1) && (i1 == 1) && (i7 == 1)) index = 7;
			if ((i7 == 0) && (i3 == 1) && (i5 == 1) && (i1 == 1)) index = 11;

			if ((i0 == 1) && (i1 == 1) && (i3 == 1) && (i5 == 1) && (i7 == 1) && (i8 == 1)) {
				if ((i2 == 1) && (i6 == 0)) index = 4;
				if ((i2 == 0) && (i6 == 1)) index = 8;
				if ((i2 == 0) && (i6 == 0)) index = 13;
			}

			if ((i1 == 1) && (i2 == 1) && (i3 == 1) && (i5 == 1) && (i6 == 1) && (i7 == 1)) {
				if ((i0 == 1) && (i8 == 0)) index = 3;
				if ((i0 == 0) && (i8 == 1)) index = 9;
				if ((i0 == 0) && (i8 == 0)) index = 14;
			}

		} else if (type == 3 || type == 4) {
			index = 12;

			if (true && (i1 == 0) && true && (i3 == 0) && (i5 == 1) && true && (i7 == 1) && (i8 == 1))
				index = 0;

			if (true && (i1 == 0) && true && (i3 == 1) && (i5 == 0) && (i6 == 1) && (i7 == 1) && true)
				index = 2;

			if (true && (i1 == 1) && (i2 == 1) && (i3 == 0) && (i5 == 1) && true && (i7 == 0) && true)
				index = 22;

			if ((i0 == 1) && (i1 == 1) && true && (i3 == 1) && (i5 == 0) && true && (i7 == 0) && true)
				index = 24;

			if (true && (i1 == 0) && true && (i3 == 1) && (i5 == 1) && (i6 == 1) && (i7 == 1) && (i8 == 1))
				index = 1;

			if (true && (i1 == 1) && (i2 == 1) && (i3 == 0) && (i5 == 1) && true && (i7 == 1) && (i8 == 1))
				index = 11;

			if ((i0 == 1) && (i1 == 1) && true && (i3 == 1) && (i5 == 0) && (i6 == 1) && (i7 == 1) && true)
				index = 13;

			if ((i0 == 1) && (i1 == 1) && (i2 == 1) && (i3 == 1) && (i5 == 1) && true && (i7 == 0) && true)
				index = 23;

			if (true && (i1 == 0) && true && (i3 == 0) && (i5 == 0) && true && (i7 == 1) && true) index = 3;

			if (true && (i1 == 1) && true && (i3 == 0) && (i5 == 0) && true && (i7 == 1) && true) index = 14;

			if (true && (i1 == 1) && true && (i3 == 0) && (i5 == 0) && true && (i7 == 0) && true) index = 25;

			if (true && (i1 == 0) && true && (i3 == 0) && (i5 == 1) && true && (i7 == 0) && true) index = 33;

			if (true && (i1 == 0) && true && (i3 == 1) && (i5 == 1) && true && (i7 == 0) && true) index = 34;

			if (true && (i1 == 0) && true && (i3 == 1) && (i5 == 0) && true && (i7 == 0) && true) index = 35;

			if (true && (i1 == 0) && true && (i3 == 0) && (i5 == 0) && true && (i7 == 0) && true) index = 36;

			if (true && (i1 == 0) && true && (i3 == 0) && (i5 == 1) && true && (i7 == 1) && (i8 == 0))
				index = 4;

			if (true && (i1 == 0) && true && (i3 == 1) && (i5 == 0) && (i6 == 0) && (i7 == 1) && true)
				index = 7;

			if (true && (i1 == 1) && (i2 == 0) && (i3 == 0) && (i5 == 1) && true && (i7 == 0) && true)
				index = 37;

			if ((i0 == 0) && (i1 == 1) && true && (i3 == 1) && (i5 == 0) && true && (i7 == 0) && true)
				index = 40;

			if (true && (i1 == 0) && true && (i3 == 1) && (i5 == 1) && (i6 == 1) && (i7 == 1) && (i8 == 0))
				index = 5;

			if (true && (i1 == 0) && true && (i3 == 1) && (i5 == 1) && (i6 == 0) && (i7 == 1) && (i8 == 1))
				index = 6;

			if (true && (i1 == 1) && (i2 == 1) && (i3 == 0) && (i5 == 1) && true && (i7 == 1) && (i8 == 0))
				index = 15;

			if (true && (i1 == 1) && (i2 == 0) && (i3 == 0) && (i5 == 1) && true && (i7 == 1) && (i8 == 1))
				index = 26;

			if ((i0 == 1) && (i1 == 1) && true && (i3 == 1) && (i5 == 0) && (i6 == 0) && (i7 == 1) && true)
				index = 18;

			if ((i0 == 0) && (i1 == 1) && true && (i3 == 1) && (i5 == 0) && (i6 == 1) && (i7 == 1) && true)
				index = 29;

			if ((i0 == 1) && (i1 == 1) && (i2 == 0) && (i3 == 1) && (i5 == 1) && true && (i7 == 0) && true)
				index = 38;

			if ((i0 == 0) && (i1 == 1) && (i2 == 1) && (i3 == 1) && (i5 == 1) && true && (i7 == 0) && true)
				index = 39;

			if ((i0 == 1) && (i1 == 1) && (i2 == 1) && (i3 == 1) && (i5 == 1) && (i6 == 1) && (i7 == 1) &&
				(i8 == 0))
				index = 16;

			if ((i0 == 1) && (i1 == 1) && (i2 == 1) && (i3 == 1) && (i5 == 1) && (i6 == 0) && (i7 == 1) &&
				(i8 == 1))
				index = 17;

			if ((i0 == 1) && (i1 == 1) && (i2 == 0) && (i3 == 1) && (i5 == 1) && (i6 == 1) && (i7 == 1) &&
				(i8 == 1))
				index = 27;

			if ((i0 == 0) && (i1 == 1) && (i2 == 1) && (i3 == 1) && (i5 == 1) && (i6 == 1) && (i7 == 1) &&
				(i8 == 1))
				index = 28;

			if (true && (i1 == 1) && (i2 == 0) && (i3 == 0) && (i5 == 1) && true && (i7 == 1) && (i8 == 0))
				index = 48;

			if ((i0 == 1) && (i1 == 1) && (i2 == 0) && (i3 == 1) && (i5 == 1) && (i6 == 1) && (i7 == 1) &&
				(i8 == 0))
				index = 49;

			if ((i0 == 0) && (i1 == 1) && (i2 == 1) && (i3 == 1) && (i5 == 1) && (i6 == 0) && (i7 == 1) &&
				(i8 == 1))
				index = 50;

			if ((i0 == 0) && (i1 == 1) && true && (i3 == 1) && (i5 == 0) && (i6 == 0) && (i7 == 1) && true)
				index = 51;

			if (true && (i1 == 0) && true && (i3 == 1) && (i5 == 1) && (i6 == 0) && (i7 == 1) && (i8 == 0))
				index = 8;

			if ((i0 == 1) && (i1 == 1) && (i2 == 1) && (i3 == 1) && (i5 == 1) && (i6 == 0) && (i7 == 1) &&
				(i8 == 0))
				index = 19;

			if ((i0 == 0) && (i1 == 1) && (i2 == 0) && (i3 == 1) && (i5 == 1) && (i6 == 1) && (i7 == 1) &&
				(i8 == 1))
				index = 30;

			if ((i0 == 0) && (i1 == 1) && (i2 == 0) && (i3 == 1) && (i5 == 1) && true && (i7 == 0) && true)
				index = 41;

			if ((i0 == 1) && (i1 == 1) && (i2 == 0) && (i3 == 1) && (i5 == 1) && (i6 == 0) && (i7 == 1) &&
				(i8 == 1))
				index = 9;

			if ((i0 == 0) && (i1 == 1) && (i2 == 1) && (i3 == 1) && (i5 == 1) && (i6 == 1) && (i7 == 1) &&
				(i8 == 0))
				index = 20;

			if ((i0 == 0) && (i1 == 1) && (i2 == 0) && (i3 == 1) && (i5 == 1) && (i6 == 0) && (i7 == 1) &&
				(i8 == 1))
				index = 31;

			if ((i0 == 0) && (i1 == 1) && (i2 == 0) && (i3 == 1) && (i5 == 1) && (i6 == 1) && (i7 == 1) &&
				(i8 == 0))
				index = 32;

			if ((i0 == 0) && (i1 == 1) && (i2 == 1) && (i3 == 1) && (i5 == 1) && (i6 == 0) && (i7 == 1) &&
				(i8 == 0))
				index = 42;

			if ((i0 == 1) && (i1 == 1) && (i2 == 0) && (i3 == 1) && (i5 == 1) && (i6 == 0) && (i7 == 1) &&
				(i8 == 0))
				index = 43;

			if ((i0 == 0) && (i1 == 1) && (i2 == 0) && (i3 == 1) && (i5 == 1) && (i6 == 0) && (i7 == 1) &&
				(i8 == 0))
				index = 52;

			if (type == 3) index = mapping[size_t(index)];
		}

		return index;
	}
} // namespace engine::imagegraph::detail
