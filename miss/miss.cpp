#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iphlpapi.h>

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>

#include "..\version.h"

#pragma comment(lib, "miniupnpc.lib")
#pragma comment(lib, "libnatpmp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

#define MINIUPNP_STATICLIB
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>

#define NATPMP_STATICLIB
#include <natpmp.h>

bool getHopsIP4(IN_ADDR* hopAddress, int* hopAddressCount);
struct UPNPDev* getUPnPDevicesByAddress(IN_ADDR address);
bool PCPMapPort(PSOCKADDR_STORAGE localAddr, int localAddrLen, PSOCKADDR_STORAGE pcpAddr, int pcpAddrLen, int proto, int port, bool enable, bool indefinite);

#define NL "\n"

#define SERVICE_NAME "MISS"
#define UPNP_SERVICE_NAME "Moonlight"
#define POLLING_DELAY_SEC 120
#define PORT_MAPPING_DURATION_SEC 3600
#define UPNP_DISCOVERY_DELAY_MS 5000

static struct port_entry {
    int proto;
    int port;
} k_Ports[] = {
    {IPPROTO_TCP, 47984},
    {IPPROTO_TCP, 47989},
    {IPPROTO_TCP, 48010},
    {IPPROTO_UDP, 47998},
    {IPPROTO_UDP, 47999},
    {IPPROTO_UDP, 48000},
    {IPPROTO_UDP, 48002},
    {IPPROTO_UDP, 48010}
};

static const int k_WolPorts[] = { 9, 47009 };

bool UPnPMapPort(struct UPNPUrls* urls, struct IGDdatas* data, int proto, const char* myAddr, int port, bool enable, bool indefinite)
{
    char intClient[16];
    char intPort[6];
    char desc[80];
    char enabled[4];
    char leaseDuration[16];
    const char* protoStr;
    char portStr[6];
    char myDesc[80];
    char computerName[MAX_COMPUTERNAME_LENGTH + 1];

    DWORD nameLen = sizeof(computerName);
    if (!GetComputerNameA(computerName, &nameLen)) {
        printf("GetComputerNameA() failed: %d", GetLastError());
        snprintf(computerName, sizeof(computerName), "UNKNOWN");
    }
    snprintf(myDesc, sizeof(myDesc), "%s - %s", UPNP_SERVICE_NAME, computerName);

    snprintf(portStr, sizeof(portStr), "%d", port);
    switch (proto)
    {
    case IPPROTO_TCP:
        protoStr = "TCP";
        break;
    case IPPROTO_UDP:
        protoStr = "UDP";
        break;
    default:
        assert(false);
        return false;
    }

    printf("Checking for existing UPnP port mapping for %s %s -> %s %s...", protoStr, portStr, myAddr, computerName);
    int err = UPNP_GetSpecificPortMappingEntry(
        urls->controlURL, data->first.servicetype, portStr, protoStr, nullptr,
        intClient, intPort, desc, enabled, leaseDuration);
    if (err == 714) {
        // NoSuchEntryInArray
        printf("NOT FOUND" NL);
    }
    else if (err == 606) {
        printf("UNAUTHORIZED" NL);
    }
    else if (err == UPNPCOMMAND_SUCCESS) {
        // Some routers change the description, so we can't check that here
        if (!strcmp(intClient, myAddr)) {
            if (atoi(leaseDuration) == 0) {
                printf("OK (Permanent)" NL);

                // If we have an existing permanent mapping, we can just leave it alone.
                if (enable) {
                    return true;
                }
            }
            else {
                printf("OK (%s seconds remaining)" NL, leaseDuration);
            }

            if (!enable) {
                // This is our entry. Go ahead and nuke it
                printf("Deleting UPnP mapping for %s %s -> %s...", protoStr, portStr, myAddr);
                err = UPNP_DeletePortMapping(urls->controlURL, data->first.servicetype, portStr, protoStr, nullptr);
                if (err == UPNPCOMMAND_SUCCESS) {
                    printf("OK" NL);
                }
                else {
                    printf("ERROR %d" NL, err);
                }

                return true;
            }
        }
        else {
            printf("CONFLICT: %s %s" NL, intClient, desc);

            // Some UPnP IGDs won't let unauthenticated clients delete other conflicting port mappings
            // for security reasons, but we will give it a try anyway. If GameStream is not enabled,
            // we will leave the conflicting entry alone to avoid disturbing another PC's port forwarding
            // (especially if we're double NATed).
            if (enable) {
                printf("Trying to delete conflicting UPnP mapping for %s %s -> %s...", protoStr, portStr, intClient);
                err = UPNP_DeletePortMapping(urls->controlURL, data->first.servicetype, portStr, protoStr, nullptr);
                if (err == UPNPCOMMAND_SUCCESS) {
                    printf("OK" NL);
                }
                else if (err == 606) {
                    printf("UNAUTHORIZED" NL);
                    return false;
                }
                else {
                    printf("ERROR %d" NL, err);
                    return false;
                }
            }
        }
    }
    else {
        printf("ERROR %d (%s)" NL, err, strupnperror(err));

        // If we get a strange error from the router, we'll assume it's some old broken IGDv1
        // device and only use indefinite lease durations to hopefully avoid confusing it.
        indefinite = true;
    }

    // Bail if GameStream is disabled
    if (!enable) {
        return true;
    }

    // Create or update the expiration time of an existing mapping
    snprintf(leaseDuration, sizeof(leaseDuration), "%d",
        indefinite ? 0 : PORT_MAPPING_DURATION_SEC);
    printf("Updating UPnP port mapping for %s %s -> %s...", protoStr, portStr, myAddr);
    err = UPNP_AddPortMapping(
        urls->controlURL, data->first.servicetype, portStr,
        portStr, myAddr, myDesc, protoStr, nullptr, leaseDuration);
    if (err == 725 && !indefinite) { // OnlyPermanentLeasesSupported
        err = UPNP_AddPortMapping(
            urls->controlURL, data->first.servicetype, portStr,
            portStr, myAddr, myDesc, protoStr, nullptr, "0");
        printf("PERMANENT ");
    }
    if (err == UPNPCOMMAND_SUCCESS) {
        printf("OK" NL);
        return true;
    }
    else {
        printf("ERROR %d (%s)" NL, err, strupnperror(err));
        return false;
    }
}

bool GetIP4OnLinkPrefixLength(char* lanAddressString, int* prefixLength)
{
    union {
        IP_ADAPTER_ADDRESSES addresses;
        char buffer[8192];
    };
    ULONG error;
    ULONG length;
    PIP_ADAPTER_ADDRESSES currentAdapter;
    PIP_ADAPTER_UNICAST_ADDRESS currentAddress;
    in_addr targetAddress;

    inet_pton(AF_INET, lanAddressString, &targetAddress);

    // Get a list of all interfaces with IPv4 addresses on the system
    length = sizeof(buffer);
    error = GetAdaptersAddresses(AF_INET,
        GAA_FLAG_SKIP_ANYCAST |
        GAA_FLAG_SKIP_MULTICAST |
        GAA_FLAG_SKIP_DNS_SERVER |
        GAA_FLAG_SKIP_FRIENDLY_NAME,
        NULL,
        &addresses,
        &length);
    if (error != ERROR_SUCCESS) {
        printf("GetAdaptersAddresses() failed: %d" NL, error);
        return false;
    }

    currentAdapter = &addresses;
    currentAddress = nullptr;
    while (currentAdapter != nullptr) {
        currentAddress = currentAdapter->FirstUnicastAddress;
        while (currentAddress != nullptr) {
            assert(currentAddress->Address.lpSockaddr->sa_family == AF_INET);

            PSOCKADDR_IN currentAddrV4 = (PSOCKADDR_IN)currentAddress->Address.lpSockaddr;

            if (RtlEqualMemory(&currentAddrV4->sin_addr, &targetAddress, sizeof(targetAddress))) {
                *prefixLength = currentAddress->OnLinkPrefixLength;
                return true;
            }

            currentAddress = currentAddress->Next;
        }

        currentAdapter = currentAdapter->Next;
    }

    printf("No adapter found with IPv4 address: %s" NL, lanAddressString);
    return false;
}

bool UPnPHandleDeviceList(struct UPNPDev* list, bool enable, char* lanAddrOverride, char* wanAddr)
{
    struct UPNPUrls urls;
    struct IGDdatas data;
    char localAddress[128];
    char* portMappingInternalAddress;
    int pinholeAllowed = false;
    bool success = true;

    int ret = UPNP_GetValidIGD(list, &urls, &data, localAddress, sizeof(localAddress));
    if (ret == 0) {
        printf("No UPnP device found!" NL);
        return false;
    }
    else if (ret == 3) {
        printf("No UPnP IGD found!" NL);
        FreeUPNPUrls(&urls);
        return false;
    }
    else if (ret == 1) {
        printf("Found a connected UPnP IGD" NL);
    }
    else if (ret == 2) {
        printf("Found a disconnected UPnP IGD (!)" NL);

        // Even if we are able to add forwarding entries, go ahead and try NAT-PMP
        success = false;
    }
    else {
        printf("UPNP_GetValidIGD() failed: %d" NL, ret);
        return false;
    }

    ret = UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, wanAddr);
    if (ret == UPNPCOMMAND_SUCCESS) {
        printf("UPnP IGD WAN address is: %s" NL, wanAddr);
    }
    else {
        // Empty string
        *wanAddr = 0;
    }

    // We may be mapping on behalf of another device
    if (lanAddrOverride != nullptr) {
        portMappingInternalAddress = lanAddrOverride;
    }
    else {
        portMappingInternalAddress = localAddress;
    }

    for (int i = 0; i < ARRAYSIZE(k_Ports); i++) {
        if (!UPnPMapPort(&urls, &data, k_Ports[i].proto, portMappingInternalAddress, k_Ports[i].port, enable, false)) {
            success = false;
        }
    }

    // Do a best-effort for IPv4 Wake-on-LAN broadcast mappings
    for (int i = 0; i < ARRAYSIZE(k_WolPorts); i++) {
        if (lanAddrOverride == nullptr) {
            // Map the port to the broadcast address (may not work on all routers). This
            // ensures delivery even after the ARP entry for this PC times out on the router.
            int onLinkPrefixLen;
            if (GetIP4OnLinkPrefixLength(localAddress, &onLinkPrefixLen)) {
                int netmask = 0;
                for (int j = 0; j < onLinkPrefixLen; j++) {
                    netmask |= (1 << j);
                }

                in_addr broadcastAddr;
                broadcastAddr.S_un.S_addr = inet_addr(localAddress);
                broadcastAddr.S_un.S_addr |= ~netmask;

                char broadcastAddrStr[128];
                inet_ntop(AF_INET, &broadcastAddr, broadcastAddrStr, sizeof(broadcastAddrStr));

                UPnPMapPort(&urls, &data, IPPROTO_UDP, broadcastAddrStr, k_WolPorts[i], enable, true);
            }
        }
        else {
            // When we're mapping the WOL ports upstream of our router, we map directly to
            // the port on the upstream address (likely our router's WAN interface).
            UPnPMapPort(&urls, &data, IPPROTO_UDP, lanAddrOverride, k_WolPorts[i], enable, true);
        }
    }

    FreeUPNPUrls(&urls);
    return success;
}

bool NATPMPMapPort(natpmp_t* natpmp, int proto, int port, bool enable, bool indefinite)
{
    int natPmpProto;

    switch (proto)
    {
    case IPPROTO_TCP:
        natPmpProto = NATPMP_PROTOCOL_TCP;
        break;
    case IPPROTO_UDP:
        natPmpProto = NATPMP_PROTOCOL_UDP;
        break;
    default:
        assert(false);
        return false;
    }

    int lifetime;

    if (!enable) {
        lifetime = 0;
    }
    else if (indefinite) {
        lifetime = 604800; // 1 week
    }
    else {
        lifetime = 3600;
    }

    printf("Updating NAT-PMP port mapping for %s %d...", proto == IPPROTO_TCP ? "TCP" : "UDP", port);
    int err = sendnewportmappingrequest(natpmp, natPmpProto, port, enable ? port : 0, lifetime);
    if (err < 0) {
        printf("ERROR %d" NL, err);
        return false;
    }

    natpmpresp_t response;
    do
    {
        fd_set fds;
        struct timeval timeout;

        FD_ZERO(&fds);
        FD_SET(natpmp->s, &fds);

        err = getnatpmprequesttimeout(natpmp, &timeout);
        if (err != 0) {
            assert(err == 0);
            printf("WAIT FAILED: %d" NL, err);
            return false;
        }

        select(0, &fds, nullptr, nullptr, &timeout);

        err = readnatpmpresponseorretry(natpmp, &response);
    } while (err == NATPMP_TRYAGAIN);

    if (err != 0) {
        printf("FAILED %d" NL, err);
        return false;
    }
    else if (response.pnu.newportmapping.lifetime == 0 && !enable) {
        printf("DELETED" NL);
        return true;
    }
    else if (response.pnu.newportmapping.mappedpublicport != port) {
        printf("CONFLICT" NL);

        // It couldn't assign us the external port we requested and gave us an alternate external port.
        // We can't use this alternate mapping, so immediately release it.
        printf("Deleting unwanted NAT-PMP mapping for %s %d...", proto == IPPROTO_TCP ? "TCP" : "UDP", response.pnu.newportmapping.mappedpublicport);
        err = sendnewportmappingrequest(natpmp, natPmpProto, response.pnu.newportmapping.privateport, 0, 0);
        if (err < 0) {
            printf("ERROR %d" NL, err);
            return false;
        }
        else {
            do {
                fd_set fds;
                struct timeval timeout;

                FD_ZERO(&fds);
                FD_SET(natpmp->s, &fds);

                err = getnatpmprequesttimeout(natpmp, &timeout);
                if (err != 0) {
                    assert(err == 0);
                    printf("WAIT FAILED: %d" NL, err);
                    return false;
                }

                select(0, &fds, nullptr, nullptr, &timeout);

                err = readnatpmpresponseorretry(natpmp, &response);
            } while (err == NATPMP_TRYAGAIN);

            if (err == 0) {
                printf("OK" NL);
                return false;
            }
            else {
                printf("FAILED %d" NL, err);
                return false;
            }
        }
    }
    else {
        printf("OK (%d seconds remaining)" NL, response.pnu.newportmapping.lifetime);
        return true;
    }
}

bool IsGameStreamEnabled()
{
    DWORD error;
    DWORD enabled;
    DWORD len;
    HKEY key;

    error = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\NVIDIA Corporation\\NvStream", 0, KEY_READ | KEY_WOW64_64KEY, &key);
    if (error != ERROR_SUCCESS) {
        printf("RegOpenKeyEx() failed: %d\n", error);
        return false;
    }

    len = sizeof(enabled);
    error = RegQueryValueExA(key, "EnableStreaming", nullptr, nullptr, (LPBYTE)&enabled, &len);
    RegCloseKey(key);
    if (error != ERROR_SUCCESS) {
        printf("RegQueryValueExA() failed: %d" NL, error);
        return false;
    }
    else if (!enabled) {
        printf("GameStream is OFF!" NL);
        return false;
    }
    else {
        printf("GameStream is ON!" NL);
        return true;
    }
}

void UpdatePortMappingsForTarget(bool enable, char* targetAddressIP4, char* internalAddressIP4, char* upstreamAddressIP4)
{
    natpmp_t natpmp;
    bool tryNatPmp = true;
    bool tryPcp = true;
    char upstreamAddrNatPmp[128] = {};
    char upstreamAddrUPnP[128] = {};

    printf("Starting port mapping update on %s to %s..." NL,
        targetAddressIP4 ? targetAddressIP4 : "default gateway",
        internalAddressIP4 ? internalAddressIP4 : "local machine");

    int natPmpErr = initnatpmp(&natpmp, targetAddressIP4 ? 1 : 0, targetAddressIP4 ? inet_addr(targetAddressIP4) : 0);
    if (natPmpErr != 0) {
        printf("initnatpmp() failed: %d" NL, natPmpErr);
    }
    else {
        natPmpErr = sendpublicaddressrequest(&natpmp);
        if (natPmpErr < 0) {
            printf("sendpublicaddressrequest() failed: %d" NL, natPmpErr);
            closenatpmp(&natpmp);
        }
    }

    fflush(stdout);

    {
        int upnpErr;
        struct UPNPDev* ipv4Devs;
        
        if (targetAddressIP4 == nullptr) {
            // If we have no target, use discovery to find the first hop
            ipv4Devs = upnpDiscoverAll(UPNP_DISCOVERY_DELAY_MS, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, 0, 2, &upnpErr);
            printf("UPnP IPv4 IGD discovery completed with error code: %d" NL, upnpErr);
        }
        else {
            // We have a specified target, so do discovery against that directly (may be outside our subnet in case of double-NAT)
            struct in_addr addr;
            addr.S_un.S_addr = inet_addr(targetAddressIP4);
            ipv4Devs = getUPnPDevicesByAddress(addr);
        }

        // Use the delay of discovery to also allow the NAT-PMP endpoint time to respond
        if (natPmpErr >= 0) {
            natpmpresp_t response;
            natPmpErr = readnatpmpresponseorretry(&natpmp, &response);
            if (natPmpErr == 0) {
                inet_ntop(AF_INET, &response.pnu.publicaddress.addr, upstreamAddrNatPmp, sizeof(upstreamAddrNatPmp));
                printf("NAT-PMP upstream address is: %s" NL, upstreamAddrNatPmp);
            }
            else {
                printf("NAT-PMP public address request failed: %d" NL, natPmpErr);
                closenatpmp(&natpmp);
            }
        }

        // Don't try NAT-PMP if UPnP succeeds
        if (UPnPHandleDeviceList(ipv4Devs, enable, internalAddressIP4, upstreamAddrUPnP)) {
            printf("UPnP IPv4 port mapping successful" NL);
            if (enable) {
                // We still want to try NAT-PMP if we're removing
                // rules to ensure any NAT-PMP rules get cleaned up
                tryNatPmp = false;
                tryPcp = false;
            }
        }

        freeUPNPDevlist(ipv4Devs);
    }

    fflush(stdout);

    if (natPmpErr == 0) {
        // NAT-PMP has no description field or other token that we can use to determine
        // if we created the rules we'd be deleting. Since we don't have that, we can't
        // safely remove mappings that could be shared by another machine behind a double NAT.
        if (!enable && targetAddressIP4 != nullptr) {
            printf("Not removing upstream NAT-PMP mappings on non-default gateway device" NL);
            tryNatPmp = false;
        }

        // Don't try with NAT-PMP if the UPnP attempt for the same gateway failed due to being
        // disconnected or some other error. This will avoid overwriting UPnP rules on a disconnected IGD
        // with duplicate NAT-PMP rules. We want to allow deletion of NAT-PMP rules in any case though.
        if (enable && !strcmp(upstreamAddrNatPmp, upstreamAddrUPnP)) {
            printf("Not attempting to use NAT-PMP/PCP to talk to the same UPnP gateway\n");
            tryNatPmp = false;

            // We have both UPnP and NAT-PMP on the same upstream gateway, so let's
            // assume PCP is on the same box too.
            tryPcp = false;
        }

        if (tryNatPmp) {
            bool success = true;
            for (int i = 0; i < ARRAYSIZE(k_Ports); i++) {
                if (!NATPMPMapPort(&natpmp, k_Ports[i].proto, k_Ports[i].port, enable, false)) {
                    success = false;
                }
            }

            // We can only map ports for the non-default gateway case because
            // it will use our LAN address as the internal client address, which
            // doesn't work (needs to be broadcast) for the last hop.
            if (targetAddressIP4 != nullptr) {
                // Best effort, don't care if we fail for WOL
                for (int i = 0; i < ARRAYSIZE(k_WolPorts); i++) {
                    // Indefinite mapping since we may not be awake to refresh it
                    NATPMPMapPort(&natpmp, IPPROTO_UDP, k_WolPorts[i], enable, true);
                }
            }

            if (success) {
                printf("NAT-PMP IPv4 port mapping successful" NL);

                // Always try all possibilities when disabling to ensure
                // we completely clean up
                if (enable) {
                    tryPcp = false;
                }
            }
        }

        closenatpmp(&natpmp);
    }

    // Try PCP for IPv4 if UPnP and NAT-PMP have both failed. This may be the case for CGN that only supports PCP.
    if (tryPcp) {
        SOCKADDR_IN targetAddr = {};
        SOCKADDR_IN internalAddr = {};

        targetAddr.sin_family = AF_INET;
        internalAddr.sin_family = AF_INET;

        if (targetAddressIP4 != nullptr && internalAddressIP4 != nullptr) {
            targetAddr.sin_addr.S_un.S_addr = inet_addr(targetAddressIP4);
            internalAddr.sin_addr.S_un.S_addr = inet_addr(internalAddressIP4);
        }
        else {
            MIB_IPFORWARDROW route;
            DWORD error = GetBestRoute(0, 0, &route);
            if (error == NO_ERROR) {
                targetAddr.sin_addr.S_un.S_addr = route.dwForwardNextHop;
            }
            else {
                printf("GetBestRoute() failed: %d" NL, error);
                goto Exit;
            }
        }

        bool success = true;
        for (int i = 0; i < ARRAYSIZE(k_Ports); i++) {
            if (!PCPMapPort((PSOCKADDR_STORAGE)&internalAddr, sizeof(internalAddr),
                (PSOCKADDR_STORAGE)&targetAddr, sizeof(targetAddr),
                k_Ports[i].proto, k_Ports[i].port, enable, false)) {
                success = false;
            }
        }

        // We can only map ports for the non-default gateway case because
        // it will use our internal address as the internal client address, which
        // doesn't work (needs to be broadcast) for the last hop.
        if (internalAddressIP4 != nullptr) {
            // Best effort, don't care if we fail for WOL
            for (int i = 0; i < ARRAYSIZE(k_WolPorts); i++) {
                // Indefinite mapping since we may not be awake to refresh it
                PCPMapPort((PSOCKADDR_STORAGE)&internalAddr, sizeof(internalAddr),
                    (PSOCKADDR_STORAGE)&targetAddr, sizeof(targetAddr),
                    IPPROTO_UDP, k_WolPorts[i], enable, true);
            }
        }

        if (success) {
            printf("PCP IPv4 port mapping successful" NL);
        }
    }

Exit:
    // Write this at the end to avoid clobbering an input parameter
    if (upstreamAddrNatPmp[0] != 0 && inet_addr(upstreamAddrNatPmp) != 0) {
        printf("Using NAT-PMP upstream IPv4 address: %s" NL, upstreamAddrNatPmp);
        strcpy(upstreamAddressIP4, upstreamAddrNatPmp);
    }
    else if (upstreamAddrUPnP[0] != 0 && inet_addr(upstreamAddrUPnP) != 0) {
        printf("Using UPnP upstream IPv4 address: %s" NL, upstreamAddrUPnP);
        strcpy(upstreamAddressIP4, upstreamAddrUPnP);
    }
    else {
        printf("No valid upstream IPv4 address found!" NL);
        upstreamAddressIP4[0] = 0;
    }
}

bool IsLikelyNAT(unsigned long netByteOrderAddr)
{
    DWORD addr = htonl(netByteOrderAddr);

    // 10.0.0.0/8
    if ((addr & 0xFF000000) == 0x0A000000) {
        return true;
    }
    // 172.16.0.0/12
    else if ((addr & 0xFFF00000) == 0xAC100000) {
        return true;
    }
    // 192.168.0.0/16
    else if ((addr & 0xFFFF0000) == 0xC0A80000) {
        return true;
    }
    // 100.64.0.0/10 - RFC6598 official CGN address
    else if ((addr & 0xFFC00000) == 0x64400000) {
        return true;
    }

    return false;
}

void UpdatePortMappings(bool enable)
{
    IN_ADDR hops[4];
    int hopCount = ARRAYSIZE(hops);
    char upstreamAddrStr[128];
    unsigned long upstreamAddr;

    printf("Finding upstream IPv4 hops via traceroute..." NL);
    if (!getHopsIP4(hops, &hopCount)) {
        hopCount = 0;
    }
    else {
        printf("Found %d hops" NL, hopCount);
    }

    // getHopsIP4() already skips the default gateway, so 0
    // is actually the first hop after the default gateway
    int nextHopIndex = 0;

    // Start by probing for the default gateway
    UpdatePortMappingsForTarget(enable, nullptr, nullptr, upstreamAddrStr);
    while (upstreamAddrStr[0] != 0 && (upstreamAddr = inet_addr(upstreamAddrStr)) != 0) {
        // We got an upstream address. Let's check if this is a NAT
        if (IsLikelyNAT(upstreamAddr)) {
            printf("Upstream address %s is likely a NAT" NL, upstreamAddrStr);

            if (nextHopIndex >= hopCount) {
                printf("Traceroute didn't reach this hop! Aborting!" NL);
                break;
            }

            char targetAddress[128];
            inet_ntop(AF_INET, &hops[nextHopIndex], targetAddress, sizeof(targetAddress));

            // It's a NAT, so let's direct our UPnP/NAT-PMP messages to it.
            // The internal IP address for the new mapping will be the upstream address of the last one.
            // The target IP address to which to send the UPnP/NAT-PMP is the next hop of the traceroute.
            UpdatePortMappingsForTarget(enable, targetAddress, upstreamAddrStr, upstreamAddrStr);
        }
        else {
            // If we reach a proper public IP address, we're done
            printf("Reached the Internet at hop %d" NL, nextHopIndex);
            break;
        }

        // Next hop
        nextHopIndex++;
    }

    fflush(stdout);
}

void NETIOAPI_API_ IpInterfaceChangeNotificationCallback(PVOID context, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE)
{
    SetEvent((HANDLE)context);
}

void ResetLogFile()
{
    char oldLogFilePath[MAX_PATH + 1];
    char currentLogFilePath[MAX_PATH + 1];
    char timeString[MAX_PATH + 1] = {};
    SYSTEMTIME time;

    ExpandEnvironmentStringsA("%ProgramData%\\MISS\\miss-old.log", oldLogFilePath, sizeof(oldLogFilePath));
    ExpandEnvironmentStringsA("%ProgramData%\\MISS\\miss-current.log", currentLogFilePath, sizeof(currentLogFilePath));

    // Close the existing stdout handle. This is important because otherwise
    // it may still be open as stdout when we try to MoveFileEx below.
    fclose(stdout);

    // Rotate the current to the old log file
    MoveFileExA(currentLogFilePath, oldLogFilePath, MOVEFILE_REPLACE_EXISTING);

    // Redirect stdout to this new file
    freopen(currentLogFilePath, "w", stdout);

    // Print a log header
    printf("Moonlight Internet Streaming Service v" VER_VERSION_STR NL);

    // Print the current time
    GetSystemTime(&time);
    GetTimeFormatA(LOCALE_SYSTEM_DEFAULT, 0, &time, "hh':'mm':'ss tt", timeString, ARRAYSIZE(timeString));
    printf("The current UTC time is: %s" NL, timeString);
}

DWORD WINAPI GameStreamStateChangeThread(PVOID Context)
{
    HKEY key;

    // We're watching this key that way we can still detect GameStream turning on
    // if GFE wasn't even installed when our service started
    DWORD err = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\NVIDIA Corporation", 0, KEY_READ | KEY_WOW64_64KEY, &key);
    if (err != ERROR_SUCCESS) {
        printf("RegOpenKeyExA() failed: %d" NL, err);
        return err;
    }

    // Notify the main thread when the GameStream state changes
    bool lastGameStreamState = IsGameStreamEnabled();
    while ((err = RegNotifyChangeKeyValue(key, true, REG_NOTIFY_CHANGE_LAST_SET, nullptr, false)) == ERROR_SUCCESS) {
        bool currentGameStreamState = IsGameStreamEnabled();
        if (lastGameStreamState != currentGameStreamState) {
            SetEvent((HANDLE)Context);
        }
        lastGameStreamState = currentGameStreamState;
    }

    printf("RegNotifyChangeKeyValue() failed: %d" NL, err);
    return err;
}

int Run()
{
    HANDLE ifaceChangeEvent = CreateEvent(nullptr, true, false, nullptr);
    HANDLE gsChangeEvent = CreateEvent(nullptr, true, false, nullptr);
    HANDLE events[2] = { ifaceChangeEvent, gsChangeEvent };

    ResetLogFile();

    // Create the thread to watch for GameStream state changes
    CreateThread(nullptr, 0, GameStreamStateChangeThread, gsChangeEvent, 0, nullptr);

    // Watch for IP address and interface changes
    HANDLE ifaceChangeHandle;
    NotifyIpInterfaceChange(AF_UNSPEC, IpInterfaceChangeNotificationCallback, ifaceChangeEvent, false, &ifaceChangeHandle);

    for (;;) {
        ResetEvent(gsChangeEvent);
        ResetEvent(ifaceChangeEvent);
        UpdatePortMappings(IsGameStreamEnabled());

        // Refresh when half the duration is expired or if an IP interface
        // change event occurs.
        printf("Going to sleep..." NL);
        fflush(stdout);

        ULONGLONG beforeSleepTime = GetTickCount64();
        DWORD ret = WaitForMultipleObjects(ARRAYSIZE(events), events, false, POLLING_DELAY_SEC * 1000);
        if (ret == WAIT_OBJECT_0) {
            ResetLogFile();

            printf("Woke up for interface change notification after %lld seconds" NL,
                (GetTickCount64() - beforeSleepTime) / 1000);

            // Wait a little bit for the interface to settle down (DHCP, RA, etc)
            Sleep(10000);
        }
        else if (ret == WAIT_OBJECT_0 + 1) {
            ResetLogFile();

            printf("Woke up for GameStream state change notification after %lld seconds" NL,
                (GetTickCount64() - beforeSleepTime) / 1000);
        }
        else {
            ResetLogFile();

            printf("Woke up for periodic refresh" NL);
        }
    }
}

static SERVICE_STATUS_HANDLE ServiceStatusHandle;
static SERVICE_STATUS ServiceStatus;

DWORD
WINAPI
HandlerEx(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext)
{
    switch (dwControl)
    {
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;

    case SERVICE_CONTROL_STOP:
        ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        ServiceStatus.dwControlsAccepted = 0;
        SetServiceStatus(ServiceStatusHandle, &ServiceStatus);

        printf("Removing UPnP/NAT-PMP/PCP rules after service stop request\n");
        UpdatePortMappings(false);

        printf("The service is stopping\n");
        ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(ServiceStatusHandle, &ServiceStatus);
        return NO_ERROR;

    default:
        return NO_ERROR;
    }
}

VOID
WINAPI
ServiceMain(DWORD dwArgc, LPTSTR *lpszArgv)
{
    int err;

    ServiceStatusHandle = RegisterServiceCtrlHandlerEx(SERVICE_NAME, HandlerEx, NULL);
    if (ServiceStatusHandle == NULL) {
        fprintf(stderr, "RegisterServiceCtrlHandlerEx() failed: %d" NL, GetLastError());
        return;
    }

    ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    ServiceStatus.dwServiceSpecificExitCode = 0;
    ServiceStatus.dwWin32ExitCode = NO_ERROR;
    ServiceStatus.dwWaitHint = 0;
    ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    ServiceStatus.dwCheckPoint = 0;

    // Tell SCM we're running
    ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(ServiceStatusHandle, &ServiceStatus);

    // Start the service
    err = Run();
    if (err != 0) {
        ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        ServiceStatus.dwWin32ExitCode = err;
        SetServiceStatus(ServiceStatusHandle, &ServiceStatus);
        return;
    }
}

static const SERVICE_TABLE_ENTRY ServiceTable[] = {
    { (LPSTR)SERVICE_NAME, ServiceMain },
    { NULL, NULL }
};

int main(int argc, char* argv[])
{
    WSADATA wsaData;
    int err = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (err != NO_ERROR) {
        return err;
    }

    if (argc == 2 && !strcmp(argv[1], "exe")) {
        Run();
        return 0;
    }

    return StartServiceCtrlDispatcher(ServiceTable);
}