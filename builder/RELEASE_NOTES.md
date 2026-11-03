## Patch notes: 1.3.2

* **Fixed a crash on the tram ride** ("Wrong compressed size") in ROMs built with the Windows
  builder: compressing a model or map over itself could leave the old file's end behind it. The
  builder now compresses into a separate folder and checks every file before using it.

See the README for [1.3.1](https://github.com/ToxyCola/Half-Life-Nintendo-64-Port#whats-new-in-131) and [1.3](https://github.com/ToxyCola/Half-Life-Nintendo-64-Port#whats-new-in-13).
