// UDIM tile selection for material draws. A slot whose maps are a UDIM set is drawn
// once per tile; Flipbook.w carries that draw's tile number (1001 and up, zero for an
// ordinary draw) and every fragment whose UV lies in another tile is discarded.
// Tile 1001 + u + 10 * v covers UV [u, u + 1) x [v, v + 1).
bool OutsideUdimTile(vec2 uv, float tile) {
	if (tile < 1000.5) return false;
	int index = int(tile + 0.5) - 1001;
	return floor(uv) != vec2(float(index % 10), float(index / 10));
}
