/* shared.h - layout shared between the injector (host side) and the bridge
 * payload that runs inside Emu.exe. Plain C, no headers, fixed-size types. */
#ifndef TSFP_SHARED_H
#define TSFP_SHARED_H

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define BRIDGE_MAGIC 0x4B4E4C53u /* "SLNK" */

/* stage values reported by the bridge */
#define STAGE_INIT        1
#define STAGE_SOCKET      2
#define STAGE_BOUND       3
#define STAGE_THREAD      4
#define STAGE_LOOP        11
#define STAGE_TX_SEND     12  /* calling sendto */
#define STAGE_TX_RAISE    13  /* calling the "sent" interrupt helper */
#define STAGE_RX_RAISE    14  /* calling the "received" interrupt helper */
#define STAGE_IDLE        15  /* back in the loop after a raise */
#define STAGE_ERR_WS2     90
#define STAGE_ERR_SOCKET  91
#define STAGE_ERR_BIND    92
#define STAGE_ERR_THREAD  93

typedef struct BridgeCtx {
    u32 magic;
    u32 size;

    /* kernel32 functions (same addresses in every process of a session) */
    u64 fnLoadLibraryA;
    u64 fnGetProcAddress;
    u64 fnCreateEventA;
    u64 fnCreateThread;
    u64 fnWaitForMultipleObjects;

    /* emulator addresses, found by the injector's pattern scan */
    u64 nic;          /* VirtualNicDevice (primary subobject) */
    u64 nic2;         /* secondary subobject, "this" for the interrupt helpers */
    u64 raiseRx;      /* sets interrupt status 0x40 and raises the NIC interrupt */
    u64 raiseTx;      /* sets interrupt status 0x14 and raises the NIC interrupt */
    u64 bridgeLoop;   /* address of the bridge thread routine (set by the injector) */

    /* NIC object field offsets (verified by the injector's pattern scan) */
    u32 offRxLen;     /* u16 length of the frame waiting for the guest */
    u32 offRxBuf;     /* frame buffer */
    u32 offRxEvent;   /* HANDLE the guest SetEvent()s after copying the frame */
    u32 offTxRing;    /* 40 entries of {u64 buf; u32 len; u32 pad; u64 status} */
    u32 offTxProd;    /* producer index */
    u32 offTxEvent;   /* HANDLE the guest SetEvent()s after queueing */
    u32 txRingSize;
    u32 txStatusDone; /* value written to an entry's status when "sent" */

    /* network configuration, IPv4 in network byte order as a little-endian u32 */
    u32 localIp;
    u32 localPort;
    u32 remoteIp;
    u32 remotePort;

    /* status, read by the injector */
    volatile u32 stage;
    volatile u32 running;
    volatile u32 wsaError;
    volatile u32 txFrames;
    volatile u32 rxFrames;
    volatile u32 rxDropped;
    volatile u32 sendErrors;
    volatile u32 started;     /* set by the hook stub */

    /* private to the bridge */
    u64 sock;
    u64 pSendto;
    u64 pRecvfrom;
    u8  saRemote[16];

    /* strings (kept here so the payload has no data section) */
    char sWs2[16];
    char sWSAStartup[16];
    char sSocket[8];
    char sBind[8];
    char sSendto[8];
    char sRecvfrom[16];
    char sIoctlsocket[16];
    char sWSAGetLastError[24];
} BridgeCtx;

#endif
