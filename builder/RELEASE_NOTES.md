## Patch notes: 1.3.1

**New**

* **Language option in the builder:** speech and on-screen text in the language Half-Life is set
  to in Steam (Steam keeps each language in a `valve_<language>` folder next to `valve`). The
  builder lists the languages your install has, starts on Steam's setting, and remembers your
  choice. The text font now has accented letters (Latin-1). For Japanese, Korean, Chinese, Thai
  and Vietnamese only the speech changes, as the N64 font can't show those scripts. If a
  translation's speech would make the ROM larger than 64 MB, the builder compresses that speech
  harder until it fits (and warns at the end if it still doesn't).
* **Button icons:** the Hazard Course's instructions show the icon of the button each action is
  on, following your button mapping and control scheme ("PRESS (B) TO PUSH A BUTTON" instead of
  "PRESS YOUR USE KEY"). The Button Mapping screen shows the icons too.
* **Flashlight battery on the HUD,** as on PC: the flashlight icon empties as its battery runs
  down (it turns red below 20 %) and refills while it's off.
* **A new lambda** on the loading screen.
* **Hazard icons on the HUD,** as on PC: heat, cold, drowning (O2), radiation, chemicals,
  poison, nerve gas and shock each show their pulsing icon above the health while they hurt you.
* **Drowning, as on PC:** 12 seconds of air under water, then damage that grows every second;
  once you surface, the health drowning took comes back (up to 10 every 2 seconds).
* **The Geiger counter:** radiation clicks faster and louder the closer you are to it.

**Fixed**

* **EverDrive 64 X5/X7:** release ROMs no longer write a debug log. On an EverDrive with no PC
  connected, every log line could stall the game for up to a tenth of a second.
* **Hazard Course:**
  * The light-up progress displays light up, the flashlight room's lights go out, and the
    shooting range's lights work. Lights sharing a name were each toggling the same light style,
    so an even number of them cancelled out.
  * The spinning bridge turns with its wheel and swings back when you let go.
  * Pushed crates fall, so the crate drops into its pit and you can cross on it.
  * The shooting range works: the targets rise and fall, take hits, light up and score, and
    autoaim helps with them as on PC.
  * The guards at the end no longer shoot the switched-off turrets.
* Light switches keep their state when you save and load.
* Momentary wheels and valves are no longer solid (as on PC).

For everything new in 1.3 (Head-To-Head and more), see [What's new in 1.3](https://github.com/ToxyCola/Half-Life-Nintendo-64-Port#whats-new-in-13) in the README.
