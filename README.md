# Scope Zoom Levels (prototype)

Cycles how much a scope's lens magnifies, with one key, while the view around the scope
stays at normal field of view.

All standard scopes that have a magnifying lens (11, all skins, Skin Changer Plus skins
included). The Add-Vantage, Type 2067, Kanetsugu, SO-21 Saika and Kairo SA-1 have no lens
glass the trick can use, so they have no zoom. The list is `tools/scopes.txt`.

## How it works

- The game's scope glass shader (`base\materials\glass_scope.mt`) can enlarge the scene seen
  through the lens. For every scope in `tools/scopes.txt`, `tools/build_scopes.py` finds the
  meshes whose materials use that shader and builds a patch mesh for each, with one lens copy
  per zoom level and, for every skin, one appearance per level that uses them
  (`default_szl1`, `neon1_szl2`, ...). Level 0 is the untouched vanilla appearance.
- ArchiveXL adds those appearances to the vanilla mesh at load time
  (`ScopeZoomLevels.archive.xl`, resource patching), so no vanilla file is replaced.
- Each scope declares its lens parts and zoom levels in TweakDB (`scopes.yaml`, see
  "For mod authors"), keyed by the scope's model name. Every item, variant and skin copy
  of a scope (Skin Changer Plus's included) shares that model name, so they all get the
  zoom levels, whatever order mods load in.
- `ScopeZoomLevels.reds` reads those settings for the fitted scope, finds the lens part on
  the equipped weapon (a fitted scope's meshes are built into the weapon entity) and switches
  it to the matching appearance with Codeware's `ChangeAppearance`. The level is reapplied
  every time you aim, because a re-equipped weapon spawns its scope with the vanilla appearance.
- The same script removes the camera zoom of every weapon when aiming: the weapon's zoom
  reaches the camera through `ZoomTransition.SendZoomAnimFeatureData`, and after vanilla sends
  it the script sends it again with the weapon zoom at 1×. Kiroshi optics zoom is untouched.

## Requirements

Codeware, ArchiveXL, TweakXL. Mod Settings is optional.

## Controls

Mouse button 5 (forward thumb button) cycles through the fitted scope's levels and back to
vanilla. Change it in Mod Settings.

## Settings (Mod Settings)

- **Next lens zoom level**: the key that cycles the levels.
- **No camera zoom when aiming**: on by default; turn off for vanilla camera zoom.
- **Debug logging**: writes what the mod does to the CET game log.

## For mod authors

Your scope can have lens zoom levels too. This mod stays an optional dependency: without it
installed, the lines below are ignored.

1. In your scope's lens mesh, add one appearance per zoom level for every skin, named
   `<skin>_szl1`, `<skin>_szl2`, ... Each uses a copy of your lens material (on
   `base\materials\glass_scope.mt`) with a higher `DistortionStrenght` (the magnification),
   `SphereMaskDistortionRadius` 0 and `AberrationStrenght` 0. `tools/build_mesh.py` here
   can do this for you.
2. Declare it in a TweakXL file, keyed by your scope item's `appearanceResourceName`:

```yaml
ScopeZoomLevels.my_scope_model.lensParts: [ n"my_scope_lens_component" ]
ScopeZoomLevels.my_scope_model.zoomLevels: [ 1.5, 2.5, 4.0 ]   # levels 1, 2, 3
```

`lensParts` names the mesh component(s) in your scope's `.app` that hold the lens.
`zoomLevels` has one entry per `_szl` appearance, in order; leave the lines out for no lens zoom.

## Building

Add a scope's model name and its magnifications to `tools/scopes.txt`, then run
`bash tools/build.sh`. It extracts the vanilla scopes from the installed game, builds the patch
meshes, writes the `.xl` and `scopes.yaml`, packs `archive\pc\mod\ScopeZoomLevels.archive`
and writes `dist\ScopeZoomLevels.zip`. Needs WolvenKit.CLI and Python.

## Credits

Idea inspired by the lens approach in Split Focus OptiX on Nexus Mods. No files from that mod
are used; everything is built from the game's own files.

I built it with Claude (the AI).

## License

MIT
