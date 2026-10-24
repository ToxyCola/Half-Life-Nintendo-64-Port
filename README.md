# Half-Life 64

A Nintendo 64 port of Half-Life's single-player campaign: the original maps, textures and
lighting, played with Valve's own player-movement code, running on stock N64 hardware with an
Expansion Pak.

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
| All 103 campaign maps (Black Mesa Inbound → the G-Man ending, plus the Hazard Course) | New game, Training Room, and chapter select (Load game) |
| World rendering | BSP + PVS culling, frustum culling, original textures, baked lighting, skyboxes |
| Characters | Scientists, Barney, the G-Man and every monster are placed with their original models, skins, heads and animations (studio models converted by `tools/mdl2n64.py`, skinned on the RSP). They are solid, lit by the floor beneath them, and follow the player through level transitions |
| Navigation | Characters walk on the map's own node graph (`info_node`), like `dlls/nodes.cpp`: `tools/navgen.c` links the nodes offline with the same 18-unit step test the PC does at load time, and the N64 finds routes with A*. Movement is collision-checked against the world, doors, other characters and the player, with step-up, gravity and riding lifts |
| Scripted sequences | `dlls/scripted.cpp`: a scene waits (retrying every second) until its character is free, the character walks or runs to its mark along the node graph, turns, plays its animation and fires its events; the scene's target fires when the animation ends |
| Talking characters | `talkmonster.cpp`: +use makes a scientist or Barney follow you (or wait), pre-disaster staff decline, greetings, idle chatter, questions and answers between characters, remarks about your health, scientists heal you, getting shot makes them angry |
| Speech and music | All of `sentences.txt` (the engine's VOX system: word lists, per-word pitch/volume/start/end, groups), scripted_sentence, speaker announcements, ambient_generic, env_message sounds, the HEV suit voice, and the CD-audio music (trigger_cdaudio / target_cdaudio) from the game's MP3s. 3D panning and distance falloff follow the engine's `S_Spatialize`. Audio is compressed with libdragon's ULC codec |
| HEV suit | Picking up the suit starts the HEV logon (with its music in Anomalous Materials), turns on the HUD, and unlocks the map logic that waits for it. Health, armour, batteries, medkits, wall health and HEV chargers, fall damage, drowning, trigger_hurt, the suit's damage reports, death and restart |
| Weapons | All 14 weapons with their first-person models and animations, ammo, reloads and alternate fire: crowbar, glock, .357, MP5 + grenades, shotgun, crossbow, RPG (laser-guided), gauss (charged shot), egon, hivehand, hand grenades, satchels, trip mines, snarks. Placed where the maps put them, in crates (`func_breakable` spawn objects) and dropped by soldiers. Autoaim as in single player. HUD ammo counters, weapon selection, crosshairs, damage indicators |
| Combat | Monsters see, hear and chase you along the node graph, and use their own attack animations: zombie claws, headcrab leaps, houndeye blasts, vortigaunt zaps, soldiers' MP5s, shotguns and grenades (they take cover when hurt), alien grunt hornets, bullsquid spit, gargantua flames, turrets, barnacles, tentacles. Monstermakers bring in reinforcements. Barney fights alongside you, scientists flee. Damage, pain, deaths. Values come from the game's `skill.cfg` (difficulty from the New Game dialog) |
| Bosses | Nihilanth (arenas, teleport balls, crystal recharging, opening head), Gonarch (node path with per-node health, mortar, baby headcrabs), Apache (path, strafing, rockets), Osprey (drops soldiers) |
| Brush entities | Doors (sliding and rotating), buttons, lifts/plats, func_train, rotating objects, walls, water, ladders, breakables (shoot or hit them; they drop their items) |
| Trains | `func_tracktrain` ported from `plats.cpp`/`pathcorner.cpp`: path_track switching (alternate paths, disabled tracks), track-change lifts, dead ends, and **player-driven trains** (On A Rail) with the HUD speed gauge |
| Map logic | trigger_once/multiple/auto/relay/push/teleport/hurt, multi_manager, multisource masters, trigger_changetarget, trigger_camera, env_fade, env_message, env_shake, env_explosion, env_spark, game_text, env_render, killtarget, `globalname` carry-over |
| Text | Half-Life's own font (`gfx.wad` CREDITSFONT) for menus and HUD messages. `titles.txt` is parsed at run time: intro credits, chapter titles, Hazard Course instructions and end credits use their original positions, colours and fade/type-on effects (`cl_dll/message.cpp`) |
| Menus | Modelled on the PC GameUI: logo, New game / Load game / Options, the VGUI-style New Game, Load Game and Options dialogs, "Loading..." box, pause menu over the game |
| Level transitions | trigger_changelevel with landmarks, as on PC. The intro tram plays through c0a0–c0a0e. Your health, suit and weapons carry over |
| Player physics | Valve's pm_shared.c, unmodified (including the long jump module) |
| Performance | Roughly 20–60 fps at 320×240 in ares (cycle-accurate emulator), depending on the view and how many characters are visible. PVS + frustum culling of world faces and brush entities, idle monsters out of sight think at 4 Hz |

## What is not there (yet)

* The monster AI is a compact rewrite of the SDK's schedule system, not a port of it: no squad
  tactics, and the bosses and flying machines are simplified versions of the PC behaviour.
* No decals or smoke; effects use a handful of the game's sprites (explosions, muzzle flashes,
  beams, sparks, blood, spit and energy balls). Animated (`+0…`) textures show their first frame.
* No save games. A death restarts the current map with what you had when you entered it.
  Use Load game to start any chapter.

## Controls (N64 pad)
## Controls (N64 pad)

| Button | Action |
|---|---|
| Analog stick | Move and strafe |
| C buttons | Look (left/right turn, up/down look) |
| Z | Fire |
| R | Alternate fire (MP5 grenade, double barrel, gauss charge, RPG laser, satchel detonator...) |
| A | Jump |
| L | Duck (hold A while ducking for the long jump) |
| B | Use: doors, buttons, chargers (hold), scientists and Barney (follow / wait). On a train: take or leave the controls |
| D-pad left / right | Previous / next weapon |
| D-pad down | Reload |
| D-pad up | Flashlight |
| Stick up/down while driving a train | Faster / slower (four steps each way, as on PC) |
| Start | Pause menu |
| Cheats (Options → Controls): L + D-pad up | Noclip |
| Cheats: L + D-pad down | Every weapon and full ammo (impulse 101) |

Two other layouts are in **Options → Controls**: stick looks / C buttons move (Turok style),
and stick moves and turns (GoldenEye 1.1). Look sensitivity and "Reverse up-down axis" are
there too. In menus, use the D-pad or stick, A to choose, B to go back, Z/R to change tabs.

**An Expansion Pak (8 MB) is required.** Large maps decompress to about 3 MB.

### Running it

* **Emulator:** use an accurate, LLE emulator such as [ares](https://ares-emu.net/) (tested),
  gopher64 or simple64. Older HLE emulators (Project64 with default plugins, mupen64plus with
  HLE plugins) cannot run libdragon ROMs. Enable the Expansion Pak.
* **Real hardware:** copy `hl64.z64` to a flashcart that supports 64 MB ROMs (SummerCart64,
  EverDrive-64 X7/V3, 64drive). The ROM uses libdragon's open-source IPL3 boot code, so no CIC
  setting is needed.

## Building

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
#    --models-only / --ui-only rebuild just the characters or the fonts, sprites and titles
#    options: --maps c1a0 c1a1 ...   (subset)   --grid 64 --tol 12 (lighting detail vs triangles)

# 3. Build the ROM
make T3D_INST=/path/to/tiny3d
#    -> hl64.z64
```

The asset build takes about 45 minutes, most of it Shrinkler compression of the maps
(`--compress-level 3`, ~20% smaller than aPLib). That, and libdragon's ULC audio codec for the
2,500 sounds and 27 music tracks, is what makes everything fit in one 64 MB cartridge image
(about 62 MB). `--maps-only`, `--models-only`, `--ui-only` and `--sounds-only` rebuild one part.
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
* **Fonts and UI:** `tools/ui2n64.py` converts `gfx.wad`'s CREDITSFONT into three sizes of
  I4 glyph atlas, the 320×240 HUD sprites (`sprites/320hud3.spr`: train gauge and title) and the
  menu logo (`resource/logo.tga`). Colours and layout come from `resource/TrackerScheme.res`
  and `GameMenu.res`.

## Source layout

```
src/main.c        main loop, input, view, trigger_camera, level changes
src/render.c      tiny3d renderer: PVS, batching, CI8 textures, skybox, characters
src/bsp.c         map loader, PVS, hull traces, texture-under-point, light sampling
src/pmove.c       engine callbacks for Valve's pm_shared.c (incl. monster box hulls)
src/entities.c    doors, buttons, trains, triggers, multi_managers, fades, messages ...
src/studio.c      studio model loading and skinned drawing
src/font.c        Half-Life font drawing
src/hud.c         titles.txt messages, screen fades, game title, train gauge
src/menu.c        title screen, pause menu and dialogs
src/sound.c       channels, 3D sound, sentences.txt speech, music
src/npc.c         monsters, NPCs, items: AI, talking, following, scripted sequences
src/nav.c         node graph routes (A*)
src/player.c      health, HEV suit, damage, items, health/armour HUD
src/weapons.c     weapons, projectiles, view model, ammo HUD
src/skill.c       skill.cfg
valve_sdk/        files from Valve's Half-Life SDK (see valve_sdk/LICENSE)
tools/            bsp2n64.py, mdl2n64.py, ui2n64.py, sky2n64.py, spr.py, navgen.c, build_assets.py
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
