#ifndef NP_PLAT_H
#define NP_PLAT_H

/* 跨平台 socket 封装 */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET np_sock;
#define NP_BAD_SOCK INVALID_SOCKET
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int np_sock;
#define NP_BAD_SOCK (-1)
#endif

int np_net_init(void);
int np_closesocket(np_sock s);

#endif
