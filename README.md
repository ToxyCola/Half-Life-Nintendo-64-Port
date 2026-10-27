# Half-Life 64

A Nintendo 64 port of Half-Life's single-player campaign: the original maps, textures and
lighting, played with Valve's own player-movement code, running on stock N64 hardware with an
Expansion Pak.

**Current release: 1.1.** Get `HL64Builder.exe` from the [Releases](../../releases) page; it
builds the ROM from your own copy of Half-Life (see
[Building the ROM on Windows](#building-the-rom-on-windows-the-easy-way)).

It is built from:

* **Valve's Half-Life SDK** (`github.com/ValveSoftware/halflife`). Valve's player physics,
  `pm_shared/pm_shared.c` and `pm_math.c`, is compiled for the N64's VR4300 CPU **unmodified**.
  Walking, running, air control, jumping, ducking, stair-stepping, ladders, swimming, friction
  and footstep materials all come from the real code.
* **Your own copy of Half-Life.** The maps, textures, skies, character models, fonts, HUD
  sprites, titles and sounds are converted from your Steam install by the tools in `tools/`.
  No Valve game data is included in this source.
* **A new N64 engine** written for this port, because the GoldSrc engine (`hw.dll`) has never
  been released as source. It uses [libdragon](https://github.com/DragonMinded/libdragon) and
  [tiny3d](https://github.com/HailToDodongo/tiny3d).

## What works

| Area | Status |
|---|---|
| The whole campaign | All 103 maps, Black Mesa Inbound → the G-Man ending, plus the Hazard Course, played through in order with the PC's level changes. New game (Easy / Medium / Hard or the Training Room), Continue, Load game (autosaves and manual saves) |
| Saving | Battery-backed SRAM, like a cartridge save: an autosave each time you enter a level (from Office Complex on, as the PC starts autosaving after the tram ride) and at the maps' trigger_autosave points, plus manual saves from the pause menu (Save game). A save holds the player, the level's doors, lifts, triggers and characters, global states (Blast Pit's fuel, oxygen and power) and Xen's gravity. Dying restarts from where you entered the level |
| World rendering | BSP + PVS culling, frustum culling, original textures, baked lighting, light styles (switchable and flickering lights), skyboxes, see-through grates (alpha-tested) and glass/water (translucent, drawn after the effects so beams and glows behind them show through correctly) |
| Characters | Scientists, Barney, the G-Man and every monster are placed with their original models, skins, heads and animations (studio models converted by `tools/mdl2n64.py`, skinned on the RSP). They are solid, lit by the floor beneath them, and follow the player through level transitions |
| Navigation | Characters walk on the map's own node graph (`info_node`), like `dlls/nodes.cpp`: `tools/navgen.c` links the nodes offline with the same 18-unit step test the PC does at load time, and the N64 finds routes with A*. Movement is collision-checked against the world, doors, other characters and the player, with step-up, gravity and riding lifts |
| Scripted sequences | `dlls/scripted.cpp`: a scene waits (retrying every second) until its character is free, the character walks or runs to its mark along the node graph, turns, plays its animation and fires its events; the scene's target fires when the animation ends |
| Talking characters | `talkmonster.cpp`: +use makes a scientist or Barney follow you (or wait), pre-disaster staff decline, greetings, idle chatter, questions and answers between characters, remarks about your health, scientists heal you, getting shot makes them angry |
| Speech and music | All of `sentences.txt` (the engine's VOX system: word lists, per-word pitch/volume/start/end, groups), scripted_sentence, speaker announcements, ambient_generic, env_message sounds, the HEV suit voice, and the CD-audio music (trigger_cdaudio / target_cdaudio) from the game's MP3s. 3D panning and distance falloff follow the engine's `S_Spatialize`. Audio is compressed with libdragon's ULC codec |
| HEV suit | Picking up the suit starts the HEV logon (with its music in Anomalous Materials), turns on the HUD, and unlocks the map logic that waits for it. Health, armour, batteries, medkits, wall health and HEV chargers, fall damage, drowning, trigger_hurt, the suit's damage reports, death and restart |
| Weapons | All 14 weapons with their first-person models and animations, ammo, reloads and alternate fire: crowbar, glock, .357, MP5 + grenades, shotgun, crossbow, RPG (laser-guided), gauss (charged shot), egon, hivehand, hand grenades, satchels, trip mines, snarks. Placed where the maps put them, in crates (`func_breakable` spawn objects) and dropped by soldiers. Autoaim as in single player. HUD ammo counters, weapon selection, crosshairs, damage indicators |
| Combat | Monsters see, hear and chase you along the node graph, and use their own attack animations: zombie claws, headcrab leaps, houndeye blasts, vortigaunt zaps, soldiers' MP5s, shotguns and grenades (they take cover when hurt), alien grunt hornets, bullsquid spit, gargantua flames, turrets, barnacles, tentacles. Monstermakers bring in reinforcements. Barney fights alongside you, scientists flee. Damage, pain, deaths. Bodies (killed monsters and the maps' placed corpses) take hits like on PC: bullets draw blood, the crowbar and explosions gib them (`DeadTakeDamage`). Values come from the game's `skill.cfg` (difficulty from the New Game dialog) |
| Bosses | Nihilanth (arenas, teleport balls, crystal recharging, opening head), Gonarch (node path with per-node health, mortar, baby headcrabs), Apache (path, strafing, rockets), Osprey (drops soldiers) |
| Effects | The PC's own sprites: explosions, muzzle flashes (at the weapon model's muzzle), gluon gun beams (xbeam1/xspark1), lightning and laser beams, sparks, blood, gibs, bullsquid/Gonarch spit, alien-controller balls, Nihilanth zaps and teleport balls, and every env_sprite/env_glow in the maps (portals, including the G-Man's at the end, light glows, Xen flares) animated at their framerate. Glows follow GoldSrc's `R_GlowBlend`: hidden by walls in front, constant size on screen, fading with distance. Everything else is depth-tested against the world, so walls and grates hide it |
| Brush entities | Doors (sliding and rotating), buttons, lifts/plats, func_train, rotating objects, walls, water, ladders, breakables (shoot or hit them; they drop their items) |
| Trains | `func_tracktrain` ported from `plats.cpp`/`pathcorner.cpp`: path_track switching (alternate paths, disabled tracks), track-change lifts, dead ends, and **player-driven trains** (On A Rail) with the HUD speed gauge |
| Map logic | trigger_once/multiple/auto/relay/push/teleport/hurt (including the monster-only ones: the vortigaunts that flash into the test chamber during the resonance cascade are sent on as on PC), multi_manager, multisource masters (sources are everything that targets them plus the multi_managers that list them, as in `CMultiSource::Register`; their state is saved), trigger_changetarget, trigger_camera, env_fade, env_message, env_shake, env_explosion, env_spark, game_text, env_render, env_sprite/env_glow, env_shooter, env_funnel, env_blood, env_beverage, env_global (global states carried between maps and saves), trigger_gravity (Xen's 0.6 gravity), trigger_autosave, player_loadsaved, func_friction, func_pendulum, func_mortar_field, momentary_rot_button + momentary_door, func_tank family, killtarget, `globalname` carry-over |
| Text | Half-Life's own font (`gfx.wad` CREDITSFONT) for menus and HUD messages. `titles.txt` is parsed at run time: intro credits, chapter titles, Hazard Course instructions and end credits use their original positions, colours and fade/type-on effects (`cl_dll/message.cpp`) |
| Menus | Modelled on the PC GameUI: logo, Continue / New game / Load game / Options on the title screen; Resume / Save game / Load game / Options / Leave game in the pause menu; the VGUI-style New Game, Load Game (autosaves and saved games, newest first) and Options dialogs, "Loading..." box |
| Level transitions | trigger_changelevel with landmarks, as on PC, carrying over characters that follow you, `changetarget` events (Office Complex's elevator) and global states. The intro tram plays through c0a0–c0a0e. Your health, suit, weapons and ducking carry over. The Options → Debug tab can also jump straight to any map, placing you where the level is normally entered (or at the map's own start point when that spot is over a drop or outside the new map) |
| Player physics | Valve's pm_shared.c, unmodified (including the long jump module) |
| Performance | About 30–60 fps at 320×240 in ares (cycle-accurate emulator), averaging ~47 fps over an automated tour of the campaign's maps; the busiest scenes dip to the high 20s. PVS + frustum culling of world faces and brush entities, characters drawn as triangle strips, fixed poses (gibs, mines, items, bodies) computed once and shared, view-culled gibs and projectiles, billboards drawn as texture rectangles, monster sight checks that test the nearest targets first and skip those outside the PVS, throttled route searches, idle monsters out of sight think at 4 Hz, and a deeper RSP command queue so the CPU and RSP work in parallel |
| HUD | HEV HUD and title credit inset for CRT overscan; the crosshair (on once you have the suit) slides onto the enemy the auto-aim locks on to |

## Known issues

* Tested mainly in the ares emulator. Speed on real hardware should be similar but hasn't been
  measured scene by scene.
* The heaviest scenes (the tram ride and busy parts of Unforeseen Consequences, Office Complex and
  "We've Got Hostiles", with many characters on screen) run in the high 20s fps.
* A few of the largest maps use nearly all of the Expansion Pak's memory.
* A report of the shotgun's view model briefly covering the lower half of the screen while
  moving hasn't been reproduced yet.
* The tentacles in Blast Pit snap between heights instead of playing their rise/lower animations,
  and only the player's footsteps and noises draw them (not other characters walking).
* The Debug map loader's arrival spot is worked out from the previous map's level change; on a
  few maps it falls back to the map's own start point, which can be a little off the usual path.
* With the flashlight on and the view held still, a character walking into the beam can take a
  few frames to be lit.
* Maps do not remember the last thing done in them. When things are broken, NPCs are killed, they
  will be in their original state when backtracking.

## What is not there (yet)

* The monster AI is a compact rewrite of the SDK's schedule system, not a port of it: no squad
  tactics, and the bosses and flying machines are simplified versions of the PC behaviour.
* No decals (bullet holes, blood splats, scorch marks) and no smoke puffs.
* Animated (`+0…`) textures show their first frame.
* Sprites missing from the `sprites` folder you build from are replaced by the nearest one you
  have (for example the G-Man's `E-Tele1` portal by the Xen `exit1` portal); the asset build
  prints which. Building from a full Half-Life install uses the exact art.

## Controls (N64 pad)

| Button | Action |
|---|---|
| Analog stick | Move and strafe |
| C buttons | Look (left/right turn, up/down look) |
| Z | Fire |
| R | Alternate fire (MP5 grenade, double barrel, gauss charge, RPG laser, satchel detonator...) |
| A | Jump |
| L | Duck (hold A while ducking for the long jump) |
| B | Use: doors, buttons, chargers and valves (hold), scientists and Barney (follow / wait). On a train or a mounted gun: take or leave the controls |
| D-pad left / right | Previous / next weapon |
| D-pad down | Reload |
| D-pad up | Flashlight |
| Stick up/down while driving a train | Faster / slower (four steps each way, as on PC) |
| Start | Pause menu |
| Cheats (Options → Controls): L + D-pad up | Noclip |
| Cheats: L + D-pad down | Every weapon and full ammo (impulse 101) |
| Cheats: Options → Debug | God mode, noclip, give everything, and go to any map |

Two other layouts are in **Options → Controls**: stick looks / C buttons move (Turok style),
and stick moves and turns (GoldenEye 1.1). Look sensitivity and "Reverse up-down axis" are
there too. In menus, use the D-pad or stick, A to choose, B to go back, Z/R to change tabs.

**An Expansion Pak (8 MB) is required.** Large maps decompress to about 3 MB. Saves use the
cartridge's 32 KB SRAM (set the save type to SRAM 256k on a flashcart or emulator).

### Running it

* **Emulator:** use an accurate, LLE emulator such as [ares](https://ares-emu.net/) (tested),
  gopher64 or simple64. Older HLE emulators (Project64 with default plugins, mupen64plus with
  HLE plugins) cannot run libdragon ROMs. Enable the Expansion Pak.
* **Real hardware:** copy `hl64.z64` to a flashcart that supports 64 MB ROMs (SummerCart64,
  EverDrive-64 X7/V3, 64drive). The ROM uses libdragon's open-source IPL3 boot code, so no CIC
  setting is needed.

## Building the ROM on Windows (the easy way)

Download **HL64Builder.exe** from this repository's
[Releases](../../releases) page and run it. It needs Half-Life installed through Steam (or any
copy of the game's `valve` folder) and nothing else: the N64 engine (compiled from this
repository for that release), libdragon's asset tools and a private copy of Python are packed
inside it, and no Valve game data is.

1. Run `HL64Builder.exe`. It finds Half-Life through Steam (every Steam library is checked). If it
   can't, drag your `Half-Life` folder onto the window (or onto the .exe) and press Enter.
2. It converts the maps, textures, models, sprites, sounds and music (30–90 minutes, most of it
   compressing the maps), then writes **`hl64.z64` next to the .exe**.
3. Run it again later and it reuses the converted data, so only the ROM is rebuilt;
   `HL64Builder.exe --clean` converts everything again.

The first run unpacks the tools to `%LOCALAPPDATA%\HL64Builder` (about 85 MB) and works in
`%LOCALAPPDATA%\HL64Builder\work` (about 200 MB). Windows SmartScreen may warn that the program
is unrecognised (it isn't code-signed): choose "More info" → "Run anyway". The ROM contains
Half-Life's game data, so keep it for yourself and don't share it.

To make HL64Builder.exe yourself (on Linux, with mingw-w64): `builder/build_windows.sh`
(see the comments at its top). Releases are built by GitHub Actions
(`.github/workflows/release.yml`): changing `builder/VERSION` on `main` builds the .exe and
publishes it as release `v<VERSION>`. "Run workflow" on the Actions tab builds it too, and with
"publish" ticked publishes it as (or replaces it in) the release for the current `builder/VERSION`.
The builder's version includes the commit, so a new exe never reuses files an older one unpacked
or converted in `%LOCALAPPDATA%\HL64Builder`.

## Building from source

You need Linux or WSL, Python 3 with `numpy` and `Pillow`, and your Half-Life install.

```sh
# 1. Toolchain + libraries
#    libdragon "preview" branch (toolchain .deb from its GitHub releases), then:
export N64_INST=/opt/libdragon
git clone -b preview https://github.com/DragonMinded/libdragon
#    bigger RSP command queue: lets the CPU run game logic while the RSP/RDP draws (~+30% fps in big maps)
git -C libdragon apply ../tools/libdragon-rspq-buffer.patch
make -C libdragon install tools-install
git clone https://github.com/HailToDodongo/tiny3d && (cd tiny3d && ./build.sh)

# 2. Convert your game data (maps, node graphs, textures, skies, models, fonts, sounds, music) into filesystem/
python3 tools/build_assets.py "/path/to/Half-Life/valve" filesystem
#    --models-only / --ui-only rebuild just the characters or the fonts, HUD/effect sprites and titles
#    options: --maps c1a0 c1a1 ...   (subset)   --grid 64 --tol 12 (lighting detail vs triangles)

# 3. Build the ROM
make T3D_INST=/path/to/tiny3d
#    -> hl64.z64
```

The asset build takes about 45 minutes, most of it Shrinkler compression of the maps
(`--compress-level 3`, ~20% smaller than aPLib). That, and libdragon's ULC audio codec for the
2,500 sounds and 27 music tracks, is what makes everything fit in one 64 MB cartridge image
(about 63 MB). `--maps-only`, `--models-only`, `--ui-only` and `--sounds-only` rebuild one part.
Building needs a host C compiler (`cc`) for `tools/navgen.c`.

## How the conversion works

* **Textures:** Half-Life textures are 8-bit with a 256-colour palette, which matches the N64's
  CI8 format. They are resampled to power-of-two sizes of at most 2048 texels (the N64's texture
  memory holds 4 KB including the palette) and re-indexed into the *original* palette.
  Duplicates across maps are stored once in `textures.pak`.
* **Lighting:** The N64 has no room for a second lightmap texture, so each face's lightmap is
  baked into vertex colours. Faces are subdivided on their lightmap grid only as finely as the
  light actually varies (`--tol`). A T-junction welding pass, like qbsp's `tjunc.c`, removes
  cracks between faces.
* **Collision:** The BSP clipnodes for all four GoldSrc hulls are kept bit-for-bit. Hull 0 is
  generated from the nodes the way the engine does it. `src/bsp.c` implements the
  recursive hull trace that `pm_shared.c` expects from the engine, including rotated brush
  entities.
* **Visibility:** The original PVS is kept (run-length compressed) and decompressed per leaf.
* **Entities:** The entity lump is kept verbatim and interpreted on the N64 in `src/entities.c`,
  following the SDK sources (`doors.cpp`, `buttons.cpp`, `plats.cpp`, `pathcorner.cpp`,
  `triggers.cpp`, `effects.cpp`, `scripted.cpp`).
* **Characters:** `tools/mdl2n64.py` reads studio models (v10, including `*t.mdl` texture files
  and `*01.mdl` sequence groups) and keeps the sequences the maps use: idles, walks and runs,
  every animation named by a scripted_sequence, dead and sitting poses. Vertices are grouped by
  bone so tiny3d can skin them with one matrix per batch. Textures become CI8.
* **Effect sprites:** `tools/spr2n64.py` converts every sprite the maps' env_sprite, env_glow and
  cycler_sprite entities use (and those the game code uses) into N64 frame strips: single-hue art
  as 8-bit intensity tinted at draw time, coloured art as RGBA32 tiles. Additive art is drawn as
  alpha blending with alpha = brightness, because the RDP's additive blend wraps around instead
  of saturating.
* **Fonts and UI:** `tools/ui2n64.py` converts `gfx.wad`'s CREDITSFONT into three sizes of
  I4 glyph atlas, the 320×240 HUD sprites (`sprites/320hud3.spr`: train gauge and title) and the
  menu logo (`resource/logo.tga`). Colours and layout come from `resource/TrackerScheme.res`
  and `GameMenu.res`.

## Source layout

```
src/main.c        main loop, input, view, trigger_camera, level changes, saves
src/render.c      tiny3d renderer: PVS, batching, CI8 textures, skybox, characters
src/bsp.c         map loader, PVS, hull traces, texture-under-point, light sampling
src/pmove.c       engine callbacks for Valve's pm_shared.c (incl. monster box hulls)
src/entities.c    doors, buttons, trains, tanks, triggers, multi_managers, global states, fades ...
src/studio.c      studio model loading and skinned drawing
src/font.c        Half-Life font drawing
src/hud.c         titles.txt messages, screen fades, game title, train gauge
src/menu.c        title screen, pause menu and dialogs
src/sound.c       channels, 3D sound, sentences.txt speech, music
src/npc.c         monsters, NPCs, items: AI, talking, following, scripted sequences
src/nav.c         node graph routes (A*)
src/player.c      health, HEV suit, damage, items, health/armour HUD
src/weapons.c     weapons, projectiles, view model, ammo HUD, effects and env_sprites
src/skill.c       skill.cfg
valve_sdk/        files from Valve's Half-Life SDK (see valve_sdk/LICENSE)
tools/            bsp2n64.py, mdl2n64.py, ui2n64.py, spr2n64.py, sky2n64.py, spr.py, navgen.c, build_assets.py
builder/          HL64Builder.exe: Windows launcher (hl64builder.c), packer, build_windows.sh
test/             PC test harness: runs the same game code on a converted map
```

The only change to Valve's files is in `pm_defs.h`: `MAX_PHYSENTS` is wrapped in `#ifndef` so the
N64 build can use 128 entries instead of 600 (only nearby brush entities are passed in, as the PC server does), to save RAM.

## Legal

This project contains code from Valve's Half-Life 1 SDK, distributed under Valve's SDK licence
(`valve_sdk/LICENSE`): non-commercial use only, with the licence included. A ROM built with
`tools/build_assets.py` contains converted Half-Life game data from *your* copy of the game.
Keep it for personal use and do not distribute it. Half-Life is a trademark of Valve
Corporation. This is an unofficial fan project, not affiliated with Valve or Nintendo.
