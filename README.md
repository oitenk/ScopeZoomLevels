# Scope Zoom Levels

Cycles how much a scope's lens magnifies, with one key, while the view around the scope
stays at normal field of view.

Scopes with zoom levels (all skins, Skin Changer Plus skins included):

| Scope | Zoom levels |
|---|---|
| Hyakume | vanilla, 1.3×, 1.6× |
| Grandstand Mk.2x, E255 Percipient, ClearVue Mk.8, Jue | 1.6×, 2×, 3× (no vanilla level) |
| HPO Kanone MAX Mk.77, E305 Prospecta, Gaki | 2×, 3×, 4× (no vanilla level) |

The OS-1 GimletEye, CQO Kanone MINI Mk.72 and Handyman stay vanilla. The Add-Vantage,
Type 2067, Kanetsugu, SO-21 Saika and Kairo SA-1 have no lens glass the trick can use, so
they have no zoom. The list is `tools/scopes.txt`.

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

### Ping and scan outlines in the lens

The lens enlarges a copy of the scene, but the game draws ping and scan outlines later, on a
layer of their own that it lays over the screen together with the HUD. Without help, the
outlines inside a zoomed lens stay at their unzoomed size and place. The mod's native part
(`red4ext\plugins\ScopeZoomLevels\ScopeZoomLevels.dll`, a RED4ext plugin) fixes that:

- While you aim at a zoom level, it draws your scope's lens glass a second time into a small
  mask of its own, so it knows exactly where the lens is on screen, for any scope shape, field
  of view or aim animation.
- Right after the game finishes the outline layer, it enlarges the part inside the lens by the
  lens zoom around the lens centre, so outlines sit on the enemies you see through the scope.
  By default only the outline itself is kept inside the lens, without the game's hatched fill.
- Not aiming, it does next to nothing. It only adds its own steps to the frame and puts the
  game's state back exactly afterwards.
- It is written for game version 2.5. On any other version RED4ext doesn't load it, and the
  mod works as before, with vanilla outlines in the lens.

## Requirements

Codeware, ArchiveXL, TweakXL, and RED4ext (which Codeware already needs). Mod Settings is
optional.

## Controls

Mouse button 5 (forward thumb button) cycles through the fitted scope's levels and back to
the first one (vanilla, for scopes that keep it). Scopes without a vanilla level start at
their first zoom level when you aim. Change the key in Mod Settings.

## Settings (Mod Settings)

- **Next lens zoom level**: the key that cycles the levels.
- **No camera zoom when aiming**: on by default; turn off for vanilla camera zoom.
- **Remember zoom level per weapon**: on by default, each weapon keeps the level you last
  chose. Off: every aim starts at the scope's first level.
- **Outline only in the lens**: on by default, pinged enemies show just their outline through
  the scope. Off: the game's filled look (still lined up with the zoom).
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
ScopeZoomLevels.my_scope_model.noVanilla: true                 # optional
```

`lensParts` names the mesh component(s) in your scope's `.app` that hold the lens.
`zoomLevels` has one entry per `_szl` appearance, in order; leave the lines out for no lens zoom.
`noVanilla` drops the vanilla level while aiming: the scope starts at level 1 and the key
cycles only its zoom levels. Leave it out to keep vanilla as the first level.

Ping outlines line up in your scope's lens too, with nothing to add, as long as its lens uses
`base\materials\glass_scope.mt` on the weapon: the native part finds the lens from that glass.

## Building

Add a scope's model name and its magnifications to `tools/scopes.txt` (with `no-vanilla`
before the numbers to drop its vanilla level), then run
`bash tools/build.sh`. It extracts the vanilla scopes from the installed game, builds the patch
meshes, writes the `.xl` and `scopes.yaml`, packs `archive\pc\mod\ScopeZoomLevels.archive`
and writes `dist\ScopeZoomLevels.zip`. Needs WolvenKit.CLI and Python.

The native part is in `native\` (C++, built against [RED4ext.SDK](https://github.com/wopss/RED4ext.SDK)
with Visual Studio's CMake; `tools/build.sh` builds it). Its lens mask shader is compiled ahead
of time to DXIL by `native\tools\make_shaders.py`. `inspector\` holds the diagnostic plugin
used to find out how the game draws a frame; it isn't part of the release.

## Credits

Idea inspired by the lens approach in Split Focus OptiX on Nexus Mods. No files from that mod
are used; everything is built from the game's own files.

The native part is built with RED4ext.SDK by wopss and contributors (MIT).

I built it with Claude (the AI).

## License

MIT
