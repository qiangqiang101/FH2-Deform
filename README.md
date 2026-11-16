# Deform

Cars crumple more, and more realistically, when they crash in **Forza Horizon 2** (Azimuth). Built on the **FH2 Mod Platform** (2.9.0 or newer). It's cosmetic: handling doesn't change.

<img width="1920" height="1080" alt="Forza Horizon 2 2026-11-15 13-54" src="https://github.com/user-attachments/assets/bc569c44-a499-42eb-b405-6e8f28d5bfb3" />
<img width="1920" height="1080" alt="Forza Horizon 2 2026-11-15 15-26" src="https://github.com/user-attachments/assets/90b2c553-ae61-488b-a92e-35f79bcedb46" />

## What it does

- **Deeper dents** (Dent Depth): the game's authored crushed shape is pushed further, past its own limit.
- **Dents where the car was hit** (Dents Where Hit): panels are pushed in where the impact was, not only along the authored shape.
- **Crumple**: ripples in the metal around a hit, with the lighting following them.
- **Scrape Dents**: shallow dents where the car was scraped along a wall.
- **Popping windows** (Glass Breaks At): glass pieces disappear where the damage is over the setting, instead of floating in front of a crushed body. Taillight lenses pop too.
- Compatible with Underglow and the other mods tested (the shaders are handed over on a background thread).

## Settings

MODS > YOUR MODS > FH2Deform, saved in `fh2-mods\FH2Deform.ini`. Changes apply live (the shaders are rewritten in about 10-15 s).

| Setting | Range | Default |
|---|---|---|
| Stronger Deformation | On / Off | On |
| Impact Multiplier | 1.0 - 10.0 | 2.0 (no visible effect found; applies at the next start) |
| Dent Depth | 1.0 - 5.0 | 2.0 |
| Dents Where Hit | 0 - 2 | 1.0 |
| Crumple | 0 - 2 | 1.0 |
| Scrape Dents | 0 - 2 | 1.0 |
| Glass Breaks At | 0.05 - 1.0 | 0.30 (1.0 keeps all glass) |

## Console commands (for testing)

`fh2mod FH2Deform hit front|back|left|right [strength]`, `push forward|back|right|left [m/s]`, `pose`, `hitraw`.

## Known limits

- Bumpers still don't come off the player's car (the AI cars' do): the game never sends its drop event for the player's car.
- The car's black inner shell doesn't deform: patching the shader variants that draw it turns damaged areas black, so only the `Morphing` variants are patched (glass: all variants).
- On some cars (BMW M5 '12) a dark pane with defroster lines stays behind a popped rear window; no car shader I could replace draws it.
- Windows crack and pop by the damage at their own spot: a sunroof far from the impact keeps its glass.

## How FH2 deforms a car

Found in the car vertex shaders (`media\_library\Shaders.zip`, the `.durango.vso` files the game loads):

- A **damage cage** (`DamageCageTexture`): a 3D texture around the car's bounding box holding how damaged each spot is.
- An authored **crushed shape** (`g_MorphDataBuffer`): where each vertex goes when its spot is fully damaged.
- Each vertex moves by its crushed offset times the cage's damage there, clamped to 1.

The mod rewrites those DXBC shaders (and recomputes their checksum) and hands them to the game through the platform. The game's files aren't changed. See `NOTES.md` for the full findings.

## Building

Visual Studio 2022 and CMake 3.20+:

```powershell
git submodule update --init
cmake -B build -A x64
cmake --build build --config Release
```

Copy `build\Release\FH2Deform.dll` into the Azimuth folder's `fh2-mods`.
