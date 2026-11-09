# Policenauts — PSX static recompilation

Static recompilation of *Policenauts* (PlayStation, SLPS-00215/00216, English patch
v1.0) into native C. The MIPS R3000A code from the disc is translated into C
functions ahead of time, then compiled and linked against a runtime that stands in
for the PS1 hardware and BIOS.

No game data is in this repository. You supply your own disc image, and everything
derived from it (`extracted/`, `generated/`, `build/`) is git-ignored.

## Build

Requirements: Python 3.11+, MSYS2 (`ucrt64` gcc, `mingw64` ninja) or any gcc/clang + ninja.

```bash
python -m venv .venv && .venv/Scripts/python -m pip install -r requirements.txt
export PATH=/c/msys64/ucrt64/bin:/c/msys64/mingw64/bin:$PATH
.venv/Scripts/python tools/extract_all.py "<Disc 1 .bin>" extracted   # first time only
.venv/Scripts/python recomp/recomp.py
.venv/Scripts/python configure.py
ninja
./build/policenauts.exe
```

`configure.py` finds the Disc 1 image in `Policenauts PSX*/` automatically (or pass
`--disc`). After the first configure, `ninja` re-runs extraction and recompilation when
their inputs change.

## Layout

| Path | Contents |
|---|---|
| `recomp/segments.toml` | Code segments: file, load address, overlay slot |
| `config/symbol_addrs.<seg>.txt` | splat-style symbols (`name = 0xADDR; // type:func`) — named functions and extra entry points |
| `recomp/` | Recompiler: `mips.py` decode, `analyze.py` function discovery, `emit.py` C output |
| `runtime/` | Hand-written runtime: memory, dispatch, BIOS/HW (currently stubs) |
| `tools/` | `extract_all.py` (disc → `extracted/`), `psxiso.py` (ISO9660 from raw bin), `dpk.py` (FRID archives), `gen_task_syms.py` (kernel task table → `config/`), `disasm.py` (debug listing), optional m2c checkout |
| `generated/` | Recompiler output (ignored) |

## What's on the disc

Both discs ship byte-identical `SLPS_002.1x` and `BIN.DPK`, so one build serves both.

`SLPS_002.15` is not the game itself. It's Konami's **Kernel Ver2.1.0** (R&D Dept.5,
1994–95): a small multitasking OS that runs `NAUTS/KERNEL.SC`, a script of
`segment` / `load` / `launch` commands. Game code lives in headerless overlays inside
`BIN.DPK` (FRID archive), each linked at a fixed address.

| Address | Contents |
|---|---|
| `0x80010000` | kernel (crt0, task scheduler, DPK/CD file system, sound driver, memcard) |
| `0x80033688`–`0x80043E04` | kernel BSS |
| `0x80043E04` | `DISPLAY.BIN` — resident display/text library |
| `0x8006CEE8` | slot A: `ITP` (script interpreter) · `ENGLISH` · `SUBS1` (carchase, shtrainer) · `MENU` |
| `0x80087614` | slot B: `OPEN` · `MV` (projector/FMV) · `BLOOD` · `MASQ1/2` · `SUBS2/3/4` (shooting) · `ROLL` |
| `0x80111308` | code embedded in the kanji font module (`FONT.DPK/KPRFONT.MDB` from offset `0x10000`; the font always loads at `0x80101308`) |

The load addresses come from the kernel's overlay table at `0x80010034`. Task entry
points come from the launch table at `0x800309E8` (36-byte records
`{task[16], file[16], entry}`) and are written to `config/` by `tools/gen_task_syms.py`.

Tasks run on **BIOS threads** (`OpenTh`/`ChangeTh`/`CloseTh` through the standard B0
stubs), so the runtime can map each kernel task to a host fiber; no register-level
context-switch emulation is needed. GTE use is close to zero: this is a 2D engine.

## How the recompiler works

- **Discovery**: seeds are the entry point, every `jal` target (including calls from
  other segments into resident code), symbols from `config/`, data words and
  `lui`/`addiu` constants that point at `addiu sp,sp,-N` prologues, and finally any
  remaining prologue that decodes cleanly. Each function is a recursive-descent walk.
  Branches, switch jump tables (GCC `lui/addiu/addu/lw/jr` idiom) and `j` to
  non-function addresses become `goto`s. `j` to a known function start becomes a tail
  call. Functions may overlap; duplicated C is still correct.
- **Emission**: one C function per MIPS function (`f_<seg>_<ADDR>(CPU *c)`), registers
  in `CPU`, delay slots reordered after the branch condition is computed. BIOS stubs
  (`li t2,0xB0; jr t2; li t1,N`) become `bios_call()`. `jalr` and calls into another
  overlay slot go through `call_dynamic()`, which picks the overlay currently in the
  slot by checking signature code words in RAM.
- ASPSX emits a MIPS-II trap word (`0x00001770`) for the `INT_MIN / -1` division check;
  it's treated as `break`.

Current output: 2,442 functions over 16 segments, 2 dynamic call sites (font code calling into slot A). Every
backward branch carries a cheap `BACKEDGE()` countdown, so loops that poll RAM still
let virtual time and interrupts advance.

## Runtime

- **BIOS HLE** (`runtime/bios.c`): A0/B0/C0 tables, critical sections, events, heap,
  console output, pads, and memcard/file calls (`runtime/card.c`, currently "no card").
- **Threads**: TCBs live in emulated RAM with the real BIOS layout, because the
  kernel copies whole TCB images between slots and rewrites the current-TCB pointer
  itself. `ChangeTh` saves registers into the current TCB and loads the target image,
  as the real BIOS does. The recompiled call stack lives in a Win32 fiber. Each
  saved image carries a tag (fiber id + suspend count) in two unused TCB words, so it
  stays bound to its fiber when the kernel copies it. Untagged images start a new fiber
  at their EPC. Fibers whose images are gone from RAM are freed on the next `OpenTh`.
- **Interrupts**: libetc hooks IRQs with `SetCustomExitFromException(jmp_buf)`. The
  runtime maps the jmp_buf's resume PC to the dispatcher it would call
  (`irq_resume_table`), runs it on its own CPU context, and `ReturnFromException`
  `longjmp`s back to the interrupted point.
- **Hardware** (`runtime/hw.c`): virtual clock (NTSC line/frame timing), I_STAT/I_MASK,
  root counters 0–2 with target/overflow IRQs, GPUSTAT odd-line bit, DMA (OTC done,
  GPU list consumed but not drawn, others ignored).

- **CD-ROM** (`runtime/cd.c`): register interface, command set (Getstat, Setloc,
  ReadN/S, Pause, Init, Setmode, Seek, GetID, GetTN/TD, GetlocL/P, Test, ReadTOC…), timed
  INT1–INT5 queue that waits for acknowledgement like the real controller, sector
  streaming at 1×/2×, data FIFO plus DMA channel 3. XA-ADPCM audio sectors are routed to
  `cd_xa_sector()` (a stub until the SPU exists). Disc paths come from `build/discs.txt`
  (written by `configure.py`) or `--disc1/--disc2`.
- **Preemption**: on an interrupt the running context is saved into the current TCB, and
  `ReturnFromException` resumes whatever TCB is current afterwards. The kernel's
  scheduler preempts tasks by repointing the current TCB inside its VBlank handler.
- **Hooks** (`recomp/hooks.toml` → `runtime/hooks.c`): native code attached to a
  recompiled function, run before it or instead of it. The kernel's debug log is
  printed to stderr this way (`[K] ...`; silence with `PSX_QUIET=1`).

- **GPU** (`runtime/gpu.c`): 1024×512 VRAM, GP0 polygons (flat/gouraud/textured,
  4/8/15-bit CLUT textures, texture window, raw/modulated), lines and polylines, rects
  and sprites with flips, fills, VRAM copies and CPU↔VRAM transfers, semi-transparency
  modes, mask bits, dithering. GP1 display control. Software rasterizer with top-left
  fill, quads drawn as two triangles, and 16.16 attribute gradients. Fed by GP0/GP1
  writes and DMA channel 2 (linked lists and blocks).
- **Window** (`runtime/display.c`): Win32/GDI, 4:3 letterboxed, shows the GPU display
  area (15- and 24-bit) every VBlank, paced to 59.94 Hz.
- **Input** (`runtime/input.c`): keyboard → pad 1 through the BIOS pad buffers. Arrows
  = D-pad, Enter = Start, Backspace = Select, Z/X/A/S = ✕/○/□/△, Q/W = L1/R1, 1/2 = L2/R2.
  `PSX_INPUT="start@30,cross@41-42"` scripts presses in virtual seconds.

Debug switches: `PSX_TRACE_BIOS=1` (BIOS calls, fiber switches), `PSX_TRACE_HW=1`
(register accesses), `PSX_TRACE_CD=1|2` (commands/interrupts, plus data requests),
`PSX_TRACE_DMA=1`, and `PSX_MAX_SECONDS=N`, which stops after N virtual seconds and
prints the hottest loop functions plus CPU/IRQ state, and `PSX_PEEK=addr,...` prints
memory words when it stops. Display: `PSX_HEADLESS=1` (no window), `PSX_FAST=1` (no
60 Hz pacing), `PSX_DUMP_DIR=dir` + `PSX_DUMP_EVERY=N` (frames as BMP, plus
`final.bmp`/`vram.bmp` on a timed stop), `PSX_TRACE_GPU=1|2` (GP1 / also GP0). An
unknown call target dumps RAM to `build/ram_crash.bin`.
`tools/contact_sheet.py` turns BMP dumps into one PNG.

Headless title-screen check (presses Start during the intro movie):

```bash
PSX_INPUT="start@30" PSX_HEADLESS=1 PSX_FAST=1 PSX_DUMP_DIR=build/frames PSX_MAX_SECONDS=45 ./build/policenauts.exe
```

Boot status: crt0 → Multi Task Scheduler v1.31 → CD mounted → `KERNEL.SC` read →
sound task → `DISPLAY.BIN` + display manager → kanji font, RUBI data, DATA table →
segment #12: `OPEN.BIN` intro → Konami logo TIM uploaded, XA audio, opening movie
(`VZ003XA.STR`, 1244 frames) streams and loops as attract mode. Pressing Start →
segment #15: `MENU.BIN` → memory card check ("no card") → **title screen with the
New Game / Continue / Options menu renders correctly**. The FMV itself is still blank
(no MDEC) and there's no sound yet.

### Kernel scheduler notes

Tasks share BIOS TCB slot 0; the scheduler lives in slot 1 (`0x80020004`). It picks
the lowest-numbered ready task (state 3) from the task table at `0x8003CE50` (0x28
bytes per task, current id at `0x8003D850`) and swaps TCB images through slot 0. The
wake routine (`0x80020894`) marks a task ready and, if it outranks the current task,
points the BIOS current-TCB at slot 1 so the return from the interrupt lands in the
scheduler. Its time tick is a `RCntCNT3` (VBlank) event callback (`0x80023398`), which
the BIOS has to deliver from its interrupt chain before libetc's hook runs. That
delivery was the missing piece behind the post-`open` stall.

`tools/decomp.py <segment> <addr>` runs m2c on a recompiled function (using the
recompiler's function bounds) and is the quickest way to read kernel code.

## Roadmap

1. ~~BIOS HLE + threads~~ (done; memcard is still "no card")
2. ~~Interrupts/timing~~ (done: VBlank, root counters, DMA skeleton)
3. ~~CD-ROM~~ (done, except XA decode and disc swap)
3b. ~~Scheduler stall after `open` launches~~ (fixed: BIOS root-counter events)
4. ~~GPU + window + input~~ (done; title screen renders)
4b. **MDEC + STR** ← next: makes the intro FMV visible
5. **SPU**: ADPCM voices, ADSR, reverb (BGM/VOX/SE are SPU-streamed from `PN_*.PAC`)
7. **Input**: gamepad support; light gun for the shooting sections
8. Verify `ROLL` and slot B co-residency; memory card saves (`bu00:` backed by a host file)

## Reference toolchain

splat / spimdisasm / rabbitizer (disassembly), m2c (readable C for functions to
replace by hand), decomp.me (per-function matching), and psyq-obj-parser (Psy-Q
library signatures for HLE). The matching tools (maspsx, GCC 2.6.3, asm-differ,
decomp-permuter, objdiff) aren't needed for the recomp, but symbols in `config/` use
the same format, so decomp work and recomp work share names.
