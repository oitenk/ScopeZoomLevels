# Changelog

## 1.1.0

NEW! Ping and scan outlines line up with the zoomed lens: inside the scope they're magnified with the picture, so they sit on the enemy you see. Works for every scope shape and zoom level, and for other mods' scopes that use the game's scope glass.

NEW! "Outline only in the lens" in Mod Settings (on by default): pinged enemies show just their outline through the scope instead of the game's hatched fill.

The mod now includes a small RED4ext plugin for the outlines. RED4ext already comes with Codeware, so nothing new to install. On a game version the plugin doesn't support, the mod works as before.

## 1.0.0

NEW! Lens zoom levels for 8 scopes, for every skin: one key cycles the lens magnification. Hyakume: vanilla, 1.3×, 1.6×. Grandstand Mk.2x, E255 Percipient, ClearVue Mk.8 and Jue: 1.6×, 2×, 3×. HPO Kanone MAX Mk.77, E305 Prospecta and Gaki: 2×, 3×, 4×.

NEW! Scopes can start at their first zoom level instead of vanilla. The long and sniper scopes do: aiming starts at 1.6× or 2×, and the key cycles their zoom levels only.

NEW! No camera zoom when aiming, for every weapon, so the view around the sights keeps its normal field of view and only scope lenses magnify. Can be turned off in Mod Settings.

NEW! Other mods can give their own scopes lens zoom levels with two TweakXL lines, and a third to drop the vanilla level (see README, "For mod authors").

Works with Skin Changer Plus: every skin of a scope gets the same zoom levels.

The lens levels are added to the game's scope model with ArchiveXL instead of replacing it.

Each weapon keeps its own zoom level, put back every time you aim with it. Switching weapons no longer carries the zoom level over to the next gun.

NEW! "Remember zoom level per weapon" in Mod Settings (on by default). Turn it off and every aim starts at the scope's first level.

Key binding and debug logging are in Mod Settings (optional).
