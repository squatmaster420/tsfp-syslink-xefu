/* bridge.c - runs inside Emu.exe. Position-independent: no imports, no data
 * section, no absolute addresses. Everything it needs comes from BridgeCtx.
 *
 * Moves raw Ethernet frames between the emulator's VirtualNicDevice and a UDP
 * socket, one frame per datagram (the format xemu's "UDP Tunnel" uses and
 * XLink Kai accepts on port 34523). */
#include "shared.h"

typedef void *(*t_LoadLibraryA)(const char *);
typedef void *(*t_GetProcAddress)(void *, const char *);
typedef void *(*t_CreateEventA)(void *, int, int, const char *);
typedef void *(*t_CreateThread)(void *, u64, void *, void *, u32, u32 *);
typedef u32 (*t_WaitForMultipleObjects)(u32, void **, int, u32);

typedef int (*t_WSAStartup)(u16, void *);
typedef u64 (*t_socket)(int, int, int);
typedef int (*t_bind)(u64, const void *, int);
typedef int (*t_sendto)(u64, const void *, int, int, const void *, int);
typedef int (*t_recvfrom)(u64, void *, int, int, void *, int *);
typedef int (*t_ioctlsocket)(u64, long, u32 *);
typedef int (*t_WSAGetLastError)(void);

typedef void (*t_raise)(u64 thisptr);

#define MAX_FRAME 1600
#define FIONBIO_  0x8004667E

static void sockaddr_in4(u8 *sa, u32 ip, u32 port)
{
    volatile u8 *p = sa;
    int i;
    for (i = 0; i < 16; i++) p[i] = 0;
    p[0] = 2;                        /* AF_INET */
    p[2] = (u8)(port >> 8);
    p[3] = (u8)port;
    p[4] = (u8)ip;
    p[5] = (u8)(ip >> 8);
    p[6] = (u8)(ip >> 16);
    p[7] = (u8)(ip >> 24);
}

u32 bridge_loop(BridgeCtx *c)
{
    t_CreateEventA CreateEventA = (t_CreateEventA)c->fnCreateEventA;
    t_WaitForMultipleObjects Wait = (t_WaitForMultipleObjects)c->fnWaitForMultipleObjects;
    t_sendto p_sendto = (t_sendto)c->pSendto;
    t_recvfrom p_recvfrom = (t_recvfrom)c->pRecvfrom;
    t_raise raiseRx = (t_raise)c->raiseRx;
    t_raise raiseTx = (t_raise)c->raiseTx;
    u8 *nic = (u8 *)c->nic;
    volatile u16 *rxLen = (volatile u16 *)(nic + c->offRxLen);
    volatile u8 *rxBuf = nic + c->offRxBuf;
    volatile u32 *txProd = (volatile u32 *)(nic + c->offTxProd);
    void *ev[2];
    u32 cons = 0;         /* also sends what the guest queued before we started */
    u8 frame[2048];
    u8 from[16];

    ev[0] = CreateEventA(0, 0, 0, 0);   /* guest queued a frame */
    ev[1] = CreateEventA(0, 0, 0, 0);   /* guest copied the rx frame */
    *(void *volatile *)(nic + c->offTxEvent) = ev[0];
    *(void *volatile *)(nic + c->offRxEvent) = ev[1];

    c->stage = STAGE_LOOP;
    c->running = 1;

    for (;;) {
        u32 w = Wait(2, ev, 0, 2);
        u32 prod, sent = 0;

        if (w == 1) *rxLen = 0;          /* rx buffer free again */

        prod = *txProd;
        while (cons != prod) {
            u8 *e = nic + c->offTxRing + cons * 24;
            u8 *buf = *(u8 **)(e + 0);
            u32 len = *(u32 *)(e + 8);
            volatile u32 *st = *(volatile u32 **)(e + 16);
            if (buf && len > 0 && len <= MAX_FRAME) {
                c->stage = STAGE_TX_SEND;
                if (p_sendto(c->sock, buf, (int)len, 0, c->saRemote, 16) > 0) c->txFrames++;
                else c->sendErrors++;
            }
            if (st) *st = c->txStatusDone;
            cons = (cons + 1) % c->txRingSize;
            sent++;
        }
        if (sent) { c->stage = STAGE_TX_RAISE; raiseTx(c->nic2); c->stage = STAGE_IDLE; }

        if (*rxLen == 0) {
            int fromlen = 16;
            int n = p_recvfrom(c->sock, frame, sizeof(frame), 0, from, &fromlen);
            if (n > 0) {
                if (n <= MAX_FRAME) {
                    int i;
                    for (i = 0; i < n; i++) rxBuf[i] = frame[i];
                    *rxLen = (u16)n;
                    c->rxFrames++;
                    c->stage = STAGE_RX_RAISE;
                    raiseRx(c->nic2);
                    c->stage = STAGE_IDLE;
                } else {
                    c->rxDropped++;
                }
            }
        }
    }
    return 0;
}

/* Called once by the hook stub, on whatever emulator thread hits it first. */
void bridge_init(BridgeCtx *c)
{
    t_LoadLibraryA LoadLibraryA = (t_LoadLibraryA)c->fnLoadLibraryA;
    t_GetProcAddress GetProcAddress = (t_GetProcAddress)c->fnGetProcAddress;
    t_CreateThread CreateThread = (t_CreateThread)c->fnCreateThread;
    t_WSAStartup p_WSAStartup;
    t_socket p_socket;
    t_bind p_bind;
    t_ioctlsocket p_ioctl;
    t_WSAGetLastError p_err;
    void *ws;
    u8 wsadata[512];
    u8 local[16];
    u32 nb = 1;

    c->stage = STAGE_INIT;
    ws = LoadLibraryA(c->sWs2);
    if (!ws) { c->stage = STAGE_ERR_WS2; return; }
    p_WSAStartup = (t_WSAStartup)GetProcAddress(ws, c->sWSAStartup);
    p_socket     = (t_socket)GetProcAddress(ws, c->sSocket);
    p_bind       = (t_bind)GetProcAddress(ws, c->sBind);
    p_ioctl      = (t_ioctlsocket)GetProcAddress(ws, c->sIoctlsocket);
    p_err        = (t_WSAGetLastError)GetProcAddress(ws, c->sWSAGetLastError);
    c->pSendto   = (u64)GetProcAddress(ws, c->sSendto);
    c->pRecvfrom = (u64)GetProcAddress(ws, c->sRecvfrom);
    if (!p_WSAStartup || !p_socket || !p_bind || !p_ioctl || !p_err || !c->pSendto || !c->pRecvfrom) {
        c->stage = STAGE_ERR_WS2; return;
    }

    c->stage = STAGE_SOCKET;
    p_WSAStartup(0x0202, wsadata);
    c->sock = p_socket(2, 2, 17);
    if (c->sock == (u64)-1) { c->wsaError = (u32)p_err(); c->stage = STAGE_ERR_SOCKET; return; }
    sockaddr_in4(local, c->localIp, c->localPort);
    sockaddr_in4(c->saRemote, c->remoteIp, c->remotePort);
    if (p_bind(c->sock, local, 16) != 0) { c->wsaError = (u32)p_err(); c->stage = STAGE_ERR_BIND; return; }
    p_ioctl(c->sock, FIONBIO_, &nb);

    c->stage = STAGE_THREAD;   /* set first: the new thread updates it from here on */
    if (!CreateThread(0, 0, (void *)c->bridgeLoop, c, 0, 0)) { c->stage = STAGE_ERR_THREAD; return; }
}
