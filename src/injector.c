/* tsfp-syslink - System Link for TimeSplitters Future Perfect running in
 * Microsoft's Xbox emulator on PC (Emu.exe).
 *
 * 1. Waits for Emu.exe and, before the game checks, makes the emulator's
 *    virtual network card report "cable connected".
 * 2. Loads a small bridge into the emulator that tunnels the card's Ethernet
 *    frames over UDP (one frame per datagram), the format XLink Kai and xemu's
 *    "UDP Tunnel" use.
 * Memory only: nothing on disk is changed, closing the game undoes it all. */
#include "win.h"
#include "payload.h"   /* generated: g_payload[], PAYLOAD_INIT_OFFSET, PAYLOAD_LOOP_OFFSET */

#define VERSION "0.1.0"

/* compilers may emit calls to these even with -fno-builtin */
void *memset(void *d, int c, u64 n) { volatile u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, u64 n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }

/* ------------------------------------------------------------------ output */
static HANDLE g_out;

static u32 slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

static void out(const char *s)
{
    DWORD w;
    WriteFile(g_out, s, slen(s), &w, 0);
}

static char *hex64(char *buf, u64 v)
{
    static const char d[] = "0123456789ABCDEF";
    int i, started = 0, n = 0;
    for (i = 60; i >= 0; i -= 4) {
        int x = (int)((v >> i) & 0xF);
        if (x || started || i == 0) { buf[n++] = d[x]; started = 1; }
    }
    buf[n] = 0;
    return buf;
}

#define say(...) do { char _b[512]; wsprintfA(_b, __VA_ARGS__); out(_b); } while (0)

static void mzero(void *p, u64 n) { volatile u8 *b = p; while (n--) *b++ = 0; }
static void mcopy(void *d, const void *s, u64 n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; }
static void scopy(char *d, const char *s, u32 cap) { u32 i = 0; while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; } d[i] = 0; }

static int ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

/* -------------------------------------------------------------------- config */
typedef struct {
    char host[64];
    u32  ip;
    u32  port;
    u32  localPort;
    int  launch;
    char app[256];
} Config;

static u32 atou(const char *s) { u32 v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (u32)(*s++ - '0'); return v; }

static int parse_ip(const char *s, u32 *ip)
{
    u32 part[4], i;
    for (i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return 0;
        part[i] = 0;
        while (*s >= '0' && *s <= '9') part[i] = part[i] * 10 + (u32)(*s++ - '0');
        if (part[i] > 255) return 0;
        if (i < 3) { if (*s != '.') return 0; s++; }
    }
    if (*s) return 0;
    *ip = part[0] | (part[1] << 8) | (part[2] << 16) | (part[3] << 24);
    return 1;
}

static const char INI_TEMPLATE[] =
"; tsfp-syslink settings\r\n"
";\r\n"
"; host/port: where the game's System Link traffic is sent.\r\n"
";   XLink Kai on this PC ...... host=127.0.0.1  port=34523   (default)\r\n"
";   xemu on this PC ........... host=127.0.0.1  port=9368\r\n"
";     (xemu: Network -> UDP Tunnel, bind 127.0.0.1:9368, remote 127.0.0.1:9369)\r\n"
";   XLink Kai on another PC ... host=<its LAN IP> port=34523\r\n"
";     (enable 'Allow Remote DDS Connections' in XLink Kai)\r\n"
"; local_port: UDP port this tool listens on.\r\n"
"; launch: 1 = start the game from the Xbox app automatically.\r\n"
"\r\n"
"[relay]\r\n"
"host=127.0.0.1\r\n"
"port=34523\r\n"
"local_port=9369\r\n"
"\r\n"
"[game]\r\n"
"launch=1\r\n"
"app=Xbox360BackwardCompatibil.PrimaryTimeSplittersFutu_ksqcvrsvwz2jp!App\r\n";

static void load_config(Config *c, char *iniPath)
{
    char exe[520], buf[256];
    u32 n = GetModuleFileNameA(0, exe, sizeof(exe)), i;
    for (i = n; i > 0 && exe[i - 1] != '\\'; i--) ;
    mcopy(iniPath, exe, i);
    scopy(iniPath + i, "tsfp-syslink.ini", 64);

    if (GetFileAttributesA(iniPath) == 0xFFFFFFFFu) {
        HANDLE h = CreateFileA(iniPath, 0x40000000 /* GENERIC_WRITE */, 0, 0, 1 /* CREATE_NEW */, 0x80, 0);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(h, INI_TEMPLATE, slen(INI_TEMPLATE), &w, 0);
            CloseHandle(h);
            say("Created %s with default settings.\r\n", iniPath);
        }
    }

    GetPrivateProfileStringA("relay", "host", "127.0.0.1", c->host, sizeof(c->host), iniPath);
    GetPrivateProfileStringA("relay", "port", "34523", buf, sizeof(buf), iniPath);  c->port = atou(buf);
    GetPrivateProfileStringA("relay", "local_port", "9369", buf, sizeof(buf), iniPath); c->localPort = atou(buf);
    GetPrivateProfileStringA("game", "launch", "1", buf, sizeof(buf), iniPath); c->launch = atou(buf) != 0;
    GetPrivateProfileStringA("game", "app", "Xbox360BackwardCompatibil.PrimaryTimeSplittersFutu_ksqcvrsvwz2jp!App", c->app, sizeof(c->app), iniPath);
    if (!parse_ip(c->host, &c->ip)) { say("Invalid host '%s' in the ini; using 127.0.0.1\r\n", c->host); parse_ip("127.0.0.1", &c->ip); scopy(c->host, "127.0.0.1", 64); }
    if (!c->port || c->port > 65535) c->port = 34523;
    if (!c->localPort || c->localPort > 65535) c->localPort = 9369;
}

/* ------------------------------------------------------------------ process */
static u32 find_process(const char *name, u32 skip)
{
    PROCESSENTRY32 pe;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    u32 pid = 0;
    if (s == INVALID_HANDLE_VALUE) return 0;
    pe.dwSize = sizeof(pe);
    if (Process32First(s, &pe)) do {
        if (ieq(pe.szExeFile, name) && pe.th32ProcessID != skip) { pid = pe.th32ProcessID; break; }
    } while (Process32Next(s, &pe));
    CloseHandle(s);
    return pid;
}

static int find_module(u32 pid, const char *name, u64 *base, u32 *size)
{
    MODULEENTRY32 me;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    int ok = 0;
    if (s == INVALID_HANDLE_VALUE) return 0;
    me.dwSize = sizeof(me);
    if (Module32First(s, &me)) do {
        if (ieq(me.szModule, name)) { *base = (u64)me.modBaseAddr; *size = me.modBaseSize; ok = 1; break; }
    } while (Module32Next(s, &me));
    CloseHandle(s);
    return ok;
}

static int rd(HANDLE p, u64 a, void *b, u64 n) { SIZE_T r = 0; return ReadProcessMemory(p, (void *)a, b, n, &r) && r == n; }

static int wr(HANDLE p, u64 a, const void *b, u64 n)
{
    SIZE_T w = 0;
    DWORD old = 0, tmp;
    int ok;
    if (WriteProcessMemory(p, (void *)a, b, n, &w) && w == n) return 1;
    if (!VirtualProtectEx(p, (void *)a, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    ok = WriteProcessMemory(p, (void *)a, b, n, &w) && w == n;
    VirtualProtectEx(p, (void *)a, n, old, &tmp);
    FlushInstructionCache(p, (void *)a, n);
    return ok;
}

/* Write code while every thread of the process is suspended and none of them
 * is part-way through the bytes being replaced. */
static int wr_code_safely(HANDLE proc, u32 pid, u64 addr, const void *bytes, u32 n)
{
    int attempt;
    for (attempt = 0; attempt < 50; attempt++) {
        static HANDLE th[512];
        u32 count = 0, i;
        int clash = 0, ok;
        THREADENTRY32 te;
        HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        static u8 ctxbuf[1232 + 16];
        u8 *ctx = (u8 *)(((u64)ctxbuf + 15) & ~(u64)15);

        if (s == INVALID_HANDLE_VALUE) return 0;
        te.dwSize = sizeof(te);
        if (Thread32First(s, &te)) do {
            if (te.th32OwnerProcessID == pid && count < 512) {
                HANDLE t = OpenThread(THREAD_ACCESS, 0, te.th32ThreadID);
                if (t) { SuspendThread(t); th[count++] = t; }
            }
        } while (Thread32Next(s, &te));
        CloseHandle(s);

        for (i = 0; i < count && !clash; i++) {
            u64 rip;
            mzero(ctx, 1232);
            *(u32 *)(ctx + 0x30) = 0x00100001;  /* CONTEXT_CONTROL */
            if (GetThreadContext(th[i], ctx)) {
                rip = *(u64 *)(ctx + 0xF8);
                if (rip > addr && rip < addr + n) clash = 1;
            }
        }
        ok = clash ? 0 : wr(proc, addr, bytes, n);
        for (i = 0; i < count; i++) { ResumeThread(th[i]); CloseHandle(th[i]); }
        if (!clash) return ok;
        Sleep(1);
    }
    return 0;
}

/* -------------------------------------------------------------------- scan */
typedef struct { const char *name; const char *pat; } Pattern;

/* "??" = wildcard */
static int parse_pattern(const char *s, u8 *bytes, u8 *mask)
{
    int n = 0;
    while (*s) {
        if (*s == ' ') { s++; continue; }
        if (s[0] == '?') { bytes[n] = 0; mask[n] = 0; n++; s += (s[1] == '?') ? 2 : 1; continue; }
        {
            int hi = s[0] <= '9' ? s[0] - '0' : (s[0] | 32) - 'a' + 10;
            int lo = s[1] <= '9' ? s[1] - '0' : (s[1] | 32) - 'a' + 10;
            bytes[n] = (u8)(hi * 16 + lo); mask[n] = 1; n++; s += 2;
        }
    }
    return n;
}

/* returns offset of the only match, -1 if none, -2 if more than one */
static long long scan_unique(const u8 *img, u32 size, const char *pat)
{
    u8 b[128], m[128];
    int n = parse_pattern(pat, b, m), j;
    long long found = -1;
    u32 i;
    for (i = 0; i + (u32)n <= size; i++) {
        if (img[i] != b[0]) continue;
        for (j = 1; j < n; j++) if (m[j] && img[i + j] != b[j]) break;
        if (j == n) { if (found >= 0) return -2; found = i; }
    }
    return found;
}

static long long rel32_target(const u8 *img, u64 instrEnd, u64 dispOffset)
{
    int d = *(const int *)(img + dispOffset);
    return (long long)instrEnd + d;
}

/* ------------------------------------------------------------------ targets */
typedef struct {
    u64 linkStateImm;   /* the 0x20 immediate in GetLinkState */
    u64 nicFlags;       /* _DeviceReady, _PhysicalLinkConnected */
    u64 nic, nic2;
    u64 raiseRx, raiseTx;
    u64 irq;            /* function that raises guest interrupts (hook site) */
    u64 irqJnsTarget;
} Targets;

static const char P_LINKSTATE[] = "40 53 48 83 EC 20 8B 41 28 BB ?? 00 00 00 85 C0 74 0C 83 79 2C 00 BA 24 00 00 00 0F 45 DA";
static const char P_RAISE_RX[]  = "0F 0D 49 10 8B 41 10 66 0F 1F 84 00 00 00 00 00 8B D0 83 CA 40 F0 0F B1 51 10 75 F4 85 C0 75 0B 8B 0D ?? ?? ?? ?? E9";
static const char P_RAISE_TX[]  = "0F 0D 49 10 8B 41 10 66 0F 1F 84 00 00 00 00 00 8B D0 83 CA 14 F0 0F B1 51 10 75 F4 85 C0 75 0B 8B 0D ?? ?? ?? ?? E9";
static const char P_RX_LAYOUT[] = "0F B7 49 34 48 8D 57 36 41 8B 00 3B C1";
static const char P_RX_EVENT[]  = "89 1E 48 8B 4F 18 FF 15";
static const char P_TX_LAYOUT[] = "83 79 28 00 4C 8D 51 10 74 ?? 41 83 7A 1C 00 74 ?? 41 8B 82 B0 0A 00 00 48 83 C0 4A";
static const char P_TX_EVENT[]  = "45 87 82 B0 0A 00 00 49 8B 8A B8 0A 00 00";
static const char P_NIC_INIT[]  = "FF 50 30 48 8D 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 80 3D";
static const char P_IRQ[]       = "8B C1 85 C9 0F 89 ?? ?? ?? ?? C1 E9 08 0F B6 C9 0F BD D1 0F 84";

static int find_targets(HANDLE proc, u64 base, u32 size, Targets *t)
{
    u8 *img = (u8 *)VirtualAlloc(0, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    u32 off;
    long long o;
    int ok = 1;
    if (!img) return 0;
    for (off = 0; off < size; off += 0x1000) {
        u32 n = size - off < 0x1000 ? size - off : 0x1000;
        if (!rd(proc, base + off, img + off, n)) mzero(img + off, n);
    }

#define NEED(var, pat, label) \
    o = scan_unique(img, size, pat); \
    if (o < 0) { say("  pattern %s: %s\r\n", label, o == -1 ? "not found" : "found more than once"); ok = 0; } \
    var = o;

    {
        long long ls, rx, tx, rxl, rxe, txl, txe, ni, iq;
        NEED(ls, P_LINKSTATE, "link-state");
        NEED(rx, P_RAISE_RX, "raise-rx");
        NEED(tx, P_RAISE_TX, "raise-tx");
        NEED(rxl, P_RX_LAYOUT, "rx-layout");
        NEED(rxe, P_RX_EVENT, "rx-event");
        NEED(txl, P_TX_LAYOUT, "tx-layout");
        NEED(txe, P_TX_EVENT, "tx-event");
        NEED(ni, P_NIC_INIT, "nic-object");
        NEED(iq, P_IRQ, "irq");
        (void)rxl; (void)rxe; (void)txl; (void)txe;   /* layout checks only */
        if (ok) {
            u64 irqFromRx, irqFromTx, vt, vt2, slot;
            t->linkStateImm = base + (u64)ls + 10;
            t->raiseRx = base + (u64)rx;
            t->raiseTx = base + (u64)tx;
            t->irq = base + (u64)iq;
            t->irqJnsTarget = base + (u64)rel32_target(img, (u64)iq + 10, (u64)iq + 6);
            t->nic = base + (u64)rel32_target(img, (u64)ni + 10, (u64)ni + 6);
            t->nic2 = t->nic - 8;
            t->nicFlags = t->nic + 0x28;

            /* consistency checks against the live object */
            irqFromRx = base + (u64)rel32_target(img, (u64)rx + 43, (u64)rx + 39);
            irqFromTx = base + (u64)rel32_target(img, (u64)tx + 43, (u64)tx + 39);
            if (irqFromRx != t->irq || irqFromTx != t->irq) { say("  check failed: interrupt helpers do not call the hooked function\r\n"); ok = 0; }
            if (ok && rd(proc, t->nic, &vt, 8) && rd(proc, vt + 16, &slot, 8)) {
                if (slot != base + (u64)ls) { say("  check failed: NIC vtable does not point to the link-state function\r\n"); ok = 0; }
            } else if (ok) { say("  check failed: cannot read the NIC object\r\n"); ok = 0; }
            if (ok && rd(proc, t->nic2, &vt2, 8)) {
                u64 s1 = 0, s2 = 0;
                rd(proc, vt2 + 8, &s1, 8); rd(proc, vt2 + 16, &s2, 8);
                if (s1 != t->raiseRx || s2 != t->raiseTx) { say("  check failed: interrupt helpers are not the NIC's\r\n"); ok = 0; }
            }
        }
    }
#undef NEED
    VirtualFree(img, 0, MEM_RELEASE);
    return ok;
}

/* ------------------------------------------------------------- injection */
static u64 alloc_near(HANDLE proc, u64 near, u32 size)
{
    u64 step = 0x10000, a;
    u32 i;
    for (i = 1; i < 0x7000; i++) {
        a = (near & ~(step - 1)) - (u64)i * step;
        if (VirtualAllocEx(proc, (void *)a, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE) == (void *)a) return a;
        a = (near & ~(step - 1)) + 0x2000000 + (u64)i * step;
        if (VirtualAllocEx(proc, (void *)a, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE) == (void *)a) return a;
    }
    return 0;
}

static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void put64(u8 *p, u64 v) { put32(p, (u32)v); put32(p + 4, (u32)(v >> 32)); }

#define STUB_SIZE 96
#define OFF_CTX   0x100
#define OFF_CODE  0x400

/* hook stub layout, see README "How it works" */
static void build_stub(u8 *s, u64 stubAddr, u64 ctxAddr, u64 initAddr, const Targets *t)
{
    static const u8 tmpl[] = {
        0x80, 0x3D, 0, 0, 0, 0, 0x00,             /*  0 cmp byte [rip+flag],0  */
        0x75, 0,                                  /*  7 jne orig               */
        0xC6, 0x05, 0, 0, 0, 0, 0x01,             /*  9 mov byte [rip+flag],1  */
        0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, /* 16 push rax..r11 */
        0x48, 0x83, 0xEC, 0x20,                   /* 27 sub rsp,20h            */
        0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,       /* 31 mov rcx,ctx            */
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,       /* 41 mov rax,bridge_init    */
        0xFF, 0xD0,                               /* 51 call rax               */
        0x48, 0x83, 0xC4, 0x20,                   /* 53 add rsp,20h            */
        0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58, /* 57 pop r11..rax */
        0x8B, 0xC1, 0x85, 0xC9,                   /* 68 orig: mov eax,ecx; test ecx,ecx */
        0x0F, 0x89, 0, 0, 0, 0,                   /* 72 jns original target    */
        0xE9, 0, 0, 0, 0,                         /* 78 jmp irq+10             */
        0x00                                      /* 83 flag                   */
    };
    u64 flag = stubAddr + 83;
    mzero(s, STUB_SIZE);
    mcopy(s, tmpl, sizeof(tmpl));
    put32(s + 2, (u32)(flag - (stubAddr + 7)));
    s[8] = (u8)(68 - 9);
    put32(s + 11, (u32)(flag - (stubAddr + 16)));
    put64(s + 33, ctxAddr);
    put64(s + 43, initAddr);
    put32(s + 74, (u32)(t->irqJnsTarget - (stubAddr + 78)));
    put32(s + 79, (u32)((t->irq + 10) - (stubAddr + 83)));
}

static void fill_ctx(BridgeCtx *c, const Targets *t, const Config *cfg, u64 codeAddr)
{
    HANDLE k32 = GetModuleHandleA("kernel32.dll");
    mzero(c, sizeof(*c));
    c->magic = BRIDGE_MAGIC;
    c->size = sizeof(*c);
    c->fnLoadLibraryA = (u64)GetProcAddress(k32, "LoadLibraryA");
    c->fnGetProcAddress = (u64)GetProcAddress(k32, "GetProcAddress");
    c->fnCreateEventA = (u64)GetProcAddress(k32, "CreateEventA");
    c->fnCreateThread = (u64)GetProcAddress(k32, "CreateThread");
    c->fnWaitForMultipleObjects = (u64)GetProcAddress(k32, "WaitForMultipleObjects");
    c->nic = t->nic;
    c->nic2 = t->nic2;
    c->raiseRx = t->raiseRx;
    c->raiseTx = t->raiseTx;
    c->bridgeLoop = codeAddr + PAYLOAD_LOOP_OFFSET;
    c->offRxLen = 0x34;
    c->offRxBuf = 0x36;
    c->offRxEvent = 0x18;
    c->offTxRing = 0x700;
    c->offTxProd = 0xAC0;
    c->offTxEvent = 0xAC8;
    c->txRingSize = 40;
    c->txStatusDone = 0;
    c->localIp = ((cfg->ip & 0xFF) == 127) ? 0x0100007F : 0;   /* loopback, or all interfaces for a remote relay */
    c->localPort = cfg->localPort;
    c->remoteIp = cfg->ip;
    c->remotePort = cfg->port;
    scopy(c->sWs2, "ws2_32.dll", 16);
    scopy(c->sWSAStartup, "WSAStartup", 16);
    scopy(c->sSocket, "socket", 8);
    scopy(c->sBind, "bind", 8);
    scopy(c->sSendto, "sendto", 8);
    scopy(c->sRecvfrom, "recvfrom", 16);
    scopy(c->sIoctlsocket, "ioctlsocket", 16);
    scopy(c->sWSAGetLastError, "WSAGetLastError", 24);
}

/* ------------------------------------------------------------------- main */
static int alive(HANDLE p) { DWORD c = 0; return GetExitCodeProcess(p, &c) && c == STILL_ACTIVE; }

static void pause_exit(int code)
{
    char b[8]; DWORD r;
    out("\r\nPress Enter to close.\r\n");
    ReadConsoleA(GetStdHandle((DWORD)-10), b, sizeof(b), &r, 0);
    ExitProcess((DWORD)code);
}

void mainCRTStartup(void)
{
    Config cfg;
    Targets t;
    char ini[600], h1[24], h2[24];
    u32 pid = 0, existing, size = 0;
    u64 base = 0;
    HANDLE proc;
    DWORD t0;
    int patched = 0;

    g_out = GetStdHandle((DWORD)-11);
    SetConsoleTitleA("tsfp-syslink");
    out("tsfp-syslink " VERSION " - System Link for TimeSplitters Future Perfect (Xbox app)\r\n\r\n");
    mzero(&cfg, sizeof(cfg));
    load_config(&cfg, ini);
    say("Relay: %s:%u%s, listening on UDP %u\r\n", cfg.host, cfg.port, cfg.port == 34523 ? " (XLink Kai)" : "", cfg.localPort);

    existing = find_process("Emu.exe", 0);
    if (existing) {
        out("\r\nThe game is already running. The patch has to be applied while it starts,\r\n"
            "so close TimeSplitters Future Perfect and run this tool again.\r\n");
        pause_exit(1);
    }

    if (cfg.launch) {
        char cmd[400];
        u8 si[104], pi[24];
        mzero(si, sizeof(si)); mzero(pi, sizeof(pi));
        *(u32 *)si = sizeof(si);
        wsprintfA(cmd, "explorer.exe shell:AppsFolder\\%s", cfg.app);
        if (CreateProcessA(0, cmd, 0, 0, 0, 0, 0, 0, si, pi)) {
            out("Starting the game...\r\n");
            CloseHandle(*(HANDLE *)pi); CloseHandle(*(HANDLE *)(pi + 8));
        } else out("Could not start the game automatically; launch it from the Xbox app.\r\n");
    } else out("Launch TimeSplitters Future Perfect from the Xbox app now.\r\n");

    out("Waiting for the emulator (Emu.exe)...\r\n");
    while (!(pid = find_process("Emu.exe", 0))) Sleep(10);
    proc = OpenProcess(PROCESS_ACCESS, 0, pid);
    if (!proc) { say("Cannot open Emu.exe (error %u). Try running this tool as administrator.\r\n", GetLastError()); pause_exit(1); }
    t0 = GetTickCount();
    while (!find_module(pid, "Emu.exe", &base, &size)) {
        if (!alive(proc) || GetTickCount() - t0 > 20000) { out("Could not read the emulator's modules.\r\n"); pause_exit(1); }
        Sleep(5);
    }
    say("Found Emu.exe (pid %u) at %s\r\n", pid, hex64(h1, base));

    if (!find_targets(proc, base, size, &t)) {
        out("\r\nThis version of the emulator is not supported (see messages above).\r\n"
            "Nothing was changed. Please report the Xbox app game version.\r\n");
        pause_exit(1);
    }

    /* 1. cable connected, as early as possible */
    {
        u8 imm = 0, v = 0x24;
        u32 one[2] = {1, 1};
        rd(proc, t.linkStateImm, &imm, 1);
        if (imm == 0x20 || imm == 0x24) {
            if (imm == 0x20) patched = wr(proc, t.linkStateImm, &v, 1); else patched = 1;
            wr(proc, t.nicFlags, one, 8);
        }
        if (!patched) { out("Failed to patch the network card.\r\n"); pause_exit(1); }
        out("Network card patched: cable connected.\r\n");
    }

    /* 2. bridge */
    {
        u64 mem = alloc_near(proc, t.irq, 0x2000), ctxAddr, codeAddr;
        BridgeCtx ctx;
        u8 stub[STUB_SIZE], jmp[10], orig[10];
        u32 waited = 0;
        if (!mem) { out("Could not allocate memory in the emulator.\r\n"); pause_exit(1); }
        ctxAddr = mem + OFF_CTX;
        codeAddr = mem + OFF_CODE;
        fill_ctx(&ctx, &t, &cfg, codeAddr);
        build_stub(stub, mem, ctxAddr, codeAddr + PAYLOAD_INIT_OFFSET, &t);
        if (!wr(proc, codeAddr, g_payload, sizeof(g_payload)) || !wr(proc, ctxAddr, &ctx, sizeof(ctx)) || !wr(proc, mem, stub, STUB_SIZE)) {
            out("Could not write the bridge.\r\n"); pause_exit(1);
        }
        rd(proc, t.irq, orig, 10);
        jmp[0] = 0xE9; put32(jmp + 1, (u32)(mem - (t.irq + 5)));
        jmp[5] = 0x0F; jmp[6] = 0x1F; jmp[7] = 0x44; jmp[8] = 0x00; jmp[9] = 0x00; /* 5-byte nop */

        /* Keep the card's flags set while the emulator boots, and only start the
           bridge once the guest's network driver is up (it has queued a frame).
           Raising network interrupts before that crashes the boot. */
        out("Waiting for the game's network stack...\r\n");
        for (;;) {
            u32 f[2] = {0, 0}, one[2] = {1, 1}, prod = 0;
            DWORD el = GetTickCount() - t0;
            if (!alive(proc)) { out("The game closed during start-up.\r\n"); pause_exit(1); }
            rd(proc, t.nicFlags, f, 8);
            if (f[0] != 1 || f[1] != 1) wr(proc, t.nicFlags, one, 8);
            rd(proc, t.nic + 0xAC0, &prod, 4);
            if ((prod != 0 && el > 4000) || el > 120000) break;
            Sleep(20);
        }

        if (!wr_code_safely(proc, pid, t.irq, jmp, 10)) { out("Could not install the start-up hook.\r\n"); pause_exit(1); }
        out("Starting the bridge...\r\n");
        for (;;) {
            BridgeCtx live;
            if (!alive(proc)) { out("The game closed.\r\n"); pause_exit(0); }
            if (rd(proc, ctxAddr, &live, sizeof(live))) {
                if (live.running) { say("Bridge running: UDP %u <-> %s:%u\r\n", cfg.localPort, cfg.host, cfg.port); break; }
                if (live.stage >= STAGE_ERR_WS2) {
                    wr_code_safely(proc, pid, t.irq, orig, 10);
                    if (live.stage == STAGE_ERR_BIND) say("Could not use UDP port %u (Winsock error %u). Is another program using it? Change local_port in the ini.\r\n", cfg.localPort, live.wsaError);
                    else say("The bridge failed to start (stage %u, Winsock error %u).\r\n", live.stage, live.wsaError);
                    pause_exit(1);
                }
            }
            Sleep(20);
            if ((waited += 20) > 60000) { out("The bridge did not start within a minute.\r\n"); wr_code_safely(proc, pid, t.irq, orig, 10); pause_exit(1); }
        }
        wr_code_safely(proc, pid, t.irq, orig, 10);

        out("\r\nReady. In the game: Multiplayer -> System Link. Leave this window open.\r\n\r\n");
        {
            u32 lastTx = 0xFFFFFFFF, lastRx = 0xFFFFFFFF;
            while (alive(proc)) {
                BridgeCtx live;
                u32 f[2] = {0, 0}, one[2] = {1, 1};
                if (rd(proc, ctxAddr, &live, sizeof(live)) && (live.txFrames != lastTx || live.rxFrames != lastRx)) {
                    char line[160];
                    lastTx = live.txFrames; lastRx = live.rxFrames;
                    wsprintfA(line, "\rsent %u  received %u  send errors %u      ", live.txFrames, live.rxFrames, live.sendErrors);
                    out(line);
                }
                rd(proc, t.nicFlags, f, 8);
                if (f[0] != 1 || f[1] != 1) wr(proc, t.nicFlags, one, 8);
                Sleep(500);
            }
        }
        (void)h2;
    }
    out("\r\n\r\nThe game closed. Goodbye.\r\n");
    CloseHandle(proc);
    ExitProcess(0);
}
