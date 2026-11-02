## Patch notes: 1.3

**New**

* **Head-To-Head:** the PlayStation 2 version's two-player split-screen deathmatch. Player 1 is
  on the top half with controller 1, player 2 on the bottom half with controller 2. It has the
  PS2 version's ten maps (Basement, Debris, Water Canal, Skirmish, Waypoint, Signal, Office,
  Snark Pit, Datacore and Stalkyard) plus **Crimson**, a new map by UnhealthyGrapejuice, and its
  six characters (Gordon Freeman, Security Guard, Gina, Colette, Scientist and Soldier). The rules
  are the PC's deathmatch rules: frag and time limits, respawning with a crowbar and a pistol,
  weapons and items coming back after a while, and a dead player's gun dropped where they fell.
  Each player sees the other holding their gun, and players fall over when killed. It needs the
  PS2 game's files when you build the ROM (see
  [Adding Head-To-Head](https://github.com/ToxyCola/Half-Life-Nintendo-64-Port#adding-head-to-head-from-the-playstation-2-version) in the README).
* **Head-To-Head menus:** a character select screen for both players, then a match screen with
  the map (with a preview of each), the frag limit (5–50) and the time limit (none, or up to 30
  minutes).
* **A new lambda** on the loading screen.
* **The builder** has an optional field for the PS2 files, and now checks the Half-Life folder
  before starting, naming any files that are missing.

**Fixed**

* Characters no longer disappear in large maps, or after loading a save of one.
* A band from the shotgun (and other weapons) no longer covers the lower part of the screen while
  you walk.
* Lasers, electricity, beams and particles no longer show through walls, objects and see-through
  textures. They are hidden by what's in front of them, and seen through glass and grates as they
  should be.
* Electric beams look like lightning again instead of plain blue lines, and the tripmine laser is
  its textured beam again instead of a thin red line.
* Crates can be pushed straight forward, not only from the side.
* The muzzle flash sits just in front of the barrel and the gun hides the part of it behind the
  barrel, like the beams.
* The elevator in Anomalous Materials (c1a0b and c1a0c) only moves when you press its button.
* The scientist at the console in Anomalous Materials no longer stays hunched over the console
  while he talks to you.
* The scientists who fall down the elevator shaft at the start of Unforeseen Consequences now die
  at the bottom and burst into gibs. Damage areas (`trigger_hurt`) hurt characters as well as the
  player, as on PC.
* Going back from some maps to the one before (Questionable Ethics' c2a1b to c2a1a) no longer runs
  out of memory.
* The builder no longer stops with an error when the Half-Life folder has no
  `resource/logo.tga`.
