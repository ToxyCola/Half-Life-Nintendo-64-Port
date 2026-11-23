**A logic overhaul based on the Dreamcast port**

Version 1.5 goes back over the game's logic and rebuilds it against the source of Half-Life's cancelled Dreamcast port, which is far closer to what an N64 can do than the PC game. Doors, buttons, lifts, trains, triggers, sounds, monsters and every weapon were checked line by line against it and brought into line, within what the N64 and libdragon allow.

**New**

- **Map decals:** the signs, warning stripes, stains, scorch marks and graffiti the maps place (`infodecal`) are drawn, lined up with the surface's texture and lit by it, as the engine lays them. Decals that a map adds later appear when it does, and saves remember them. The builder converts them from `valve/decals.wad`.
- **Room echo:** `env_sound` picks the room acoustics as the player moves around, and being under water muffles everything. Tunnels, vents, chambers and canyons have their echo and reverb.
- **Flashlight:** the beam is the Dreamcast one: an 80-unit light where the view lands, falling off with distance, lighting walls, characters and brush entities. The spot is kept to the surface it hits, so it no longer spills off the edge of a wall.
- **Crossbow scope:** alternate fire zooms in, as on PC.

**Map logic**

- Doors: locked and unlocked sounds and sentences, masters, use-only and touch-only doors, doors that heal or hurt, partner doors that reverse together when blocked, and targets fired at the bottom as well as the top.
- Buttons: lit button textures, sparking buttons, shootable and touch buttons, masters, and the original sound for each button type.
- Lifts, trains and track trains: lift trigger zones, blocking damage, start and stop sounds, volumes, and riders carried properly on everything that turns.
- Triggers, relays and switches follow the game's on, off and toggle rules (`ShouldToggle`), including `trigger_counter`, fire-once relays, kill targets and delayed kill targets.
- Breakables: damage by material, touch and pressure breaking, and the right break sounds.
- Fans and rotating machinery spin up and down, with their sounds, as on PC. `ambient_generic` has all 27 dynamic pitch and volume presets.

**Monsters**

- Monsters judge distance and aim as the original game does: they turn to face you before attacking and use each monster's own attack ranges. Each type has its own field of view.
- Pain, flinching and idle sounds follow each monster's own rules. Soldiers usually run for cover when hit.
- Baby headcrab health and the vortigaunt's zap range match the original.

**Weapons**

- Every weapon was audited against the original:
  - The MP5 and shotgun have their real spreads.
  - The gauss gun drains cells while charging, bites if held too long, reflects off walls at shallow angles and punches through thin ones.
  - Hornets hunt, turn and dive as on PC.
  - RPG rockets accelerate and steer like the originals.
  - The egon has its real damage and ammo use.
  - The crowbar does half damage on quick swings.
  - Grenades are thrown harder the higher you aim.
- **Snarks** are little hunters again: they look for the nearest target, bounce after it, bite and grow a bigger pop with every bite, and can be shot. Like on PC, they don't care who threw them.

**Player**

- Drowning, surfacing, fatal falls, the death sound and the view kick from damage behave as in the original.

**Fixes**

- A shot or beam with an invalid aim could freeze the game; it no longer can.
