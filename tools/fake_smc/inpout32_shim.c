//
// A fake InpOut32: the Apple SMC's I/O-port interface, implemented in user
// space so that the *shipped Windows binaries* can be driven end to end on a
// machine that is not a Mac.
//
// This is the test harness that closes the last verification gap. Everything
// above the raw port access is the real code in the real .exe: the Win32 loader,
// the InpOut32/InpOutx64 discovery, the poll loop, the SMC transaction framing,
// the key decoding, the fan bank, the controller, and - with a display attached -
// the whole GUI. The only thing that is not real is the chip on the other side
// of ports 0x300/0x304, and this file is that chip.
//
// It reproduces the register state machine exactly as the Linux applesmc driver
// documents it, including the details that are easy to get wrong and that the
// transport in this project depends on:
//
//   * status bits: 0x01 = a byte is ready on the data port,
//                  0x02 = input is closed, 0x04 = busy
//   * a transaction is: command byte to 0x304, then four key bytes and a length
//     byte to 0x300, then either `length` bytes read back or `length` bytes
//     written
//   * a read returns *the whole key* regardless of the requested length, so the
//     transport's trailing flush loop is genuinely exercised
//
// Environment variables, all read once at load:
//   FAKESMC_SWALLOW_WRITES=1  accepts writes and silently forgets them. This is
//                             how the write-path selftest's failure detection is
//                             proven against the real binary.
//   FAKESMC_STATIC=1          stops the fan speeds and temperatures moving, for
//                             deterministic output.
//   FAKESMC_WRITE_LOG=<path>  appends every accepted write to a file. This is
//                             how a program that only has a window - the GUI -
//                             can be shown to be actually driving the fans
//                             rather than merely running.
//
// Build (see tools/fake_smc/README.md):
//   x86_64-w64-mingw32-gcc -O2 -shared -static -o inpoutx64.dll inpout32_shim.c
//
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA_PORT 0x300
#define CMD_PORT 0x304

#define STATUS_AWAITING_DATA 0x01
#define STATUS_IB_CLOSED 0x02
#define STATUS_BUSY 0x04

#define CMD_READ 0x10
#define CMD_WRITE 0x11
#define CMD_KEY_BY_INDEX 0x12
#define CMD_KEY_TYPE 0x13

#define ATTR_READABLE 0x80
#define ATTR_WRITABLE 0x40

#define MAX_KEYS 64
#define MAX_VALUE 32
#define MAX_FANS 4

typedef struct {
    unsigned char key[4];
    unsigned char type[4];
    unsigned char attrs;
    unsigned char size;
    unsigned char data[MAX_VALUE];
} SmcKey;

static SmcKey g_keys[MAX_KEYS];
static int g_keyCount = 0;
static DWORD g_keyCountKey = 0;  /* index of "#KEY", so it can be refreshed */

/* One transaction's worth of state, exactly as the chip holds it. */
static unsigned char g_command = 0;
static unsigned char g_length = 0;
static unsigned char g_input[64];
static int g_inputLen = 0;
static unsigned char g_output[64];
static int g_outputLen = 0;
static int g_outputPos = 0;
static int g_awaiting = 0;
static int g_busy = 0;

static int g_swallowWrites = 0;
static int g_static = 1;
static char g_writeLogPath[MAX_PATH] = {0};

static void logWrite(const unsigned char key[4], const unsigned char* data, int length) {
    if (g_writeLogPath[0] == 0) return;

    FILE* file = fopen(g_writeLogPath, "a");
    if (!file) return;

    char name[5];
    memcpy(name, key, 4);
    name[4] = 0;
    fprintf(file, "%s", name);
    for (int i = 0; i < length; ++i) fprintf(file, " %02X", data[i]);
    fprintf(file, "\n");
    fclose(file);
}

/* Dynamic state, so the GUI has something worth drawing. */
static double g_fanRpm[MAX_FANS] = {2160.0, 2000.0};
static DWORD g_lastTick = 0;

/* --------------------------------------------------------------------------- */
/* key table                                                                    */
/* --------------------------------------------------------------------------- */

static void putBE(unsigned char* out, int width, unsigned long long value) {
    for (int i = width - 1; i >= 0; --i) {
        out[i] = (unsigned char)(value & 0xFF);
        value >>= 8;
    }
}

static unsigned long long getBE(const unsigned char* in, int width) {
    unsigned long long v = 0;
    for (int i = 0; i < width; ++i) v = (v << 8) | in[i];
    return v;
}

static void addKey(const char* key, const char* type, unsigned char attrs,
                   const unsigned char* data, unsigned char size) {
    if (g_keyCount >= MAX_KEYS) return;
    SmcKey* k = &g_keys[g_keyCount];
    ++g_keyCount;
    memset(k, 0, sizeof *k);
    memcpy(k->key, key, 4);   /* exactly four bytes; callers space-pad */
    memcpy(k->type, type, 4);
    k->attrs = attrs;
    k->size = size;
    if (data && size) memcpy(k->data, data, size);
}

static void addU8(const char* key, unsigned char v) {
    addKey(key, "ui8 ", ATTR_READABLE, &v, 1);
}

static void addU16(const char* key, unsigned short v, unsigned char attrs, unsigned char mode) {
    unsigned char b[2];
    putBE(b, 2, v);
    addKey(key, mode ? "fpe2" : "ui16", attrs, b, 2);
}

static void addU32(const char* key, unsigned int v) {
    unsigned char b[4];
    putBE(b, 4, v);
    addKey(key, "ui32", ATTR_READABLE, b, 4);
}

static void addFpe2(const char* key, double rpm, unsigned char attrs) {
    addU16(key, (unsigned short)(rpm * 4.0 + 0.5), attrs, 1);
}

static void addSp78(const char* key, double celsius) {
    long scaled = (long)(celsius * 256.0 + (celsius < 0 ? -0.5 : 0.5));
    unsigned char b[2];
    putBE(b, 2, (unsigned long long)(unsigned short)(short)scaled);
    addKey(key, "sp78", ATTR_READABLE, b, 2);
}

static void addString(const char* key, const char* text, unsigned char size) {
    unsigned char b[MAX_VALUE];
    memset(b, ' ', size);
    for (int i = 0; i < size && text[i]; ++i) b[i] = (unsigned char)text[i];
    addKey(key, "ch8*", ATTR_READABLE, b, size);
}

static void addFan(int index, const char* label, double mn, double mx, double sf, double now) {
    char name[8];
    char id[8];
    char suf[8];
    _snprintf(name, sizeof name, "F%d", index);
    _snprintf(id, sizeof id, "F%dID", index);
    memcpy(suf, name, 2);

    addString(id, label, 16);

    suf[2] = 'M'; suf[3] = 'n'; suf[4] = 0;
    addFpe2(suf, mn, ATTR_READABLE);
    suf[2] = 'M'; suf[3] = 'x';
    addFpe2(suf, mx, ATTR_READABLE);
    suf[2] = 'S'; suf[3] = 'f';
    addFpe2(suf, sf, ATTR_READABLE);
    suf[2] = 'A'; suf[3] = 'c';
    addFpe2(suf, now, ATTR_READABLE);
    suf[2] = 'T'; suf[3] = 'g';
    /* The target is the one the controller writes. */
    addFpe2(suf, now, ATTR_READABLE | ATTR_WRITABLE);
}

static int compareKeys(const void* a, const void* b) {
    return memcmp(((const SmcKey*)a)->key, ((const SmcKey*)b)->key, 4);
}

static void buildMachine(void) {
    g_keyCount = 0;

    addU8("FNum", 2);
    addFan(0, "Left side", 2160.0, 5927.0, 3500.0, 2160.0);
    addFan(1, "Right side", 2000.0, 5489.0, 3300.0, 2000.0);

    /* The firmware's manual-force bitmask: one bit per fan. */
    addU16("FS! ", 0, ATTR_READABLE | ATTR_WRITABLE, 0);

    /* A believable thermal picture. The hottest is meant to be the CPU. */
    addSp78("TC0P", 52.0);
    addSp78("TC0D", 55.0);
    addSp78("TC1C", 54.0);
    addSp78("TC2C", 56.0);
    addSp78("TG0D", 48.0);
    addSp78("TG0P", 46.0);
    addSp78("TB0T", 31.0);
    addSp78("TA0P", 28.0);

    /* A dead sensor, reporting the sentinel a real SMC uses. It must be
       filtered out rather than shown as -127. */
    addSp78("TP0P", -127.0);

    /* Non-temperature keys, so every decoder path is exercised. */
    addString("REV ", "1.30", 4);
    addU16("VC0C", 12500, ATTR_READABLE, 0);   /* millivolts */
    addU32("#KEY", 0);
    g_keyCountKey = (DWORD)(g_keyCount - 1);

    qsort(g_keys, (size_t)g_keyCount, sizeof(SmcKey), compareKeys);

    /* #KEY counts the keys this table actually yields. */
    for (int i = 0; i < g_keyCount; ++i) {
        if (memcmp(g_keys[i].key, "#KEY", 4) == 0) {
            putBE(g_keys[i].data, 4, (unsigned long long)g_keyCount);
            g_keyCountKey = (DWORD)i;
            break;
        }
    }
    (void)g_keyCountKey;
}

static SmcKey* findKey(const unsigned char key[4]) {
    for (int i = 0; i < g_keyCount; ++i) {
        if (memcmp(g_keys[i].key, key, 4) == 0) return &g_keys[i];
    }
    return NULL;
}

/* --------------------------------------------------------------------------- */
/* a machine that is doing something                                            */
/* --------------------------------------------------------------------------- */

static double decodeFpe2(const unsigned char* b) {
    return (double)getBE(b, 2) / 4.0;
}

static void updateDynamics(void) {
    if (g_static) return;

    const DWORD now = GetTickCount();
    if (g_lastTick == 0) {
        g_lastTick = now;
        return;
    }
    double dt = (double)(now - g_lastTick) / 1000.0;
    g_lastTick = now;
    if (dt <= 0.0) return;
    if (dt > 2.0) dt = 2.0;

    /* Fans chase their target the way a real fan does: fast to speed up, slower
       to wind down, and never outside the range the SMC itself reports. A
       curve that asks for 2000 RPM does not produce 2000 RPM instantly, and the
       GUI's "red dot" is supposed to show exactly that lag. */
    for (int i = 0; i < 2; ++i) {
        char name[8];
        char target[8];
        _snprintf(name, sizeof name, "F%dAc", i);
        _snprintf(target, sizeof target, "F%dTg", i);

        SmcKey* ac = findKey((const unsigned char*)name);
        SmcKey* tg = findKey((const unsigned char*)target);
        if (!ac || !tg) continue;

        const double want = decodeFpe2(tg->data);
        double have = g_fanRpm[i];
        if (have <= 0.0) have = want;

        const double rate = (want > have) ? 1400.0 : 700.0;  /* RPM per second */
        const double step = rate * dt;
        double delta = want - have;
        if (delta > step) delta = step;
        if (delta < -step) delta = -step;
        have += delta;

        g_fanRpm[i] = have;
        putBE(ac->data, 2, (unsigned long long)(unsigned int)(have * 4.0 + 0.5));
    }

    /* Temperatures drift, so the curve has something to react to. */
    const double seconds = (double)now / 1000.0;
    struct { const char* key; double base; double amplitude; double period; } drift[] = {
        {"TC0P", 52.0, 3.0, 17.0},
        {"TC0D", 55.0, 4.0, 13.0},
        {"TC1C", 54.0, 3.5, 11.0},
        {"TC2C", 56.0, 4.5, 19.0},
        {"TG0D", 48.0, 2.0, 23.0},
        {"TG0P", 46.0, 1.5, 29.0},
    };
    for (size_t i = 0; i < sizeof drift / sizeof drift[0]; ++i) {
        SmcKey* k = findKey((const unsigned char*)drift[i].key);
        if (!k) continue;
        const double value =
            drift[i].base + drift[i].amplitude * sin(2.0 * 3.14159265358979 * seconds / drift[i].period);
        long scaled = (long)(value * 256.0 + 0.5);
        putBE(k->data, 2, (unsigned long long)(unsigned short)(short)scaled);
    }
}

/* --------------------------------------------------------------------------- */
/* the register state machine                                                   */
/* --------------------------------------------------------------------------- */

static unsigned char statusByte(void) {
    unsigned char s = 0;
    if (g_awaiting) s |= STATUS_AWAITING_DATA;
    if (g_busy) s |= STATUS_BUSY;
    return s;
}

static void beginCommand(unsigned char command) {
    g_command = command;
    g_inputLen = 0;
    g_outputLen = 0;
    g_outputPos = 0;
    g_awaiting = 0;
    g_busy = 1;
}

static void finishRequest(void) {
    g_awaiting = 0;
    g_busy = 0;
    g_outputLen = 0;
    g_outputPos = 0;
}

static void startRequest(unsigned char length) {
    g_length = length;
    const unsigned char* arg = g_input;   /* the four key bytes */

    switch (g_command) {
        case CMD_READ: {
            SmcKey* k = findKey(arg);
            if (k && k->size > 0) {
                /* A real SMC returns the whole key, not `length` bytes. The
                   transport's flush loop exists because of this. */
                memcpy(g_output, k->data, k->size);
                g_outputLen = k->size;
                g_outputPos = 0;
                g_awaiting = 1;
                g_busy = 1;
            } else {
                finishRequest();
            }
            return;
        }
        case CMD_KEY_TYPE: {
            SmcKey* k = findKey(arg);
            if (k) {
                g_output[0] = k->size;
                memcpy(g_output + 1, k->type, 4);
                g_output[5] = k->attrs;
                g_outputLen = 6;
                g_outputPos = 0;
                g_awaiting = 1;
                g_busy = 1;
            } else {
                finishRequest();
            }
            return;
        }
        case CMD_KEY_BY_INDEX: {
            const unsigned int index = (unsigned int)getBE(arg, 4);
            if (index < (unsigned int)g_keyCount) {
                memcpy(g_output, g_keys[index].key, 4);
                g_outputLen = 4;
                g_outputPos = 0;
                g_awaiting = 1;
                g_busy = 1;
            } else {
                finishRequest();
            }
            return;
        }
        case CMD_WRITE:
            /* Hold the controller busy while the payload arrives. */
            g_awaiting = 0;
            g_busy = (length == 0) ? 0 : 1;
            return;
        default:
            finishRequest();
            return;
    }
}

static void consumeInputByte(unsigned char value) {
    if (!g_busy) return;
    if (g_inputLen < (int)sizeof g_input) g_input[g_inputLen] = value;
    ++g_inputLen;

    if (g_inputLen == 5) {
        startRequest(g_input[4]);
        return;
    }
    if (g_command == CMD_WRITE && g_inputLen == 5 + (int)g_length) {
        if (!g_swallowWrites) {
            SmcKey* k = findKey(g_input);
            if (k && (k->attrs & ATTR_WRITABLE) && k->size == g_length) {
                memcpy(k->data, g_input + 5, g_length);
                logWrite(k->key, g_input + 5, g_length);
            }
        }
        finishRequest();
    }
}

static unsigned char ioIn(unsigned short port) {
    updateDynamics();

    if (port == CMD_PORT) return statusByte();
    if (port != DATA_PORT) return 0;

    if (g_outputPos < g_outputLen) {
        const unsigned char v = g_output[g_outputPos];
        ++g_outputPos;
        if (g_outputPos >= g_outputLen) {
            /* The last byte has been taken: the controller is idle again. */
            g_awaiting = 0;
            g_busy = 0;
        }
        return v;
    }
    return 0;
}

static void ioOut(unsigned short port, unsigned char value) {
    updateDynamics();

    if (port == CMD_PORT) {
        beginCommand(value);
    } else if (port == DATA_PORT) {
        consumeInputByte(value);
    }
}

/* --------------------------------------------------------------------------- */
/* the InpOut32 surface the project loads                                       */
/* --------------------------------------------------------------------------- */

__declspec(dllexport) unsigned char __stdcall DlPortReadPortUchar(unsigned long port) {
    return ioIn((unsigned short)port);
}

__declspec(dllexport) void __stdcall DlPortWritePortUchar(unsigned long port, unsigned char value) {
    ioOut((unsigned short)port, value);
}

__declspec(dllexport) unsigned short __stdcall DlPortReadPortUshort(unsigned long port) {
    return ioIn((unsigned short)port);
}

__declspec(dllexport) void __stdcall DlPortWritePortUshort(unsigned long port, unsigned short value) {
    ioOut((unsigned short)port, (unsigned char)value);
}

__declspec(dllexport) unsigned long __stdcall DlPortReadPortUlong(unsigned long port) {
    return ioIn((unsigned short)port);
}

__declspec(dllexport) void __stdcall DlPortWritePortUlong(unsigned long port, unsigned long value) {
    ioOut((unsigned short)port, (unsigned char)value);
}

/* The classic short-based entry points, which every build of the DLL exports. */
__declspec(dllexport) short __stdcall Inp32(short port) {
    return (short)ioIn((unsigned short)port);
}

__declspec(dllexport) void __stdcall Out32(short port, short value) {
    ioOut((unsigned short)port, (unsigned char)value);
}

__declspec(dllexport) BOOL __stdcall IsInpOutDriverOpen(void) {
    return TRUE;
}

__declspec(dllexport) void __stdcall DlPortIO(void) {}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH) {
        const char* writeLog = getenv("FAKESMC_WRITE_LOG");
        if (writeLog && *writeLog) {
            strncpy(g_writeLogPath, writeLog, sizeof g_writeLogPath - 1);
            g_writeLogPath[sizeof g_writeLogPath - 1] = 0;
        }
        g_swallowWrites = getenv("FAKESMC_SWALLOW_WRITES") != NULL;
        /* Moving by default: a frozen machine hides exactly the bugs this
           harness exists to find. */
        g_static = getenv("FAKESMC_STATIC") != NULL;
        buildMachine();
        g_lastTick = GetTickCount();
    }
    return TRUE;
}
