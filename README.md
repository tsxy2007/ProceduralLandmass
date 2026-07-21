# ProceduralLandmass

Procedural terrain generation plugin based on fractal Perlin noise (fBm).

## AProceduralLandmassActor

Placeable terrain actor. Drag it into a level and it auto-generates a terrain mesh.

### Usage

1. Drag `ProceduralLandmassActor` into the level.
2. Tune noise parameters in the Details panel (Seed, Scale, Octaves, etc.).
3. Click **Generate Terrain** to regenerate.
4. Optionally assign a `HeightCurve` to reshape the terrain profile.
5. Assign a `Material` for appearance. If `TerrainTypes` array is populated, a color texture is created and passed to the material as `TerrainColorTexture`.

### Noise Parameters

| Param        | Default | Description                              |
|-------------|---------|------------------------------------------|
| ChunkSize   | 128     | Vertices per side (NxN grid)            |
| Scale       | 50      | Sampling scale — larger = smoother       |
| Octaves     | 6       | fBm detail layers                        |
| Persistence | 0.5     | Amplitude decay per octave               |
| Lacunarity  | 2.0     | Frequency multiplier per octave          |
| Seed        | 0       | Random seed                              |
| Offset      | (0,0)   | Global sampling offset                   |

### Blueprint API

- `GenerateTerrain()` — regenerate the terrain mesh from current parameters.
