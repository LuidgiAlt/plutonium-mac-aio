/* Receives on nonblocking sockets (patch 0021 answers empty ones in ntdll).
 *   empty:     an empty nonblocking UDP recv fails with WSAEWOULDBLOCK (x1000)
 *   dup-block: after FIONBIO(0) on a duplicate handle of the same socket, a
 *              recv on the original handle blocks until a datagram arrives
 *              (nonblocking mode belongs to the socket, not the handle)
 *   shutdown:  after shutdown(SD_RECEIVE), recv does not report WSAEWOULDBLOCK
 * Build: i686-w64-mingw32-gcc -O2 -o recv-test.exe recv-test.c -lws2_32 */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include "aio_test.h"

static SOCKET bound_udp(struct sockaddr_in *addr)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int len = sizeof(*addr);
    u_long on = 1;

    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, (struct sockaddr *)addr, sizeof(*addr));
    getsockname(s, (struct sockaddr *)addr, &len);
    ioctlsocket(s, FIONBIO, &on);
    return s;
}

static volatile LONG received = -2;
static DWORD WINAPI receiver(void *arg)
{
    char buf[16];
    int r = recv((SOCKET)(ULONG_PTR)arg, buf, sizeof(buf), 0);
    InterlockedExchange(&received, r < 0 ? -(LONG)WSAGetLastError() : r);
    return 0;
}

int main(void)
{
    WSADATA wsa;
    struct sockaddr_in addr;
    SOCKET s, dup, sender;
    HANDLE dup_handle, thread;
    char buf[16];
    u_long off = 0;
    int i, r, err;

    WSAStartup(MAKEWORD(2, 2), &wsa);

    /* empty */
    s = bound_udp(&addr);
    for (i = 0; i < 1000; i++)
    {
        r = recv(s, buf, sizeof(buf), 0);
        if (r != SOCKET_ERROR || (err = WSAGetLastError()) != WSAEWOULDBLOCK)
            return report(0, "empty: recv %d returned %d, error %d (expected WSAEWOULDBLOCK)", i, r, WSAGetLastError());
    }

    /* dup-block: the original handle is now known to be nonblocking. */
    if (!DuplicateHandle(GetCurrentProcess(), (HANDLE)s, GetCurrentProcess(), &dup_handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return report(0, "DuplicateHandle failed: %lu", GetLastError());
    dup = (SOCKET)dup_handle;
    if (ioctlsocket(dup, FIONBIO, &off))
        return report(0, "FIONBIO(0) on the duplicate failed: %d", WSAGetLastError());
    thread = CreateThread(NULL, 0, receiver, (void *)(ULONG_PTR)s, 0, NULL);
    Sleep(200);
    if (received != -2)
        return report(0, "dup-block: recv on the original returned %ld after FIONBIO(0) on a duplicate (expected to block)", received);
    sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sendto(sender, "ping", 4, 0, (struct sockaddr *)&addr, sizeof(addr));
    if (WaitForSingleObject(thread, 1000) || received != 4)
        return report(0, "dup-block: recv returned %ld after a datagram (expected 4)", received);
    closesocket(dup);
    closesocket(s);

    /* shutdown */
    s = bound_udp(&addr);
    recv(s, buf, sizeof(buf), 0);
    shutdown(s, SD_RECEIVE);
    r = recv(s, buf, sizeof(buf), 0);
    err = r == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (r == SOCKET_ERROR && err == WSAEWOULDBLOCK)
        return report(0, "shutdown: recv after SD_RECEIVE reported WSAEWOULDBLOCK");

    return report(1, "empty x1000, dup-block, shutdown (recv %d, error %d)", r, err);
}
