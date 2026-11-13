-- tsfp_syslink.lua
-- TimeSplitters Future Perfect (Microsoft Xbox emulator, PC): System Link over UDP.
--
-- 1. At boot: makes the emulated network card report "cable connected", so the
--    System Link menu opens instead of "no network cable attached".
-- 2. Then loads a small bridge into Emu.exe that tunnels the card's raw Ethernet
--    frames over UDP, one frame per datagram (xemu's "UDP tunnel" format).
--
-- Usage: start Cheat Engine, run this script (File > Execute script), THEN launch
-- the game from the Xbox app. Memory only - closing the game undoes everything.
--
-- Relay (change below):
--   XLink Kai (default): run XLink Kai, REMOTE_PORT = 34523. Kai relays the
--     game to your arena, the same way it does for xemu.
--   xemu directly on this PC: REMOTE_PORT = 9368, and set xemu's network to
--     "UDP Tunnel", bind 127.0.0.1:9368, remote 127.0.0.1:9369.
-- REMOTE_HOST is the machine running Kai (or xemu). If it is not this PC,
-- enable "Allow Remote DDS Connections" in Kai.
--
-- Built for Emu.exe from package 2610.3007.1901.0; original bytes are checked
-- before anything is patched.

local REMOTE_HOST = "127.0.0.1"
local REMOTE_PORT = 34523   -- 34523 = XLink Kai, 9368 = xemu direct
local LOCAL_PORT  = 9369

local OFF_LINKSTATE_IMM = 0x8D6AA
local OFF_NIC_FLAGS     = 0x12C2E0
local OFF_IRQ_FUNC      = 0x70980
local IRQ_ORIG = {0x8B,0xC1,0x85,0xC9,0x0F,0x89,0xAD,0x00,0x00,0x00}

local BRIDGE_C = [==[
typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

void *LoadLibraryA(const char *);
void *GetProcAddress(void *, const char *);
void *CreateEventA(void *, int, int, const char *);
void *CreateThread(void *, u64, void *, void *, u32, u32 *);
u32 WaitForMultipleObjects(u32, void **, int, u32);
void Sleep(u32);

typedef int (*t_WSAStartup)(u16, void *);
typedef u64 (*t_socket)(int, int, int);
typedef int (*t_bind)(u64, const void *, int);
typedef int (*t_sendto)(u64, const void *, int, int, const void *, int);
typedef int (*t_recvfrom)(u64, void *, int, int, void *, int *);
typedef int (*t_ioctlsocket)(u64, long, u32 *);
typedef int (*t_WSAGetLastError)(void);

/* configuration, written by the Lua loader before init */
u64 cfg_base;            /* Emu.exe base */
u32 cfg_local_port  = 9369;
u32 cfg_remote_port = 9368;
u32 cfg_remote_ip   = 0x0100007F; /* 127.0.0.1, bytes in network order */
u32 cfg_tx_status   = 0;  /* value written to a frame's status when "sent" */

/* stats, read by the Lua loader */
volatile u32 st_running, st_tx, st_rx, st_rx_dropped, st_err, st_stage, st_wsaerr;
volatile u32 st_init_once;

static t_sendto p_sendto;
static t_recvfrom p_recvfrom;
static u64 sock;
static u8 sa_remote[16];

typedef void (*t_irqhelper)(u64 thisptr);

static void fill_sockaddr(u8 *sa, u32 ip, u32 port)
{
    int i;
    for (i = 0; i < 16; i++) sa[i] = 0;
    sa[0] = 2;                       /* AF_INET (little-endian u16) */
    sa[2] = (u8)(port >> 8);         /* port, network order */
    sa[3] = (u8)(port & 0xFF);
    sa[4] = (u8)ip; sa[5] = (u8)(ip >> 8); sa[6] = (u8)(ip >> 16); sa[7] = (u8)(ip >> 24);
}

static u32 bridge_loop(void *unused)
{
    u8 *obj = (u8 *)(cfg_base + 0x12C2B8);
    u64 obj2 = cfg_base + 0x12C2B0;
    t_irqhelper raise_rx = (t_irqhelper)(cfg_base + 0x8D640);
    t_irqhelper raise_tx = (t_irqhelper)(cfg_base + 0x8D670);
    void *ev[2];
    u32 cons = 0;
    u8 frame[2048];
    u8 from[16];
    int fromlen;

    st_stage = 10;
    ev[0] = CreateEventA(0, 0, 0, 0);    /* tx queued (auto-reset) */
    ev[1] = CreateEventA(0, 0, 0, 0);    /* rx consumed */
    *(void **)(obj + 0xAC8) = ev[0];
    *(void **)(obj + 0x18)  = ev[1];
    st_stage = 11;
    st_running = 1;

    for (;;) {
        u32 w = WaitForMultipleObjects(2, ev, 0, 2);
        u32 prod, sent = 0;

        if (w == 1) {                    /* guest copied the rx frame out */
            *(volatile u16 *)(obj + 0x34) = 0;
        }

        /* TX: drain everything the guest has queued */
        prod = *(volatile u32 *)(obj + 0xAC0);
        while (cons != prod) {
            u8 *e = obj + 0x700 + cons * 24;
            u8 *buf = *(u8 **)(e + 0);
            u32 len = *(u32 *)(e + 8);
            u32 *status = *(u32 **)(e + 0x10);
            if (buf && len > 0 && len <= 1600) {
                if (p_sendto(sock, buf, (int)len, 0, sa_remote, 16) > 0) st_tx++;
                else st_err++;
            }
            if (status) *status = cfg_tx_status;
            cons = (cons + 1) % 40;
            sent++;
        }
        if (sent) raise_tx(obj2);

        /* RX: one frame at a time into the card's buffer */
        if (*(volatile u16 *)(obj + 0x34) == 0) {
            int n;
            fromlen = 16;
            n = p_recvfrom(sock, frame, sizeof(frame), 0, from, &fromlen);
            if (n > 0) {
                if (n <= 1600) {
                    int i;
                    for (i = 0; i < n; i++) obj[0x36 + i] = frame[i];
                    *(volatile u16 *)(obj + 0x34) = (u16)n;
                    st_rx++;
                    raise_rx(obj2);
                } else st_rx_dropped++;
            }
        }
    }
    return 0;
}

/* called once (from the hook) to start the bridge thread */
void bridge_init(void)
{
    void *ws;
    t_WSAStartup p_WSAStartup;
    t_socket p_socket;
    t_bind p_bind;
    t_ioctlsocket p_ioctl;
    t_WSAGetLastError p_err;
    u8 wsadata[512];
    u8 sa_local[16];
    u32 nb = 1;

    st_stage = 1;
    ws = LoadLibraryA("ws2_32.dll");
    if (!ws) { st_stage = 90; return; }
    p_WSAStartup = (t_WSAStartup)GetProcAddress(ws, "WSAStartup");
    p_socket     = (t_socket)GetProcAddress(ws, "socket");
    p_bind       = (t_bind)GetProcAddress(ws, "bind");
    p_sendto     = (t_sendto)GetProcAddress(ws, "sendto");
    p_recvfrom   = (t_recvfrom)GetProcAddress(ws, "recvfrom");
    p_ioctl      = (t_ioctlsocket)GetProcAddress(ws, "ioctlsocket");
    p_err        = (t_WSAGetLastError)GetProcAddress(ws, "WSAGetLastError");
    st_stage = 2;
    p_WSAStartup(0x0202, wsadata);
    sock = p_socket(2, 2, 17);           /* AF_INET, SOCK_DGRAM, IPPROTO_UDP */
    if (sock == (u64)-1) { st_stage = 91; st_wsaerr = p_err(); return; }
    fill_sockaddr(sa_local, (cfg_remote_ip & 0xFF) == 127 ? 0x0100007F : 0, cfg_local_port);
    fill_sockaddr(sa_remote, cfg_remote_ip, cfg_remote_port);
    if (p_bind(sock, sa_local, 16) != 0) { st_stage = 92; st_wsaerr = p_err(); return; }
    p_ioctl(sock, 0x8004667E, &nb);      /* FIONBIO: non-blocking */
    st_stage = 3;
    CreateThread(0, 0, (void *)bridge_loop, 0, 0, 0);
    st_stage = 4;
}
]==]

if tsfpTimer then tsfpTimer.destroy() tsfpTimer = nil end
tsfpLog = {}
local function log(s) tsfpLog[#tsfpLog+1] = os.date("%H:%M:%S ") .. s print("[tsfp] " .. s) end

local ignore = {}
local existing = getProcessIDFromProcessName("Emu.exe")
if existing and existing ~= 0 then ignore[existing] = true log("ignoring already-running Emu.exe pid " .. existing .. " (relaunch the game)") end

local S = { pid=nil, base=nil, patched=false, start=0, writes=0, bridge=0, sym=nil }
tsfpState = S

local function findBase()
  local ok, mods = pcall(enumModules)
  if ok and mods then for _, m in ipairs(mods) do
    if m.Name and m.Name:lower() == "emu.exe" then return m.Address end end end
end

local function bytesEqual(addr, t)
  local x = readBytes(addr, #t, true)
  if not x then return false end
  for i = 1, #t do if x[i] ~= t[i] then return false end end
  return true
end

local function loadBridge(b)
  local src = BRIDGE_C:gsub("u32 cfg_local_port  = 9369;", "u32 cfg_local_port  = " .. LOCAL_PORT .. ";")
                      :gsub("u32 cfg_remote_port = 9368;", "u32 cfg_remote_port = " .. REMOTE_PORT .. ";")
  local ok, sym = pcall(compile, src)
  if not ok or type(sym) ~= "table" or not sym.bridge_init then
    log("bridge compile failed: " .. tostring(sym)) return false end
  S.sym = sym
  writeQword(sym.cfg_base, b)
  local a, b2, c2, d2 = REMOTE_HOST:match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")
  if not a then log("REMOTE_HOST must be an IPv4 address") return false end
  writeInteger(sym.cfg_remote_ip, tonumber(a) + tonumber(b2) * 256 + tonumber(c2) * 65536 + tonumber(d2) * 16777216)
  if not bytesEqual(b + OFF_IRQ_FUNC, IRQ_ORIG) then log("irq function bytes differ - not hooking") return false end
  local irq = b + OFF_IRQ_FUNC
  local script = string.format([[
alloc(tsfpinit,256,%X)
label(orig)
label(tsfpdone)
registersymbol(tsfpdone)
tsfpinit:
  cmp byte ptr [tsfpdone],0
  jne orig
  mov byte ptr [tsfpdone],1
  push rax
  push rcx
  push rdx
  push r8
  push r9
  push r10
  push r11
  sub rsp,20
  mov rax,%X
  call rax
  add rsp,20
  pop r11
  pop r10
  pop r9
  pop r8
  pop rdx
  pop rcx
  pop rax
orig:
  mov eax,ecx
  test ecx,ecx
  jns %X
  jmp %X
tsfpdone:
  db 0
%X:
  jmp tsfpinit
  nop 5
]], irq, sym.bridge_init, irq + 0xB7, irq + 0x0A, irq)
  local aok, err = autoAssemble(script)
  if not aok then log("hook failed: " .. tostring(err)) return false end
  log("bridge loaded; waiting for it to start")
  return true
end

tsfpTimer = createTimer(nil, false)
tsfpTimer.Interval = 20
tsfpTimer.OnTimer = function(t)
  if not S.pid then
    local pid = getProcessIDFromProcessName("Emu.exe")
    if pid and pid ~= 0 and not ignore[pid] then openProcess(pid) S.pid = pid log("found Emu.exe pid " .. pid) end
    return
  end
  if not S.base then
    local b = findBase()
    if b then S.base = b log(string.format("base %X", b)) else return end
  end
  local b = S.base
  if not S.patched then
    if bytesEqual(b + OFF_LINKSTATE_IMM - 1, {0xBB, 0x20, 0x00}) then
      writeBytes(b + OFF_LINKSTATE_IMM, 0x24) S.patched = true S.start = getTickCount()
      log("link state patched (cable connected)")
    elseif bytesEqual(b + OFF_LINKSTATE_IMM - 1, {0xBB, 0x24, 0x00}) then
      S.patched = true S.start = getTickCount() log("link state already patched")
    else
      log("unexpected Emu.exe bytes - different version, stopping") t.Enabled = false return
    end
  end
  local f1, f2 = readInteger(b + OFF_NIC_FLAGS), readInteger(b + OFF_NIC_FLAGS + 4)
  if f1 ~= 1 or f2 ~= 1 then
    writeInteger(b + OFF_NIC_FLAGS, 1) writeInteger(b + OFF_NIC_FLAGS + 4, 1) S.writes = S.writes + 1
  end
  local now = getTickCount() - S.start
  if S.bridge == 0 and now > 3000 then
    S.bridge = loadBridge(b) and 1 or -1
  elseif S.bridge == 1 then
    if readInteger(S.sym.st_running) == 1 then
      writeBytes(b + OFF_IRQ_FUNC, table.unpack(IRQ_ORIG))
      S.bridge = 2
      log(string.format("bridge running: UDP 127.0.0.1:%d <-> %s:%d%s", LOCAL_PORT, REMOTE_HOST, REMOTE_PORT, REMOTE_PORT == 34523 and " (XLink Kai)" or ""))
    elseif readInteger(S.sym.st_stage) >= 90 then
      log("bridge socket error, stage " .. readInteger(S.sym.st_stage) .. " wsa " .. readInteger(S.sym.st_wsaerr))
      writeBytes(b + OFF_IRQ_FUNC, table.unpack(IRQ_ORIG)) S.bridge = -1
    end
  end
  if now > 90000 and S.bridge ~= 1 then t.Enabled = false log("setup finished") end
end

function tsfpStats()
  if not (S.sym and S.bridge == 2) then return "bridge not running" end
  return string.format("sent %d  received %d  errors %d", readInteger(S.sym.st_tx), readInteger(S.sym.st_rx), readInteger(S.sym.st_err))
end

tsfpTimer.Enabled = true
log("armed: launch the game now")
