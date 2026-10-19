/* SPDX-License-Identifier: Apache-2.0
 *
 * Minimal request logger for the USB host-interface bring-up probe.
 *
 * The point is not to serve anything -- it is to answer one question: does the
 * host firmware actually talk over the USB link once we present a standard
 * RNDIS/ECM device?
 *
 * From the decompiled RedfishHi BIOS module: at POST the BIOS opens a
 * Simple-Network interface on this device and speaks TLS Redfish to
 * https://169.254.0.17:443 (DMTF USB link-local addressing). So the probe
 * listens on both ports:
 *
 *   - port 80  : plain HTTP. Requests are logged verbatim (request line and
 *                headers, including the X-Host header AMI's host interface
 *                uses) and answered with an empty 200 so the client does not
 *                hang.
 *   - port 443 : TLS. The first bytes of a TLS session are the ClientHello;
 *                they are logged as a hex dump and the connection is closed.
 *                A ClientHello here proves the BIOS's NIC and TCP stack came
 *                up, even though the TLS handshake itself will fail.
 *
 * Deliberately dependency-free: no busybox applet or capture tool is assumed
 * to exist in the image.
 *
 * See CLAUDE.md.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define REQ_MAX 4096
#define PORT_COUNT 2

static const int listenPorts[PORT_COUNT] = {80, 443};

static const char response[] = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: 2\r\n"
                               "Connection: close\r\n"
                               "\r\n"
                               "{}";

/* Print the request as single lines so the journal stays readable. A leading
 * marker makes it greppable: journalctl | grep 'HOST-IF REQUEST'. */
static void logRequest(const char* buf, ssize_t len)
{
    fprintf(stderr, "HOST-IF REQUEST (%zd bytes):\n", len);
    for (ssize_t i = 0; i < len; i++)
    {
        fputc(buf[i], stderr);
    }
    if (len > 0 && buf[len - 1] != '\n')
    {
        fputc('\n', stderr);
    }
    fprintf(stderr, "HOST-IF END\n");
}

/* Binary protocols cannot be printed as text; hex dump them instead. Also
 * greppable: journalctl | grep 'HOST-IF TLS'. */
static void logHex(const char* label, const unsigned char* buf, ssize_t len)
{
    fprintf(stderr, "HOST-IF %s (%zd bytes):\n", label, len);
    for (ssize_t i = 0; i < len; i += 16)
    {
        fprintf(stderr, "  %04zx:", i);
        size_t span = (len - i < 16) ? (size_t)(len - i) : 16;
        for (size_t j = 0; j < span; j++)
        {
            fprintf(stderr, " %02x", buf[i + j]);
        }
        for (size_t j = span; j < 16; j++)
        {
            fputs("   ", stderr);
        }
        fprintf(stderr, "  |");
        for (size_t j = 0; j < span; j++)
        {
            unsigned char c = buf[i + j];
            fputc((c >= 0x20 && c < 0x7f) ? c : '.', stderr);
        }
        fputs("|\n", stderr);
    }
    fprintf(stderr, "HOST-IF %s END\n", label);
}

/* One listener per port. On EADDRNOTAVAIL the caller retries; EADDRINUSE is
 * not fatal because whatever owns the port will answer -- and log -- the
 * host's requests itself. Returns the bound fd, -1 to skip, or exits on real
 * errors. */
static int makeListener(int port, const struct sockaddr_in* addr)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
    {
        perror("socket");
        exit(1);
    }

    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in tmp = *addr;
    tmp.sin_port = htons(port);
    for (;;)
    {
        if (bind(listener, (struct sockaddr*)&tmp, sizeof(tmp)) == 0)
        {
            break;
        }
        if (errno == EADDRNOTAVAIL)
        {
            fprintf(stderr, "HOST-IF address not assigned yet; retrying\n");
            sleep(2);
            continue;
        }
        if (errno == EADDRINUSE)
        {
            fprintf(stderr,
                    "HOST-IF port %d already in use on this address; not "
                    "listening on this port. Check whichever service owns it "
                    "(e.g. 'journalctl -u bmcweb') for the host's requests.\n",
                    port);
            close(listener);
            return -1;
        }
        perror("bind");
        exit(1);
    }
    if (listen(listener, 8) < 0)
    {
        perror("listen");
        exit(1);
    }

    fprintf(stderr, "HOST-IF listening on port %d\n", port);
    fflush(stderr);
    return listener;
}

int main(int argc, char** argv)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;

    /* Bind to the USB link address when one is given, so that only
     * host-interface traffic is logged and not unrelated requests arriving on
     * the management LAN. */
    if (argc > 1)
    {
        if (inet_pton(AF_INET, argv[1], &addr.sin_addr) != 1)
        {
            fprintf(stderr, "HOST-IF bad bind address '%s'\n", argv[1]);
            return 1;
        }
    }
    else
    {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    struct pollfd fds[PORT_COUNT];
    int portOf[PORT_COUNT];
    int count = 0;
    for (int i = 0; i < PORT_COUNT; i++)
    {
        int fd = makeListener(listenPorts[i], &addr);
        if (fd < 0)
        {
            continue;
        }
        fds[count].fd = fd;
        fds[count].events = POLLIN;
        portOf[count] = listenPorts[i];
        count++;
    }
    if (count == 0)
    {
        fprintf(stderr, "HOST-IF no ports available; exiting\n");
        return 0;
    }

    for (;;)
    {
        int rc = poll(fds, count, -1);
        if (rc < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("poll");
            sleep(1);
            continue;
        }

        for (int i = 0; i < count; i++)
        {
            if (!(fds[i].revents & POLLIN))
            {
                continue;
            }

            struct sockaddr_in peer;
            socklen_t peerLen = sizeof(peer);
            int conn = accept(fds[i].fd, (struct sockaddr*)&peer, &peerLen);
            if (conn < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                perror("accept");
                sleep(1);
                continue;
            }

            char peerStr[INET_ADDRSTRLEN] = "?";
            inet_ntop(AF_INET, &peer.sin_addr, peerStr, sizeof(peerStr));
            fprintf(stderr, "HOST-IF connection from %s:%u on port %d\n",
                    peerStr, (unsigned)ntohs(peer.sin_port), portOf[i]);

            unsigned char buf[REQ_MAX];
            ssize_t n = read(conn, buf, sizeof(buf) - 1);
            if (n > 0)
            {
                if (portOf[i] == 443)
                {
                    logHex("TLS", buf, n);
                }
                else
                {
                    logRequest((char*)buf, n);
                }
            }
            else
            {
                fprintf(stderr, "HOST-IF empty read on port %d\n", portOf[i]);
            }

            if (portOf[i] == 80)
            {
                ssize_t ignored = write(conn, response, sizeof(response) - 1);
                (void)ignored;
            }
            close(conn);
            fflush(stderr);
        }
    }
}
