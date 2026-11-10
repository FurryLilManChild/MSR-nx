# Third-party notices

msr.nro is built with these, each under its own license:

| Part | Used for | License |
| --- | --- | --- |
| [libnx](https://github.com/switchbrew/libnx) (switchbrew) | Switch system calls and services | ISC |
| [deko3d](https://github.com/devkitPro/deko3d) (fincs) | the renderer's GPU interface | zlib |
| [uam](https://github.com/devkitPro/uam) (devkitPro), commit 5a5afc2 | compiling missing shaders on the Switch | zlib; its parts imported from Mesa are MIT |
| devkitA64's newlib and libstdc++ | C and C++ runtime | their own licenses, shipped with devkitPro |

Read as reference while writing our own code (no code copied):

| Project | What it helped with | License |
| --- | --- | --- |
| [fearkov/zakuro](https://github.com/fearkov/zakuro) | 3DS service and GPU behaviour | MIT |
| [gminustwo/msr2ds](https://github.com/gminustwo/msr2ds) | Samus Returns function discovery (its analysis cross-checked ours) | GPL-2.0-or-later |
| [JohnUzoka/TriAevum-Switch](https://github.com/JohnUzoka/TriAevum-Switch) | 3DS to Switch porting lessons, deko3d shader approach | GPL-3.0-or-later |
| [3dbrew](https://www.3dbrew.org/) | 3DS hardware and service documentation | wiki |

The game itself (code and data) is Nintendo's and is not included: every user supplies their own dump.
