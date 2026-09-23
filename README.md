# FanForge

**Free, open-source fan and thermal control for Intel Macs running Windows via Boot Camp.**

Boot Camp hands the fans to the firmware and leaves them there. The firmware is
tuned for a quiet office, not for the game or the render you actually booted into
Windows to run, so the machine gets hot and stays hot. FanForge takes the fans
off the firmware, reads every temperature sensor the Mac's SMC exposes, and drives
each fan from a curve you can see and edit.

No trial, no paid tier, no account, no telemetry, no installer. Two static
`.exe` files you can put anywhere.

---

## Read this first

**FanForge has never been run on a real Intel Mac.** The author does not own one.
What *has* been verified is written down honestly below, and the gap is real: the
SMC protocol is implemented from public documentation and validated against a
simulator, not against a Mac.

So before you trust it with your machine:

```
FanForge.exe --check
```

That opens the SMC, reports which of the two access paths your machine provides,
counts the keys and fans, and prints each fan's reported speed range — without
writing a single value. If it prints a fan range, the risky half is working. If it
does not, it tells you exactly what is missing.

Then `fanforge-cli.exe selftest`, which engages manual control *at the speed each
fan is already running at* (so nothing audible changes), verifies the readback, and
restores the original state. If both of those pass, the write path works on your
machine.

| What | Status |
|---|---|
| Unit and integration tests | **162 tests, 1164 checks, all passing** |
| Same tests on the Windows binaries, under Wine | **Passing** |
| Address sanitizer + undefined behaviour sanitizer | **Clean** |
| Cross-compiled to real Windows executables (x86-64 and x86) | **Yes, zero warnings** |
| Whole control loop against a simulated machine | **Yes — `fanforge-cli simulate`** |
| **The shipped `.exe` driving a real SMC protocol** | **Yes — 17/17 checks, see `tools/fake_smc`** |
| **The GUI window opening and driving the fans** | **Yes — verified under a virtual display** |
| Access paths implemented | **Two: `\\.\APPLESMC` and raw port I/O (InpOut32 / WinRing0)** |
| Any SMC write executed against real Mac hardware | **No — never** |
| Fan curve left running on a real Mac for hours | **No — never** |

That fake-SMC row is the important one. The port-I/O path cannot be executed on
a machine that is not a Mac, so for a long time it had never run at all — and a
code path that cannot be executed is not a tested code path. `tools/fake_smc`
implements the SMC's register state machine as a drop-in `inpoutx64.dll`, and
`tools/fake_smc/run_e2e.sh` drives the *shipped* executables against it: the real
loader, the real poll loop, the real transaction framing, the real controller, and
the real window. The first time it ran it found a crash that took down every
command of the command-line tool on a **successful** SMC connection — which is why
it never showed up on any machine without a Mac's SMC, and would have appeared on
yours immediately.

If it does something wrong on your machine, `fanforge-cli.exe auto` hands every fan
back to the firmware, and the app does that by itself on exit, on logoff, and on
shutdown.

---

## How this compares to the other free tools

Two other free, open-source Boot Camp fan controllers exist, and both have been
run on real Macs. They are the bar, and their published findings are part of why
this project's protocol layer looks the way it does.

| | **FanForge** | **MacFanCtl** (charlie754, MIT) | **RPMAC** (golirt1, GPL-2.0) |
|---|---|---|---|
| Access path | `\\.\APPLESMC` **and** port I/O | `\\.\APPLESMC` | port I/O, plus PawnIO for T2 |
| Extra driver to install | none | none | InpOut32; PawnIO on a T2 Mac |
| Run on real hardware | **no — not by the author** | MacBookPro14,3 | five Macs |
| T2 Macs (2018–2020) | expected, through Apple's own driver | **confirmed** on MacBookPro15,1 | needs an unsigned PawnIO module |
| Curves / tray / extras | curves, tray, °F, CSV, emergency cooling | curves, tray | curves, tray, presets, overlay, history, CSV, themes |
| Cross-process SMC lock | yes (`Global\AppleSmcAccess`) | single instance only | yes (`Global\AppleSmcAccess`) |
| Licence | MIT | MIT | GPL-2.0 |

What this project actually adds: **both access paths behind one interface**, so a
machine whose Boot Camp install does not publish the device can still be
controlled, and a machine with no helper driver still gets monitoring. Neither of
the others does both. On top of that, every write is *verified by readback*
rather than trusted, and the engine is portable with a test suite that drives the
real protocol against a simulated SMC chip.

**The T2 finding matters if your Mac is a 2018–2020 model.** On a T2 Mac the
legacy SMC I/O ports are intercepted by the T2 chip and return nothing. That is
why RPMAC needs a kernel MMIO module for those machines — and that module is not
signed yet, so the path is not usable in its released builds. Apple's own
`applesmc.sys` driver does the T2 work internally and publishes the same
`\\.\APPLESMC` device, and MacFanCtl has a **confirmed** report of full fan
control on a MacBookPro15,1 (2018, 8-core i9, Apple T2) through exactly that
device. FanForge prefers that same device, so the hardest hardware case is the one
it inherits working — provided the Boot Camp support software is installed, which
is precisely what `--check` tells you.

The honest summary of the bar: those two have been on real Macs and this has not.
What this has that they do not is the redundant transport and the
verified-on-every-write discipline. If you want a tool with hardware mileage
behind it, use one of theirs. If you want both access paths, or you want to read
the code that decides whether your machine is safe to write to, use this.

---

## Requirements

An **Intel** Mac (Apple Silicon has no Boot Camp and no way to reach the SMC) with
Windows installed natively through Boot Camp. Then, one of:

**Preferred — nothing extra to install.** The Boot Camp support software ships an
`AppleSMC` kernel service that publishes `\\.\APPLESMC`. If it is installed and
running, FanForge uses it. Check with:

```
sc query AppleSMC
```

You want `STATE : 4 RUNNING`. FanForge will try to start it for you on launch,
which needs Administrator.

**Fallback — a helper driver.** Some Boot Camp installs do not publish the device.
There, the SMC's two I/O ports are still reachable, but Windows does not let a
normal process touch I/O ports. Put one of these next to `FanForge.exe`:

| Helper | Files | Notes |
|---|---|---|
| **InpOut32** (preferred) | `inpoutx64.dll` + its driver | Small and signed. Get it from Highresolution Enterprises. |
| **WinRing0** | `WinRing0x64.dll` | Used by other hardware tools. Windows may refuse to load it if memory integrity or the vulnerable-driver blocklist is on. |

FanForge looks for both. If neither is present it says so plainly rather than
failing mysteriously. **FanForge does not ship or install a driver of its own** —
any driver at the kernel boundary is one you chose and can inspect.

`--check` and the tray tooltip both tell you which path is in use.

---

## Quick start

1. Download and unzip `fanforge-windows-x64.zip` (or `-x86` for 32-bit Windows).
2. Run `FanForge.exe --check`. Read what it says.
3. Run `FanForge.exe`. The window opens with the sensor list on the left and one
   panel per fan on the right.
4. Drag a point on a fan's graph, or press **Manual** and move the slider.
5. To walk away cleanly: tray icon → **Return all fans to system control**, or
   just Exit. Both hand the machine back.

Closing the window hides it to the tray — deliberately. The app exists to hold a
fan speed while something else is running, and a stray close should not drop that
mid-game. **Exit** is in the tray menu.

---

## What it does

**Live temperatures.** Every sensor the SMC exposes, hottest first, with plain
names (`TC0P` → "CPU proximity", `TG0D` → "GPU die"). Sensors that report the
SMC's dead-value sentinels are filtered out rather than shown as −127 °C. On a
two-fan MacBook Pro that is typically fourteen real sensors out of fifteen keys.

**Three modes per fan, independently:**

| Mode | What it does |
|---|---|
| **System** | Hands the fan back to the firmware's own thermal loop. |
| **Manual** | A fixed speed. The slider is clamped to the fan's reported range. |
| **Curve** | A temperature → speed curve you edit by dragging points. Follows whichever component is hottest, or a sensor you pick. |

**A curve editor that cannot produce a broken curve.** Drag a point and it stays
between its neighbours in both temperature and speed. Double-click empty graph
space to add a point; right-click one to remove it. The consequence is that the
editor can never create a folded or backwards curve, which matters a lot: an
invalid curve makes the controller hand the fan back to the firmware, so a
permissive editor would feel like the graph did nothing.

**The red dot is the fan, the line is the plan.** The gap between them is the
fan's own lag, shown rather than smoothed over.

**A tray icon** with the live temperature and fan speed, and a menu to hand every
fan back at once.

**Emergency cooling.** If any sensor reaches a temperature you set (95 °C by
default), every fan goes to its maximum regardless of its mode, and a banner
appears in the window until the heat passes. At that point the firmware's own loop
has already had its chance, so continuing to follow a quiet curve is the wrong
answer.

**A non-destructive write-path test**, in the tray menu and as
`fanforge-cli.exe selftest`. It engages manual control at the speed each fan is
*already* running at, confirms the readback, and puts everything back exactly as
it found it — so you can find out whether your Mac actually accepts fan writes
without changing anything you can hear. It knows the difference between the two
manual-control styles, so it does not report a false failure on a T2 machine.

**°C or °F, and an optional CSV log** of every reading, for looking at a thermal
problem after the fact rather than while it is happening. A logging problem is
never allowed to interrupt fan control.

**Settings that survive a reboot**, including the curves, in
`%APPDATA%\FanForge\config.ini`. It is a plain text file; edit it by hand if you
like.

---

## How it works

Nothing about the machine is hard-coded. The fan count comes from `FNum`, labels
from `F<n>ID`, speed limits from `F<n>Mn`/`F<n>Mx`, and every value is decoded
from the type the SMC declares for that key. An unrecognised key still shows up
with a generated name. So the code is model-agnostic by construction — but only
some models have been thought about, and **none have been tested on hardware**.

### Two access paths

```
                        ┌─────────────────────────────┐
  Controller ─┐         │  AppleSMC device (IOCTLs)   │  Boot Camp drivers
              ├────────▶├─────────────────────────────┤
  FanBank    ─┤         │  Port I/O 0x300 / 0x304     │  InpOut32 / WinRing0
  SmcDevice  ─┘         └─────────────────────────────┘
```

Both are behind one interface, `ISmcTransport`. `SmcSession::open()` tries the
device first, falls back to the ports, and — this is the part that matters —
**validates each one by reading the key count**. A path that opens but cannot
answer is rejected, so "the driver is loaded" and "the SMC is reachable" are
never confused for one another, and the diagnostic says which one is true.

Supporting both is the point. A Mac whose Boot Camp install does not publish the
device can still be controlled; a Mac without a helper driver still gets
monitoring for free.

### The SMC protocol

Four-character keys, each with a size, a type, and access flags. Temperatures are
`sp78` (signed fixed point, /256); fan speeds are `fpe2` (unsigned, /4). Integer
types are big-endian; `flt` is the one exception and is little-endian, which is
worth knowing because getting it wrong makes every power reading on the machine
report zero.

Manual fan control is exposed in one of two ways and the hardware decides which:
older Intel Macs use one bit per fan in the shared `FS! ` mask, newer ones use a
per-fan `F<n>Md` mode key. FanForge reads which is writable and takes that path.

### Safety

The firmware's own thermal protection is never disabled — the SMC can always
throttle the CPU and spin the fans up regardless of what FanForge asks for.
Beyond that:

- **Targets are clamped** to the `F<n>Mn`/`F<n>Mx` range the fan itself reports.
- **A fan whose range cannot be read is never driven.** No guessing.
- **Every write is verified by reading it back.** The port protocol has no
  acknowledgement at all — a rejected write still looks like a success — so the
  readback is the only honest confirmation available. If it does not match, the
  fan is handed back to the firmware rather than left in a state we cannot
  explain.
- **A curve whose sensor stops reporting hands the fan back** instead of holding
  a stale speed.
- **Fans come down on the firmware's schedule, not the temperature's.** Rises are
  fast (heat is the emergency); falls are limited to 120 RPM/s, which is what
  stops the audible up-down hunting.
- **Fans are released** on normal exit, on window-close-to-tray, on Exit,
  on `WM_ENDSESSION` (logoff and shutdown), and on an unhandled crash.
- **Manual control dropped by the firmware is re-asserted**, immediately on
  resume rather than at the next poll. The SMC drops manual control across a
  suspend, so a power event forgets the command cache and re-applies every policy
  at once instead of waiting to notice the difference.
- **Other SMC tools are cooperated with, not fought.** Every transaction is taken
  under the named mutex `Global\AppleSmcAccess` — the name the other Boot Camp fan
  tools publish for exactly this purpose. Two programs interleaving register
  traffic read each other's answers, which surfaces as impossible sensor values
  rather than as an error, and the shared name is what prevents it. If the name
  cannot be created the lock degrades to nothing rather than refusing to control
  the fans.

**The one case that leaves a fan pinned:** killing the process outright — Task
Manager → End task, or a power loss. Windows gives it no chance to clean up, so a
fan left in manual mode stays there. Fix it with:

```
fanforge-cli.exe auto
```

That is not hypothetical. Prefer Exit from the tray.

---

## Command line

`fanforge-cli.exe` runs the same engine without a window, which is also how to
diagnose a machine before trusting the GUI with it.

| Command | What it does |
|---|---|
| `info` | Access paths, key count, fan summary, control style |
| `temps` | Every plausible temperature, hottest first |
| `sensors` | All numeric keys — temperatures, voltages, currents, power |
| `fans` | Fan speeds, limits, and current control mode |
| `keys` | Every SMC key with its type, access flags and value |
| `get <KEY>` | Read one key, e.g. `get TC0P` |
| `set <fan> <rpm>` | Take one fan manual at a speed, clamped and confirmed |
| `auto [fan]` | Return one fan, or every fan, to system control |
| `restore` | Return every fan to system control |
| `selftest` | Non-destructive check of the whole write path |
| `watch [seconds]` | Run the saved configuration's control loop |
| `simulate [seconds]` | Run the whole loop against a simulated machine |

`simulate` works on any machine and needs no Mac hardware:

```
                      cool     balanced      quiet
  peak                59.5 C       64.3 C     72.9 C
  settled             59.4 C       64.3 C     72.9 C
  average fan           6723         5665       4466
  control writes          62           57         32
  fans released          yes          yes        yes
```

The controller, fan bank and SMC protocol there are the real ones; only the
machine's thermal response is modelled, and the model's assumptions are printed
above the table. The numbers are illustrative. Every write, clamp and safety
fallback in them is real code.

---

## Building

Needs a C++17 compiler and CMake 3.16+.

**On Windows** (MSYS2, or MinGW-w64 with Ninja):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

**Cross-compiling from Linux** — this is what CI does:

```
sudo apt-get install -y mingw-w64
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win -j
```

Both toolchains are in `cmake/`. The result is `FanForge.exe`, `fanforge-cli.exe`,
and `fanforge_tests.exe`, all statically linked — copy them anywhere, no runtime
DLLs.

**On Linux, macOS, or anything else** the engine and the tests build and run
normally; only the two Windows shims and the window are skipped, so you can work
on the controller and the protocol without a Windows machine at all. The CLI will
tell you honestly that there is no SMC here rather than pretending.

### Layout

```
src/smc/         SMC protocol: keys, types, both transports, the simulator
src/core/        Sensors, fans, curves, the control loop, settings
src/platform/    The only OS-specific file(s): the device shim and the port I/O shim
src/sim/         Closed-loop thermal simulation
src/cli/         Command line front end
src/win/         The window
cmake/           Cross-compilation toolchains
tests/           The test suite
```

---

## Testing

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/fanforge_tests
```

The design decision that makes this testable: **everything the SMC does above the
raw OS call is portable C++**, and the two OS shims are one method each
(`inb`/`outb`, and an `ioctl`). So the suite drives both transports directly against
a **simulated SMC**, which reproduces the register-level handshake — status bits,
command bytes, the four-byte key argument, the length byte, the trailing flush.
`PortIoTransport` is therefore tested as a protocol implementation, not stubbed
around.

What the tests actually cover:

- **Key types.** `fpe2`, `sp78`, `flt` (and that it is little-endian), `ui8/16/32`,
  `si*`, `flag`, `ch8*`. Encoding clamps rather than wraps, and refuses a type it
  cannot represent instead of producing a wrong number.
- **Both transports**, on the same machine state, asserting byte-identical
  results — a fallback that disagrees with the primary path is not a fallback.
- **Failure modes.** Unknown keys, a wedged controller, a device that will not
  open, a write that silently does not take. All bounded, none hanging.
- **Fan discovery** on four machine shapes: two-fan, fanless, no-limits, and
  per-fan-mode.
- **The control loop**: mode changes, clamping, rate limiting in both directions,
  deadband, sensor selection, a sensor that stops reporting, a curve that becomes
  invalid, and re-asserting control after the firmware drops it.
- **Emergency cooling**: that it overrides every mode including `System`, that it
  can be switched off, that it still refuses to drive a fan whose range is
  unknown, and that the curve takes over again once the heat passes.
- **The write-path selftest**, including the two cases that are easy to get
  wrong — a machine that uses per-fan `F<n>Md` instead of the `FS!` mask (where a
  mask-only check reports a false failure), and a write the SMC silently drops
  (which must be caught, not passed).
- **Settings** round-tripping, including hand-written files and malformed values,
  and the temperature-unit conversion at its fixed points.
- **CSV logging**, down to the exact bytes written, including that a missing
  reading becomes an empty cell rather than `nan`, and that a bad path is
  reported instead of thrown.
- **The whole loop**, closed, against a thermal model — including that it always
  lets go of the fans at the end.

### The shipped binaries, against a fake SMC

```
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
cmake --build build-win -j
tools/fake_smc/run_e2e.sh build-win
```

This is the only test that runs the port-I/O path, the write path and the window
at all. It checks that the tool finds the access path, reads the fan count, labels,
ranges and control style, filters the SMC's `-127` dead-sensor sentinel, confirms
the write path with the non-destructive selftest, **reports a swallowed write as a
failure rather than a success**, writes fan targets from the control loop, hands
the fans back on exit, writes the CSV log, opens the real main window, and — from
the GUI — actually writes to the SMC. See
[tools/fake_smc/README.md](tools/fake_smc/README.md).

Sanitizers:

```
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
cmake --build build-asan -j && ./build-asan/fanforge_tests
```

CI runs the suite natively, then again **as a Windows binary under Wine**, then
runs the full control loop as a Windows binary, then checks that `--check` fails
correctly on a machine with no Boot Camp drivers, and then runs the whole
**fake-SMC harness above**, so the port-I/O path and the GUI are exercised on
every push. The Windows executables are attached to every run, and to a release
when you push a tag.

---

## Troubleshooting

**"the AppleSMC driver is not installed or its service is stopped"**
The Boot Camp support software is missing or incomplete. Reinstall it from
Apple's Boot Camp Support Software download for your model, or run
`sc start AppleSMC` as Administrator. Failing that, install InpOut32 and use the
port path.

**"the device is already in use"**
`\\.\APPLESMC` allows one program at a time. Close any other SMC utility —
including another copy of FanForge, or a monitoring tool that leaves itself in the
tray.

**"This fan does not report a speed range"**
The fan's `F<n>Mn`/`F<n>Mx` keys could not be read, so FanForge refuses to drive it
rather than guessing. Its speed is still displayed. Please open an issue with the
output of `fanforge-cli.exe keys`.

**A fan is stuck at a speed I did not ask for**
`fanforge-cli.exe auto`.

**It is louder than I want at idle**
Press **Curve** and drag the leftmost point to the right — a curve that stays at
minimum until 60 °C is a normal thing to want. Or use **System**.

---

## References and credit

The SMC protocol is public. It was reimplemented from scratch here, from:

- The Linux kernel's `applesmc` driver — the canonical reference for the port
  protocol, the status bits and the command bytes.
- Apple's own `AppleSMC` interface, as documented in the Darwin sources.
- `charlie754/mac-fan-control-windows` (MIT), whose README documents the
  `\\.\APPLESMC` IOCTL numbers and the key encoding, verified on a
  MacBookPro14,3. Its protocol observations are the reason the device path here
  is likely to work at all.
- `bmats/fancontrol` and the wider `smcFanControl` / `HWSensors` lineage, for how
  manual fan control is expressed on these machines.
- `golirt1/RPMAC` (GPL-2.0), a free Boot Camp controller verified on five Macs.
  Its published notes are where the `Global\AppleSmcAccess` mutex convention came
  from, and they independently confirm two things this implementation had already
  got right: that `flt` is the one *little-endian* SMC type while `fpe2`/`sp78`
  are big-endian, and that a T2 Mac exposes per-fan `F<n>Md` instead of the `FS!`
  mask.

No code was copied from any of them, and none of them were decompiled — including
the closed-source Macs Fan Control, which was suggested but deliberately not done.
Decompiling it would be legally murky, and it would have taught this project less
than the Linux kernel driver and the two implementations above, which are better
sources precisely because their protocol observations come with the hardware they
were verified on. Macs Fan Control is a *product* reference for what a fan
control app should do, never a source. See [NOTICE](NOTICE) for full provenance,
including the licence position on the GPL-2.0 sources.

The `rpm`/`fan control` idea space has been public since 2006. What is new here is
supporting both access paths behind one interface, verifying every write by
readback, and treating a machine whose limits cannot be read as one to leave alone.

## Licence

MIT. See [LICENSE](LICENSE).

FanForge writes to a hardware fan controller. It is provided with no warranty, and
you run it at your own risk.
