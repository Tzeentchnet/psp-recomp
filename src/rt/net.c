#include "recomp.h"

#include <stddef.h>

#define A0 (s->r[4])
#define A1 (s->r[5])

enum {
    SCE_KERNEL_ERROR_ILLEGAL_ADDR = 0x80000103u,
    SCE_NET_ERROR_LOCAL_ADDR_UNAVAILABLE = 0x80410180u,
    SCE_NET_RESOLVER_ERROR_NO_DNS_SERVER = 0x80410402u,
    SCE_NET_RESOLVER_ERROR_INVALID_PTR = 0x80410403u,
    SCE_NET_ADHOC_ERROR_NOT_CONNECTED = 0x8041070bu,
    SCE_NET_APCTL_ERROR_WLAN_SWITCH_OFF = 0x80410a06u,
    SCE_NET_ADHOCCTL_ERROR_WLAN_SWITCH_OFF = 0x80410b03u,
    SCE_NET_ADHOCCTL_ERROR_DISCONNECTED = 0x80410b09u,
    ERROR_INET_ENETDOWN = 115u,
};

static const uint8_t s_mac[6] = { 0x00, 0x13, 0x37, 0xac, 0xc5, 0x10 };
static uint32_t s_inet_errno;
static uint32_t s_next_resolver_id = 1;
static uint32_t s_next_apctl_handler;
static uint32_t s_next_adhocctl_handler;

static uint32_t h_success(CpuState *s) {
    (void)s;
    return 0;
}

static uint32_t h_inet_init(CpuState *s) {
    (void)s;
    s_inet_errno = 0;
    return 0;
}

static uint32_t h_inet_term(CpuState *s) {
    (void)s;
    s_inet_errno = 0;
    return 0;
}

static uint32_t h_inet_offline(CpuState *s) {
    (void)s;
    s_inet_errno = ERROR_INET_ENETDOWN;
    return UINT32_MAX;
}

static uint32_t h_inet_get_errno(CpuState *s) {
    (void)s;
    return s_inet_errno;
}

static int parse_ipv4(uint32_t addr, uint32_t *value) {
    uint32_t result = 0;
    if (!addr) return 0;

    for (int part = 0; part < 4; part++) {
        uint32_t octet = 0;
        int digits = 0;
        for (;;) {
            uint8_t ch = MEM_R8(addr);
            if (ch < '0' || ch > '9') break;
            octet = octet * 10 + (uint32_t)(ch - '0');
            if (octet > 255) return 0;
            addr++;
            digits++;
        }
        if (!digits) return 0;
        result |= octet << (part * 8);
        if (part < 3) {
            if (MEM_R8(addr++) != '.') return 0;
        } else if (MEM_R8(addr) != 0) {
            return 0;
        }
    }

    *value = result;
    return 1;
}

static uint32_t h_inet_addr(CpuState *s) {
    uint32_t value;
    return parse_ipv4(A0, &value) ? value : UINT32_MAX;
}

static uint32_t h_inet_aton(CpuState *s) {
    uint32_t value;
    if (!A1 || !parse_ipv4(A0, &value)) return 0;
    MEM_W32(A1, value);
    return 1;
}

static uint32_t h_resolver_init(CpuState *s) {
    (void)s;
    s_next_resolver_id = 1;
    return 0;
}

static uint32_t h_resolver_create(CpuState *s) {
    if (!A0) return SCE_NET_RESOLVER_ERROR_INVALID_PTR;
    MEM_W32(A0, s_next_resolver_id++);
    return 0;
}

static uint32_t h_resolver_offline(CpuState *s) {
    (void)s;
    return SCE_NET_RESOLVER_ERROR_NO_DNS_SERVER;
}

static uint32_t h_apctl_init(CpuState *s) {
    (void)s;
    s_next_apctl_handler = 0;
    return 0;
}

static uint32_t h_apctl_add_handler(CpuState *s) {
    (void)s;
    return s_next_apctl_handler++;
}

static uint32_t h_apctl_offline(CpuState *s) {
    (void)s;
    return SCE_NET_APCTL_ERROR_WLAN_SWITCH_OFF;
}

static uint32_t h_adhocctl_init(CpuState *s) {
    (void)s;
    s_next_adhocctl_handler = 0;
    return 0;
}

static uint32_t h_adhocctl_add_handler(CpuState *s) {
    (void)s;
    return s_next_adhocctl_handler++;
}

static uint32_t h_adhocctl_offline(CpuState *s) {
    (void)s;
    return SCE_NET_ADHOCCTL_ERROR_WLAN_SWITCH_OFF;
}

static uint32_t h_adhocctl_disconnected(CpuState *s) {
    (void)s;
    return SCE_NET_ADHOCCTL_ERROR_DISCONNECTED;
}

static uint32_t h_adhoc_offline(CpuState *s) {
    (void)s;
    return SCE_NET_ADHOC_ERROR_NOT_CONNECTED;
}

static uint32_t h_net_get_local_ether_addr(CpuState *s) {
    (void)s;
    return SCE_NET_ERROR_LOCAL_ADDR_UNAVAILABLE;
}

static uint32_t h_wlan_get_ether_addr(CpuState *s) {
    if (!A0) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    for (int i = 0; i < 6; i++) MEM_W8(A0 + (uint32_t)i, s_mac[i]);
    return 0;
}

static uint32_t h_wlan_off(CpuState *s) {
    (void)s;
    return 0;
}

static uint32_t h_open_psid(CpuState *s) {
    static const uint8_t suffix[10] = {
        0x93, 0xb0, 0xcc, 0x6e, 0xd1, 0x32, 0x27, 0x85, 0x0f, 0x9d
    };
    if (!A0) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    for (int i = 0; i < 6; i++) MEM_W8(A0 + (uint32_t)i, s_mac[i]);
    for (int i = 0; i < 10; i++) MEM_W8(A0 + 6u + (uint32_t)i, suffix[i]);
    return 0;
}

typedef struct NetHleEntry {
    uint32_t nid;
    const char *name;
    HleFn fn;
} NetHleEntry;

static const NetHleEntry s_net_hle[] = {
    { 0x224c5f44u, "sceNetResolverStartNtoA", h_resolver_offline },
    { 0x244172afu, "sceNetResolverCreate", h_resolver_create },
    { 0x6138194au, "sceNetResolverTerm", h_success },
    { 0x94523e09u, "sceNetResolverDelete", h_success },
    { 0xf3370e61u, "sceNetResolverInit", h_resolver_init },

    { 0x05038fc7u, "sceNetInetSendto", h_inet_offline },
    { 0x162e6fd5u, "sceNetInetGetsockname", h_inet_offline },
    { 0x17943399u, "sceNetInetInit", h_inet_init },
    { 0x1a33f9aeu, "sceNetInetBind", h_inet_offline },
    { 0x1bdf5d13u, "sceNetInetInetAton", h_inet_aton },
    { 0x2fe71fe7u, "sceNetInetSetsockopt", h_inet_offline },
    { 0x410b34aau, "sceNetInetConnect", h_inet_offline },
    { 0x4a114c7cu, "sceNetInetGetsockopt", h_inet_offline },
    { 0x4cfe4e56u, "sceNetInetShutdown", h_inet_offline },
    { 0x5be8d595u, "sceNetInetSelect", h_inet_offline },
    { 0x774e36f4u, "sceNetInetSendmsg", h_inet_offline },
    { 0x7aa671bcu, "sceNetInetSend", h_inet_offline },
    { 0x8b7b220fu, "sceNetInetSocket", h_inet_offline },
    { 0x8d7284eau, "sceNetInetClose", h_inet_offline },
    { 0xa9ed66b9u, "sceNetInetTerm", h_inet_term },
    { 0xb75d5b0au, "sceNetInetInetAddr", h_inet_addr },
    { 0xc91142e4u, "sceNetInetRecvfrom", h_inet_offline },
    { 0xcda85c99u, "sceNetInetRecv", h_inet_offline },
    { 0xd10a1a7au, "sceNetInetListen", h_inet_offline },
    { 0xdb094e1bu, "sceNetInetAccept", h_inet_offline },
    { 0xfaabb1ddu, "sceNetInetPoll", h_inet_offline },
    { 0xfbabe411u, "sceNetInetGetErrno", h_inet_get_errno },

    { 0x24fe91a1u, "sceNetApctlDisconnect", h_success },
    { 0x2befdf23u, "sceNetApctlGetInfo", h_apctl_offline },
    { 0x5963991bu, "sceNetApctlDelHandler", h_success },
    { 0x8abadd51u, "sceNetApctlAddHandler", h_apctl_add_handler },
    { 0xb3edd0ecu, "sceNetApctlTerm", h_success },
    { 0xe2f91f9bu, "sceNetApctlInit", h_apctl_init },

    { 0x191cdeffu, "sceSslEnd", h_success },
    { 0x957ecbe2u, "sceSslInit", h_success },
    { 0x87797bddu, "sceHttpsLoadDefaultCert", h_success },
    { 0xab1abe07u, "sceHttpInit", h_success },
    { 0xd1c8945eu, "sceHttpEnd", h_success },
    { 0xe4d21302u, "sceHttpsInit", h_success },
    { 0xf9d8eb63u, "sceHttpsEnd", h_success },
    { 0xc69bebceu, "sceOpenPSIDGetOpenPSID", h_open_psid },

    { 0x2a2a1e07u, "sceNetAdhocMatchingInit", h_success },
    { 0x7945ecdau, "sceNetAdhocMatchingTerm", h_success },
    { 0x20b317a0u, "sceNetAdhocctlAddHandler", h_adhocctl_add_handler },
    { 0x34401d65u, "sceNetAdhocctlDisconnect", h_success },
    { 0x6402490bu, "sceNetAdhocctlDelHandler", h_success },
    { 0x9d689e13u, "sceNetAdhocctlTerm", h_success },
    { 0xe26f226eu, "sceNetAdhocctlInit", h_adhocctl_init },
    { 0x0ad043edu, "sceNetAdhocctlConnect", h_adhocctl_offline },
    { 0xded9d28eu, "sceNetAdhocctlGetParameter", h_adhocctl_disconnected },

    { 0xa62c6f57u, "sceNetAdhocTerm", h_success },
    { 0xe1d621d7u, "sceNetAdhocInit", h_success },
    { 0x157e6225u, "sceNetAdhocPtpClose", h_adhoc_offline },
    { 0x4da4c788u, "sceNetAdhocPtpSend", h_adhoc_offline },
    { 0x6f92741bu, "sceNetAdhocPdpCreate", h_adhoc_offline },
    { 0x7f27bb5eu, "sceNetAdhocPdpDelete", h_adhoc_offline },
    { 0x877f6d66u, "sceNetAdhocPtpOpen", h_adhoc_offline },
    { 0x8bea2b3eu, "sceNetAdhocPtpRecv", h_adhoc_offline },
    { 0x9ac2eeacu, "sceNetAdhocPtpFlush", h_adhoc_offline },
    { 0x9df81198u, "sceNetAdhocPtpAccept", h_adhoc_offline },
    { 0xabed3790u, "sceNetAdhocPdpSend", h_adhoc_offline },
    { 0xb9685118u, "sceNetAdhocGetPtpStat", h_adhoc_offline },
    { 0xc7c1fc57u, "sceNetAdhocGetPdpStat", h_adhoc_offline },
    { 0xdfe53e03u, "sceNetAdhocPdpRecv", h_adhoc_offline },
    { 0xe08bdac1u, "sceNetAdhocPtpListen", h_adhoc_offline },
    { 0xfc6fc07bu, "sceNetAdhocPtpConnect", h_adhoc_offline },

    { 0x281928a9u, "sceNetTerm", h_success },
    { 0x39af39a6u, "sceNetInit", h_success },
    { 0x0bf0a3aeu, "sceNetGetLocalEtherAddr", h_net_get_local_ether_addr },

    { 0x0c622081u, "sceWlanGetEtherAddr", h_wlan_get_ether_addr },
    { 0x93440b11u, "sceWlanDevIsPowerOn", h_wlan_off },
    { 0xd7763699u, "sceWlanGetSwitchState", h_wlan_off },
};

void sr_net_register(void) {
    for (size_t i = 0; i < sizeof(s_net_hle) / sizeof(s_net_hle[0]); i++)
        sr_hle_register(s_net_hle[i].nid, s_net_hle[i].name, s_net_hle[i].fn);
}
