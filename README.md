# OCR Texture Tools

Editor-only Unreal plugin for organizing imported texture sets and building material instances from naming rules.

## What it does

- Listens to texture import and reimport events.
- Detects texture sets by suffix:
  - `_BaseColor`
  - `_Normal`
  - `_OcclusionRoughnessMetallic`
- Normalizes ORM textures:
  - `sRGB = false`
  - `CompressionSettings = TC_Masks`
- Moves matched textures into a managed content folder based on the source disk folder name.
- Creates or updates one material instance per complete texture group.

## Setup

Open Unreal Editor and configure the plugin in:

`Edit > Project Settings > Plugins > OCR Texture Tools`

Recommended first-time setup:

1. Enable `Automatic Processing`.
2. Set `Watched Source Root` to your source texture export root.
3. Set `Target Root Content Path` to the Unreal folder that should receive managed textures.
4. Set `Parent Material Path` to your master material.
5. Set texture parameter names to match your master material.

## Notes

- The plugin is intentionally shipped with neutral defaults.
- Automatic processing is disabled by default.
- The plugin only acts on `UTexture2D` assets whose names end with the configured suffixes.
