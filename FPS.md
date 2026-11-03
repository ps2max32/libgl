# Frame rates of the Quakes on the PS2

Timedemos on a PAL PS2 (real hardware), 2026-11-03. The games were built with
maxsdk `bfef886` and libgl `2212e9a`, as VM programs (`MX_MEMORY=vm`) started by
`mxload.elf` through ps2link.

| Game, demo | Normal SD | Beam racing SD-24 | Beam racing 1080i |
|---|---|---|---|
| Quake 1, `timedemo demo1` (969 frames) | **70.5** | **49.7** | **58.0** |
| Quake 1 without VM (`MX_MEMORY=phys`, `-mem 12`) | 74.6, later **89.4** | not run | not run |
| Quake 2, `demo1.dm2` (689 frames) | 48.2, later **52.3** | **42.5** | **43.0** |
| Quake 3, `demo four` (1260 frames) | **12.4** | **13.8** | **13.1** |
| Descent 3 (demo), Polaris, 599 frames from the start | 21.6, later **22.1** | **20.7** | **21.0** |

Later the same day libgl and the games were made faster (below): Quake 1 without VM
went from 74.6 to **89.4 fps**, Quake 2 in normal SD from 48.2 to **52.3 fps**,
Descent 3 from 21.6 to **22.1 fps**. The other cells were not measured again. Those
later runs were started by mxlink (the PS2's loader since then), without `mxload.elf`.

The numbers are frames a second, as each game's timedemo reports them. Descent 3
has no timedemo: its row is the first 599 frames of play in the demo's Polaris
(`-loadlevel polaris.d3l`), the ship at the start, timed with the EE's counter.

## The modes

| Mode | Picture | Buffers |
|---|---|---|
| Normal SD | PAL 640 x 512, two buffers | CT16S colour, Z24 depth; 8-bit textures also in Z's top byte |
| Beam racing SD-24 | PAL 640 x 512, a field of 640 x 256 drawn in 3 strips | CT24 strips with Z24 (`MXGL_BEAM \| MXGL_RGB24`) |
| Beam racing 1080i | 1920 x 1080, a field of 1920 x 540 drawn in 3 strips | CT16S strips with Z16S (`MXGL_1080I \| MXGL_BEAM`) |

PAL SD shows 50 fields a second and 1080i 60. With beam racing a frame is at most
one field: the most a game can reach is 50 fps in SD, 60 fps in 1080i.

Game settings:
- **Quake 1:** `vid_beam 1` for beam racing, `vid_rgb24 1` for 24-bit colour,
  `vid_mode 1` for 1080i.
- **Quake 2:** `vid_beam`, `vid_rgb24` and `vid_hires 1` (1080i). Coloured light
  (`gl_ps2colorlight 1`, the default) is on in normal SD and off with beam racing,
  whose passes own the colour mask.
- **Quake 3:** `r_ps2beam`, and `r_colorbits 32` for 24-bit colour.
  `r_customheight 448` with `r_mode -2` gives SD; `r_mode -1` with
  `r_customheight 1080` gives 1080i. Coloured light (`r_ps2colorlight 1`) is on in
  normal SD and off with beam racing.
- **Descent 3:** command-line options `-ps2beam`, `-ps2rgb24` and `-ps2hd` (1080i);
  `MXGL_REC_BYTES` 384 KB (its frames are up to 196 KB).
- **All the Quakes:** texture quality is automatic (`*_ps2texquality -1`, the default).

## What else the runs showed

Late passes are passes left out because the pass before was still being sent. That
strip shows its old contents for one field.

| Run | Late passes | Remark |
|---|---|---|
| Quake 1 SD-24 | 0 of 7735 | |
| Quake 1 1080i | 5 of 8557 | |
| Quake 2 SD-24 | 0 | |
| Quake 2 1080i | 54 of 12910 | |
| Quake 3 SD-24 | 2 of 25491 | largest record 374 of 384 KB (`MXGL_REC_BYTES` 512 KB) |
| Descent 3 SD-24 | 0 of 52479 | largest record 196 of 288 KB |
| Descent 3 1080i | 2 of 63251 | longest pass 4.2 ms |
| Quake 3 1080i | **4494 of 27192** | longest pass 3.7 ms: a frame does not fit a 1080i strip's time; largest record 378 of 384 KB |

**Quake 3 swaps heavily in every mode.**

| Quake 3 mode | Swap-ins | Swap-outs |
|---|---|---|
| Normal SD | 886, 16.6 s waiting | 1187, 29.9 s waiting |
| SD-24 | 848, 16.1 s waiting | 1248, 31.2 s waiting |
| 1080i | 1104, 21.4 s waiting | 1351, 34.8 s waiting |

The earlier baseline was 21.5 MB in and 10.2 MB out, at 14.1 fps.

**Quake 1 without virtual memory** (`MX_MEMORY=phys`, everything in the 32 MiB, run
directly without `mxload.elf`): **74.6 fps** in normal SD, against 70.5 in VM. Its hunk
is 12 MiB (`-mem 12`): with the default 16 MiB the memory runs out at the first map
(free 0 KB, the world buffer is not made) and the game crashes in libgl's `vu0_xform`
on a NULL pointer. At 12 MiB free memory is also 0 KB at the end: program 10.6 MB,
pinned 6.1 MB (libgl 4.7 MB: the textures are whole in physical memory), malloc 13.0 MB.

**Quake 1 was 89.0 fps in normal SD before this work** (a physical-memory build with
an older libgl). The transform and triangle time a frame had grown from 1.7 and
3.5 ms to about 5 and 6 ms in VM, 3.1 and 3.4 ms without VM: VM cost about 4 fps,
the rest came from libgl and the game.

## Faster: Quake 1 without VM, normal SD

From the EE's profile (`PROF=1`, frames 480 to 960 of `timedemo demo1`), each step
measured with the timedemo:

| Step | fps |
|---|---|
| Before | 74.6 |
| Lightmaps: `glTexSubImage2D` of `GL_LUMINANCE` bytes into a T8 brightness texture copied as they are (was an RGBA8 copy, then the largest of R, G, B per texel: 6.6% of the EE); the format chosen once a call, not per texel. Quake: the water's turbulence, the particles and the BSP walk in single precision (the EE has no double FPU: `__adddf3` was 2.4%). libgl's `glRotatef` with its own sine and cosine on the FPU (picolibc's are software floating point) | 80.2 |
| The texture quality's nearest vertex noted once for a queue of polygons, not per polygon; `glBegin`/`glEnd` polygons of one colour (Quake's water and sky, subdivided into many small ones) into the polygon queue with the world's, not drawn one by one | 87.0 |
| The clipper: the polygon as pointers to its vertices (was a 52-byte copy per vertex and plane), each vertex's distance once, as a dot product | 88.9 |
| Quake's BSP walk: the frustum planes a node lies wholly inside of are not tested again for its children (software Quake's clip flags) | **89.4** |

The picture of frame 960 is the same as before, but for the water's surface (it
moves with the time, which differs with the speed). `-Os` for libgl was slower
(85.8 fps). Quake 2 (VM, normal SD) went from 48.2 to 51.9 fps with the libgl part.

What is left (profile at 89.4 fps): libgl 56% of the EE (the polygon queue and the
models' strips about 9% each, the VU0 transform about 11%, clipping about 5%: 69
polygons a frame cut, about 9 us each), Quake 31% (BSP walk, dynamic lights,
`R_LightPoint`, the models' vertices), maxsdk 10%. The EE never waits for the GS.

**Quake 1 with beam racing draws its models (the gun, monsters) black** in some
builds and not in others, for the same source with different debug code linked
in. It is still open.

## The other games

The same kinds of change, looked for in the other games' profiles (`PROF=1`);
first measured in PCSX2 (EE interpreter, the same build against the last
commits of libgl and the game), then on the PS2:

| Game | Change | PCSX2 | PS2, normal SD |
|---|---|---|---|
| Quake 2 | libgl's (above); `Q_ftol` a cast, not a call (three a lightmap texel in `R_BuildLightMap`); coloured light's lightmap updates written into the three T8 planes as bytes (was an RGBA8 copy a channel, then converted) | 28.1 -> 29.2 fps | 48.2 -> 51.9 (libgl's Quake 1 changes) -> **52.3** |
| Descent 3 | the game's timer from the EE's counter (`steady_clock` went through `clock_gettime` and `gettimeofday`: 64-bit divisions in software, `__udivdi3` 2.1% of the EE); `FixSin`/`FixCos` interpolate in single precision (they did it in double) | 38.9 -> 42.3 fps | 21.6 -> **22.1** |
| Quake 3 | libgl's only | 13.6 -> 13.6 fps | not measured: the run started by mxlink was lost while loading (after `cgame`); the same ELF loads and plays in PCSX2 through `mxload.elf` |

Pictures: Quake 2's frame 200 on the PS2 the same as before to the pixel (no
difference over 24); Descent 3's frame 600 differs along edges only (the ship
moves by the frame time, which differs with the speed).

Tried and left out: Quake 3's shader effects in single precision
(`RB_CalcTurbulentTexCoords`, the deforms, scroll: under 1% of `demo four`), and
its radix sort leaving out passes over a byte equal in every key: no change in
PCSX2, where `R_SortDrawSurfs` is 20% of the profile, all on one load from VM
memory (the TLB refills of PCSX2's interpreter: to be seen on the PS2).

What is left, by the profiles: the texture cache's search (maxsdk `vcache`:
`place_new`, `find_window`) is 7-8% of the EE in Quake 2 and Quake 3, evicting
2-9 textures a frame with the working set at 73-92% of GS memory; Quake 3's sound
mixing about 16% (PCSX2); Descent 3's portal walk (`FaceIntersectsPortal` 10%) and
its per-polygon draws (libgl's `set_state`, `prepare_state`, `update_matrix`).

## Running them again

From `ps2max` (`OUT` is absolute: make runs in the game's directory):

```
make -C apps/quake1 OUT=$PWD/build/apps/n1 FRAMES=2500 ARGS="+timedemo demo1 [+vid_beam 1 +vid_rgb24 1 | +vid_mode 1 +vid_beam 1]"
make -C apps/quake2 OUT=$PWD/build/apps/n2 FRAMES=3000 ARGS="+set timedemo 1 +demomap demo1.dm2 [+set vid_beam 1 +set vid_rgb24 1 | +set vid_hires 1 +set vid_beam 1]"
make -C apps/quake3 OUT=$PWD/build/apps/n3 FRAMES=2000 ARGS="+set r_mode -2 +set r_customheight 448 +set timedemo 1 +demo four [+set r_ps2beam 1 +set r_colorbits 32]"
~/.claude/skills/ps2-hw-test/run.sh build/apps/n1/quake1.elf 1500
```

Quake 1 without VM (the ELF runs by itself; `PROF=1` and a smaller `FRAMES`, e.g. 960, for a profile, read with `libs/maxsdk/tools/prof.py`):

```
make -C apps/quake1 MX_MEMORY=phys OUT=$PWD/build/apps/n1phys FRAMES=2500 ARGS="-mem 12 +timedemo demo1"
~/.claude/skills/ps2-hw-test/run.sh build/apps/n1phys/quake1.elf 1500
```

Descent 3 (demo data, `d3demo.hog` in `build/ps2-hw-test/d3`):

```
make -C apps/descent3 DEMO=1 FRAMES=600 PLAY=1 ARGS="-pilot ps2 -loadlevel polaris.d3l [-ps2beam -ps2rgb24 | -ps2hd -ps2beam]"
~/.claude/skills/ps2-hw-test/run.sh build/apps/descent3-demo-vm/descent3.elf 1200
```

For 1080i Quake 3 use `+set r_mode -1 +set r_customheight 1080 +set r_ps2beam 1`.

Each run ends with the timedemo's `frames ... seconds ... fps` line. Beam racing's
`stats: beam:` lines give the late passes and the record sizes. `maxsdk: VM` gives
the swap traffic. The game data goes in `build/ps2-hw-test/{id1,baseq2,baseq3}`;
Quake 3 also needs `ps2tex.pk3` there (`make -C apps/quake3 pak`).
