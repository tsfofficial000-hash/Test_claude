# A fake SMC, so the Windows binaries can be tested without a Mac

`inpout32_shim.c` builds a drop-in replacement for `inpoutx64.dll` that implements
the Apple SMC's I/O-port interface in user space. Put it next to `FanForge.exe`
and the program talks to it exactly as it would talk to a real Mac: the same
Win32 loader, the same InpOut32 discovery, the same poll loop, the same
transaction framing, the same key decoding, the same fan bank and controller, the
same window.

The only thing that is not real is the chip at the other end of ports `0x300`
and `0x304`.

This exists because the port-I/O path cannot be executed at all on a machine that
is not a Mac, and **a code path that cannot be executed is a code path that is not
tested.** The first time this harness ran, it found a crash that would have taken
down every command of the command-line tool on real hardware — and only there,
because it needed a *successful* SMC connection to appear.

## What it reproduces

- The status register: `0x01` a byte is ready, `0x02` input closed, `0x04` busy.
- A transaction: command to `0x304`, then four key bytes and a length byte to
  `0x300`, then either `length` bytes read back or `length` bytes written.
- That **a read returns the whole key** regardless of the requested length, which
  is what the transport's trailing flush loop exists for.
- A two-fan MacBook Pro key space: `FNum`, `F<n>ID`, `F<n>Mn`, `F<n>Mx`,
  `F<n>Sf`, `F<n>Ac`, `F<n>Tg`, the `FS!` force mask, real temperature keys,
  `ch8*` strings, `ui8`/`ui16`/`ui32` and a little-endian `flt`.
- A dead sensor reporting the SMC's `-127` sentinel, which must be filtered out.
- Fans that chase their target at a believable rate, and temperatures that drift,
  so the control loop has something to react to and the GUI has something to
  draw.

## Environment variables

| Variable | Effect |
|---|---|
| `FAKESMC_SWALLOW_WRITES=1` | Accepts writes and silently forgets them, so the write-path selftest's failure detection can be proven against the real binary rather than asserted. |
| `FAKESMC_STATIC=1` | Freezes fan speeds and temperatures, for deterministic output. |
| `FAKESMC_WRITE_LOG=<path>` | Appends every accepted write to a file. This is how a program that only has a window — the GUI — can be shown to be *actually driving the fans* rather than merely running. |

## Running the checks

```sh
# Needs mingw-w64 and wine; xvfb + x11-utils add the GUI checks.
tools/fake_smc/run_e2e.sh build-win
```

It checks, in order: that the tool finds the port-I/O path, that it reads the fan
count, labels, ranges and control style, that the `-127` sentinel is filtered and
sensor names resolve, that the non-destructive selftest confirms the write path,
that a **swallowed write is reported as a failure**, that the control loop writes
fan targets and hands the fans back on exit, that the CSV log appears, and that
the **main window opens and writes to the SMC**.

Exit status is non-zero if any check fails, so CI runs it on every push.

## Building it by hand

```sh
x86_64-w64-mingw32-gcc -O2 -shared -static-libgcc -o inpoutx64.dll inpout32_shim.c   # 64-bit
i686-w64-mingw32-gcc   -O2 -shared -static-libgcc -o inpout32.dll  inpout32_shim.c   # 32-bit
```

Then, on a Windows machine or under Wine, put the DLL next to the executable:

```sh
fanforge-cli.exe info      # should report: smc : port-io, fans : 2
fanforge-cli.exe selftest  # should report: the write path is confirmed working
```

## This is not a way to run FanForge on a non-Mac

The shim fakes the hardware. It does not make a PC into a Mac, and it is not
shipped in any release. It exists so that this project's own claims can be
checked by running the code, and so CI can do the same on every push.
