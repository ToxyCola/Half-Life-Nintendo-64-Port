**New: the builder's Experimental tab**

- **Map loader:** add a folder of custom .bsp maps. They're listed under a new **Custom maps** item on the main menu, and in Head-To-Head's map choice (R / Z switches between the official and the custom maps).
- **Mods:** build a Half-Life mod (pick its folder, the one with liblist.gam) instead of Half-Life. The ROM plays the mod's maps from its start map, with its own models, sounds and textures over Half-Life's.
- **Expansions:** build **Blue Shift** or **Opposing Force** from your Steam install (they sit next to `valve` in the Half-Life folder).
- Only one of these at a time: a mod, Blue Shift or Opposing Force. Choosing one leaves out Head-To-Head, the map loader and the language choice.
- Mods and the expansions get the **PC version's menu** instead of the PS2-style one, using the game's own menu background and title where it has them (an older mod's splash screen counts), with PC-style New Game, Load/Save and Leave dialogs.

What works in the expansions so far:

- **Blue Shift** should be playable from start to end. Its armour vest, helmet and Rosenberg are in, its HUD comes with no HEV voice, and its maps are read in Blue Shift's own format.
- **Opposing Force** loads and plays, but **can't be finished yet**. Its new weapons are swapped for the nearest Half-Life ones (the Desert Eagle is the .357, the M249 is the MP5, the sniper rifle is the crossbow ...). Its new people and monsters use their own models and voices with the nearest Half-Life behaviour. There's no barnacle grapple, rope climbing, Pit Worm or Gene Worm yet.

**Fixes**

- The Geiger counter's clicks (and the hazard icons) no longer carry on after you leave the radiation, load another map or start a new game, and they no longer come through walls.
- Scientists no longer get stuck running in place: one left wedged against furniture by a scripted pose (the one behind c1a0's front desk when the buzzer is pressed) now steps free and runs off, a scripted run no longer waits for ever on the player standing nearby, and a fleeing scientist picks another place to run when the way to the first is blocked.
- Some older custom maps whose lighting data was cut short now convert.
- Without the Expansion Pak, the ROM now says that it needs one instead of running out of memory.
