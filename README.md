# OCR Texture Tools

Editor-only Unreal plugin for organizing imported texture sets and building material instances from naming rules.

## What it does

- Listens to texture import and reimport events.
- Listens to static mesh import and reimport events.
- Detects texture sets by suffix:
  - `_BaseColor`
  - `_Normal`
  - `_OcclusionRoughnessMetallic`
- Normalizes ORM textures:
  - `sRGB = false`
  - `CompressionSettings = TC_Masks`
- Moves matched textures into a managed content folder based on the source disk folder name.
- Creates or updates one material instance per complete texture group.
- Optionally moves imported static meshes into a managed content folder.
- Replaces static mesh material slots with managed `MI_` assets from the matching source folder.
- Archives imported fallback materials into `_ImportFallback/<SourceFolderName>`.
- Routes duplicate static mesh imports into `_DuplicateImports` instead of overwriting the managed mesh.

## Setup

Open Unreal Editor and configure the plugin in:

`Edit > Project Settings > Plugins > OCR Texture Tools`

Recommended first-time setup:

1. Enable `Automatic Processing`.
2. Set `Watched Source Root` to your source texture export root.
3. Set `Target Root Content Path` to the Unreal folder that should receive managed textures.
4. Optionally set `Static Mesh Target Root Content Path` if imported meshes should be moved automatically.
5. Set `Parent Material Path` to your master material.
6. Set texture parameter names to match your master material.

## Texture Workflow

For managed texture imports, the plugin:

1. Reads the parent folder name from the source file path.
2. Moves matched textures into:
   - `<Target Root Content Path>/<SourceFolderName>`
3. Applies ORM normalization:
   - `sRGB = false`
   - `CompressionSettings = TC_Masks`
4. Waits until a full `BaseColor + Normal + ORM` group is present.
5. Creates or updates one material instance for that complete group.

## Static Mesh Workflow

When `Static Mesh Target Root Content Path` is set, the plugin also automates imported `UStaticMesh` assets:

1. Reads the source file path from the imported mesh.
2. Uses the source file base name as the managed mesh asset name.
3. Moves the mesh into:
   - `<Static Mesh Target Root Content Path>/<MeshAssetName>`
4. Resolves the managed material folder from the mesh source folder:
   - `<Target Root Content Path>/<SourceFolderName>`
5. Replaces mesh material slots with managed `MI_` assets from that folder.
   Matching prefers:
   - `ImportedMaterialSlotName`
   - `MaterialSlotName`
   - current assigned material name
6. Archives imported fallback materials into:
   - `<Static Mesh Target Root Content Path>/_ImportFallback/<SourceFolderName>`
7. If the managed mesh path already exists, routes the new mesh into:
   - `<Static Mesh Target Root Content Path>/_DuplicateImports`

## Naming Expectations

- Texture naming must follow the configured suffix rules.
- Material slot names should be stable semantic names from the DCC source.
- Avoid trailing numeric slot names such as `Name_1` and `Name_2` if you rely on automatic static mesh material replacement.
- The best results come from keeping DCC material names, texture set names, and managed MI names aligned.

## Notes

- The plugin is intentionally shipped with neutral defaults.
- Automatic processing is disabled by default.
- The plugin only acts on `UTexture2D` assets whose names end with the configured suffixes.
- Project-specific paths and parameter names should be configured per project, not committed as plugin defaults.
