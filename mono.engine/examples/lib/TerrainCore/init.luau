--!strict

-- Voxel terrain, as pure data.
--
-- `TerrainCore` is to terrain what `MagicCore` is to spells: it computes and it
-- never touches an Instance. `Noise` is coherent noise, `HeightField` is the
-- world's shape, `Edits` is what has been blown out of it, and `Chunker` turns a
-- square of columns into as few boxes as it can. `TerrainRuntime` is the only
-- place any of that becomes a Part.
--
-- The split is not tidiness. Every number in these files' comments -- boxes per
-- chunk, milliseconds per chunk, the measured range of an fBm -- came from
-- running this code outside Roblox, which is only possible because none of it
-- needs a place file.
--
--     local Terrain = require(ReplicatedStorage.TerrainCore)
--
--     local edits = Terrain.Edits.new()
--     local field = Terrain.HeightField.new({ seed = 4 }, edits)
--
--     local chunk = Terrain.Chunker.build(field, 0, 0, 32)
--     print(chunk.count, "boxes")
--
--     edits:crater(12, 9, 14, 8)
--     chunk = Terrain.Chunker.build(field, 0, 0, 32)  -- now with a hole in it

local TerrainCore = {
	Noise = require(script.Noise),
	HeightField = require(script.HeightField),
	Edits = require(script.Edits),
	Chunker = require(script.Chunker),
}

return table.freeze(TerrainCore)
