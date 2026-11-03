# Frame rates of the Quakes on the PS2

Timedemos on a PAL PS2 (real hardware), 2026-11-03. The games were built with
maxsdk `bfef886` and libgl `2212e9a`, as VM programs (`MX_MEMORY=vm`) started by
`mxload.elf` through ps2link.

| Game, demo | Normal SD | Beam racing SD-24 | Beam racing 1080i |
|---|---|---|---|
| Quake 1, `timedemo demo1` (969 frames) | **70.5** | **49.7** | **58.0** |
| Quake 2, `demo1.dm2` (689 frames) | **48.2** | **42.5** | **43.0** |
| Quake 3, `demo four` (1260 frames) | **12.4** | **13.8** | **13.1** |
| Descent 3 (demo), Polaris, 599 frames from the start | **21.6** | **20.7** | **21.0** |

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

**Quake 1 was 89.0 fps in normal SD before this work** (a physical-memory build with
an older libgl). The transform and triangle time a frame has grown from 1.7 and
3.5 ms to about 5 and 6 ms. Not investigated yet.

**Quake 1 with beam racing draws its models (the gun, monsters) black** in some
builds and not in others, for the same source with different debug code linked
in. It is still open.

## Running them again

From `ps2max`:

```
make -C apps/quake1 OUT=build/apps/n1 FRAMES=2500 ARGS="+timedemo demo1 [+vid_beam 1 +vid_rgb24 1 | +vid_mode 1 +vid_beam 1]"
make -C apps/quake2 OUT=build/apps/n2 FRAMES=3000 ARGS="+set timedemo 1 +demomap demo1.dm2 [+set vid_beam 1 +set vid_rgb24 1 | +set vid_hires 1 +set vid_beam 1]"
make -C apps/quake3 OUT=build/apps/n3 FRAMES=2000 ARGS="+set r_mode -2 +set r_customheight 448 +set timedemo 1 +demo four [+set r_ps2beam 1 +set r_colorbits 32]"
~/.claude/skills/ps2-hw-test/run.sh build/libs/maxsdk/mxload.elf 1500 host:build/apps/n1/quake1.elf
```

Descent 3 (demo data, `d3demo.hog` in `build/ps2-hw-test/d3`):

```
make -C apps/descent3 DEMO=1 FRAMES=600 PLAY=1 ARGS="-pilot ps2 -loadlevel polaris.d3l [-ps2beam -ps2rgb24 | -ps2hd -ps2beam]"
~/.claude/skills/ps2-hw-test/run.sh build/libs/maxsdk/mxload.elf 1200 host:build/apps/descent3-demo-vm/descent3.elf
```

For 1080i Quake 3 use `+set r_mode -1 +set r_customheight 1080 +set r_ps2beam 1`.

Each run ends with the timedemo's `frames ... seconds ... fps` line. Beam racing's
`stats: beam:` lines give the late passes and the record sizes. `maxsdk: VM` gives
the swap traffic. The game data goes in `build/ps2-hw-test/{id1,baseq2,baseq3}`;
Quake 3 also needs `ps2tex.pk3` there (`make -C apps/quake3 pak`).
