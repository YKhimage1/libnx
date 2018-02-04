#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
//#include <stdlib.h>
#include <string.h>
#include <malloc.h>
//#include <sys/dirent.h>
#include <sys/iosupport.h>
#include <sys/param.h>

#include <unistd.h>

#include <sys/select.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <netinet/in.h>
#include <net/bpf.h>

#include <arpa/inet.h>

#include "runtime/devices/socket.h"
#include "services/bsd.h"
#include "result.h"

const struct in6_addr in6addr_any = {0};
const struct in6_addr in6addr_loopback = {.__u6_addr32 = {0, 0, 0, __builtin_bswap32(1)}};

static int _socketOpen(struct _reent *r, void *fdptr, const char *path, int flags, int mode);
static int _socketClose(struct _reent *r, void *fdptr);
static ssize_t _socketWrite(struct _reent *r, void *fdptr, const char *buf, size_t count);
static ssize_t _socketRead(struct _reent *r, void *fdptr, char *buf, size_t count);

static const devoptab_t g_socketDevoptab = {
    .name = "soc",
    .structSize   = sizeof(int),
    .open_r       = _socketOpen,
    .close_r      = _socketClose,
    .write_r      = _socketWrite,
    .read_r       = _socketRead,
    .seek_r       = NULL,
    .fstat_r      = NULL,
    .stat_r       = NULL,
    .link_r       = NULL,
    .unlink_r     = NULL,
    .chdir_r      = NULL,
    .rename_r     = NULL,
    .mkdir_r      = NULL,
    .dirStateSize = 0,
    .diropen_r    = NULL,
    .dirreset_r   = NULL,
    .dirnext_r    = NULL,
    .dirclose_r   = NULL,
    .statvfs_r    = NULL,
    .ftruncate_r  = NULL,
    .fsync_r      = NULL,
    .deviceData   = 0,
    .chmod_r      = NULL,
    .fchmod_r     = NULL,
    .rmdir_r      = NULL,
};

static const SocketInitConfig g_defaultSocketInitConfig = {
    .bsdsockets_version = 1,

    .tcp_tx_buf_size        = 0x8000,
    .tcp_rx_buf_size        = 0x10000,
    .tcp_tx_buf_max_size    = 0x40000,
    .tcp_rx_buf_max_size    = 0x40000,

    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0xA500,

    .sb_efficiency = 4,
};

static int _socketParseBsdResult(struct _reent *r, int ret) {
    int errno_;
    if(ret != -1)
        return ret; // Nothing to do
    else {
        if(g_bsdErrno == -1) {
            // We're using -1 to signal Switch error codes.
            // Note: all of the bsd:u/s handlers return 0.
            switch(g_bsdResult) {
                case 0xD201:
                    errno_ = ENFILE;
                case 0xD401:
                    errno_ = EFAULT;
                    break;
                case 0x10601:
                    errno_ = EBUSY;
                    break;
                default:
                    errno_ = EPIPE;
                    break;
            }
        }
        else
            errno_ = g_bsdErrno; // Nintendo actually used the Linux errno definitions for their FreeBSD build :)
    }

    if(r == NULL)
        errno = errno_;
    else
        r->_errno = errno_;

    return -1;
}

static int _socketGetFd(int fd) {
    __handle *handle = __get_handle(fd);
    if(handle == NULL) {
        errno = EBADF;
        return -1;
    }
    if(strcmp(devoptab_list[handle->device]->name, "soc") != 0) {
        errno = ENOTSOCK;
        return -1;
    }
    return *(int *)handle->fileStruct;
}

static int _socketOpen(struct _reent *r, void *fdptr, const char *path, int flags, int mode) {
    (void)mode;
    int ret = _socketParseBsdResult(r, bsdOpen(path, flags));
    if(ret == -1)
        return ret;

    *(int *)fdptr = ret;
    return 0;
}

static int _socketClose(struct _reent *r, void *fdptr) {
    int fd = *(int *)fdptr;
    return _socketParseBsdResult(r, bsdClose(fd));
}

static ssize_t _socketWrite(struct _reent *r, void *fdptr, const char *buf, size_t count) {
    int fd = *(int *)fdptr;
    ssize_t ret = bsdWrite(fd, buf, count);

    _socketParseBsdResult(r, (int)ret);
    return ret;
}

static ssize_t _socketRead(struct _reent *r, void *fdptr, char *buf, size_t count) {
    int fd = *(int *)fdptr;
    ssize_t ret = bsdRead(fd, buf, count);

    _socketParseBsdResult(r, (int)ret);
    return ret;
}

// Adapted from ctrulib
static int _socketInetAtonDetail(int *outBase, size_t *outNumBytes, const char *cp, struct in_addr *inp) {
    int      base;
    u32      val;
    int      c;
    char     bytes[4];
    size_t   num_bytes = 0;

    c = *cp;
    for(;;) {
        if(!isdigit(c)) return 0;

        val = 0;
        base = 10;
        if(c == '0') {
            c = *++cp;
            if(c == 'x' || c == 'X') {
                base = 16;
                c = *++cp;
            }
            else base = 8;
        }

        for(;;) {
            if(isdigit(c)) {
                if(base == 8 && c >= '8') return 0;
                val *= base;
                val += c - '0';
                c    = *++cp;
            }
            else if(base == 16 && isxdigit(c)) {
                val *= base;
                val += c + 10 - (islower(c) ? 'a' : 'A');
                c    = *++cp;
            }
            else break;
        }

        if(c == '.') {
            if(num_bytes > 3) return 0;
            if(val > 0xFF) return 0;
            bytes[num_bytes++] = val;
            c = *++cp;
        }
        else break;
    }

    if(c != 0) {
        *outNumBytes = num_bytes;
        *outBase = base;
        return 0;
    }

    switch(num_bytes) {
    case 0:
        break;

    case 1:
        if(val > 0xFFFFFF) return 0;
        val |= bytes[0] << 24;
        break;

    case 2:
        if(val > 0xFFFF) return 0;
        val |= bytes[0] << 24;
        val |= bytes[1] << 16;
        break;

    case 3:
        if(val > 0xFF) return 0;
        val |= bytes[0] << 24;
        val |= bytes[1] << 16;
        val |= bytes[2] << 8;
        break;
    }

    if(inp)
        inp->s_addr = htonl(val);

    *outNumBytes = num_bytes;
    *outBase = base;

    return 1;
}

// Adapted from ctrulib
static const char *inet_ntop4(const void *src, char *dst, socklen_t size) {
    const u8 *ip = src;

    char *p;
    size_t i;
    unsigned int n;

    if(size < INET_ADDRSTRLEN) {
        errno = ENOSPC;
        return NULL;
    }

    for(p = dst, i = 0; i < 4; ++i) {
        if(i > 0) *p++ = '.';

        n = ip[i];
        if(n >= 100) {
            *p++ = n/100 + '0';
            n %= 100;
        }
        if(n >= 10 || ip[i] >= 100) {
            *p++ = n/10 + '0';
            n %= 10;
        }
        *p++ = n + '0';
    }
    *p = 0;

    return dst;
}

static int inet_pton4(const char *src, void *dst) {
    int base;
    size_t numBytes;

    int ret = _socketInetAtonDetail(&base, &numBytes, src, (struct in_addr *)dst);
    return (ret == 1 && base == 10 && numBytes == 4) ? 1 : 0;
}

// ==============================================================

/* Copyright (c) 1996 by Internet Software Consortium.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
 * CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

#define INADDRSZ 4
#define IN6ADDRSZ 16
#define INT16SZ 2
/* const char *
 * inet_ntop6(src, dst, size)
 *	convert IPv6 binary address into presentation (printable) format
 * author:
 *	Paul Vixie, 1996.
 */
static const char *
inet_ntop6(src, dst, size)
    const u_char *src;
    char *dst;
    size_t size;
{
    /*
     * Note that int32_t and int16_t need only be "at least" large enough
     * to contain a value of the specified size.  On some systems, like
     * Crays, there is no such thing as an integer variable with 16 bits.
     * Keep this in mind if you think this function should have been coded
     * to use pointer overlays.  All the world's not a VAX.
     */
    char tmp[sizeof "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255"], *tp;
    struct { int base, len; } best = {0}, cur = {0};
    u_int words[IN6ADDRSZ / INT16SZ];
    int i;

    /*
     * Preprocess:
     *	Copy the input (bytewise) array into a wordwise array.
     *	Find the longest run of 0x00's in src[] for :: shorthanding.
     */
    memset(words, 0, sizeof words);
    for (i = 0; i < IN6ADDRSZ; i++)
        words[i / 2] |= (src[i] << ((1 - (i % 2)) << 3));
    best.base = -1;
    cur.base = -1;
    for (i = 0; i < (IN6ADDRSZ / INT16SZ); i++) {
        if (words[i] == 0) {
            if (cur.base == -1)
                cur.base = i, cur.len = 1;
            else
                cur.len++;
        } else {
            if (cur.base != -1) {
                if (best.base == -1 || cur.len > best.len)
                    best = cur;
                cur.base = -1;
            }
        }
    }
    if (cur.base != -1) {
        if (best.base == -1 || cur.len > best.len)
            best = cur;
    }
    if (best.base != -1 && best.len < 2)
        best.base = -1;

    /*
     * Format the result.
     */
    tp = tmp;
    for (i = 0; i < (IN6ADDRSZ / INT16SZ); i++) {
        /* Are we inside the best run of 0x00's? */
        if (best.base != -1 && i >= best.base &&
            i < (best.base + best.len)) {
            if (i == best.base)
                *tp++ = ':';
            continue;
        }
        /* Are we following an initial run of 0x00s or any real hex? */
        if (i != 0)
            *tp++ = ':';
        /* Is this address an encapsulated IPv4? */
        if (i == 6 && best.base == 0 &&
            (best.len == 6 || (best.len == 5 && words[5] == 0xffff))) {
            if (!inet_ntop4(src+12, tp, sizeof tmp - (tp - tmp)))
                return (NULL);
            tp += strlen(tp);
            break;
        }
        //TuxSH:
        //sprintf(tp, "%x", words[i]);
        {
            char *e = tp + 7;
            memset(e, '0', 4);
            u_int word = words[i];
            while(word > 0) {
                static const char digits[] = "0123456789abcdef";
                *e-- = digits[word & 0xF];
                word >>= 4;
            }
        }
        tp += strlen(tp);
    }
    /* Was it a trailing run of 0x00's? */
    if (best.base != -1 && (best.base + best.len) == (IN6ADDRSZ / INT16SZ))
        *tp++ = ':';
    *tp++ = '\0';

    /*
     * Check for overflow, copy, and we're done.
     */
    if ((tp - tmp) > size) {
        errno = ENOSPC;
        return (NULL);
    }
    strcpy(dst, tmp);
    return (dst);
}

/* int
 * inet_pton6(src, dst)
 *	convert presentation level address to network order binary form.
 * return:
 *	1 if `src' is a valid [RFC1884 2.2] address, else 0.
 * notice:
 *	(1) does not touch `dst' unless it's returning 1.
 *	(2) :: in a full address is silently ignored.
 * credit:
 *	inspired by Mark Andrews.
 * author:
 *	Paul Vixie, 1996.
 */
static int
inet_pton6(src, dst)
    const char *src;
    u_char *dst;
{
    static const char xdigits_l[] = "0123456789abcdef",
              xdigits_u[] = "0123456789ABCDEF";
    u_char tmp[IN6ADDRSZ], *tp, *endp, *colonp;
    const char *xdigits, *curtok;
    int ch, saw_xdigit;
    u_int val;

    memset((tp = tmp), 0, IN6ADDRSZ);
    endp = tp + IN6ADDRSZ;
    colonp = NULL;
    /* Leading :: requires some special handling. */
    if (*src == ':')
        if (*++src != ':')
            return (0);
    curtok = src;
    saw_xdigit = 0;
    val = 0;
    while ((ch = *src++) != '\0') {
        const char *pch;

        if ((pch = strchr((xdigits = xdigits_l), ch)) == NULL)
            pch = strchr((xdigits = xdigits_u), ch);
        if (pch != NULL) {
            val <<= 4;
            val |= (pch - xdigits);
            if (val > 0xffff)
                return (0);
            saw_xdigit = 1;
            continue;
        }
        if (ch == ':') {
            curtok = src;
            if (!saw_xdigit) {
                if (colonp)
                    return (0);
                colonp = tp;
                continue;
            }
            if (tp + INT16SZ > endp)
                return (0);
            *tp++ = (u_char) (val >> 8) & 0xff;
            *tp++ = (u_char) val & 0xff;
            saw_xdigit = 0;
            val = 0;
            continue;
        }
        if (ch == '.' && ((tp + INADDRSZ) <= endp) &&
            inet_pton4(curtok, tp) > 0) {
            tp += INADDRSZ;
            saw_xdigit = 0;
            break;	/* '\0' was seen by inet_pton4(). */
        }
        return (0);
    }
    if (saw_xdigit) {
        if (tp + INT16SZ > endp)
            return (0);
        *tp++ = (u_char) (val >> 8) & 0xff;
        *tp++ = (u_char) val & 0xff;
    }
    if (colonp != NULL) {
        /*
         * Since some memmove()'s erroneously fail to handle
         * overlapping regions, we'll do the shift by hand.
         */
        const int n = tp - colonp;
        int i;

        for (i = 1; i <= n; i++) {
            endp[- i] = colonp[n - i];
            colonp[n - i] = 0;
        }
        tp = endp;
    }
    if (tp != endp)
        return (0);
    /* bcopy(tmp, dst, IN6ADDRSZ); */
    memcpy(dst, tmp, IN6ADDRSZ);
    return (1);
}
// ==============================================================

const SocketInitConfig *socketGetDefaultInitConfig(void) {
    return &g_defaultSocketInitConfig;
}

Result socketInitialize(const SocketInitConfig *config) {
    Result ret = 0;
    BsdInitConfig bcfg = {
        .version = config->bsdsockets_version,

        .tcp_tx_buf_size        = config->tcp_tx_buf_size,
        .tcp_rx_buf_size        = config->tcp_rx_buf_size,
        .tcp_tx_buf_max_size    = config->tcp_tx_buf_max_size,
        .tcp_rx_buf_max_size    = config->tcp_rx_buf_max_size,

        .udp_tx_buf_size = config->udp_tx_buf_size,
        .udp_rx_buf_size = config->udp_rx_buf_size,

        .sb_efficiency = config->sb_efficiency,
    };

    int dev = FindDevice("soc:");
    if(dev != -1)
        return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);

    ret = bsdInitialize(&bcfg);

    if(R_SUCCEEDED(ret))
        dev = AddDevice(&g_socketDevoptab);

    if(dev == -1) {
        socketExit();
        return MAKERESULT(Module_Libnx, LibnxError_TooManyDevOpTabs);
    }

    return ret;
}

void socketExit(void) {
    RemoveDevice("soc:");
    bsdExit();
}

Result socketGetLastResult(void) {
    return g_bsdResult;
}

/*
    It is way too complicated and inefficient to use devoptab with bsdSelect.
    We're therefore implementing select with poll.

    Code copied from libctru.
*/
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    struct pollfd *pollinfo;
    nfds_t numfds = 0;
    size_t i, j;
    int rc, found;

    if(nfds >= FD_SETSIZE || nfds < 0) {
        errno = EINVAL;
        return -1;
    }

    for(i = 0; i < nfds; ++i) {
        if((readfds && FD_ISSET(i, readfds))
        || (writefds && FD_ISSET(i, writefds))
        || (exceptfds && FD_ISSET(i, exceptfds)))
            ++numfds;
    }

    pollinfo = (struct pollfd*)malloc(numfds * sizeof(struct pollfd));
    if(pollinfo == NULL) {
        errno = ENOMEM;
        return -1;
    }

    for(i = 0, j = 0; i < nfds; ++i) {
        if((readfds && FD_ISSET(i, readfds))
        || (writefds && FD_ISSET(i, writefds))
        || (exceptfds && FD_ISSET(i, exceptfds))) {
            pollinfo[j].fd      = i;
            pollinfo[j].events  = 0;
            pollinfo[j].revents = 0;

            if(readfds && FD_ISSET(i, readfds))
                pollinfo[j].events |= POLLIN;
            if(writefds && FD_ISSET(i, writefds))
                pollinfo[j].events |= POLLOUT;

            ++j;
        }
    }

    if(timeout)
        rc = poll(pollinfo, numfds, timeout->tv_sec*1000 + timeout->tv_usec/1000);
    else
        rc = poll(pollinfo, numfds, -1);

    if(rc < 0) {
        free(pollinfo);
        return rc;
    }

    for(i = 0, j = 0, rc = 0; i < nfds; ++i) {
        found = 0;

        if((readfds && FD_ISSET(i, readfds))
        || (writefds && FD_ISSET(i, writefds))
        || (exceptfds && FD_ISSET(i, exceptfds))) {

            if(readfds && FD_ISSET(i, readfds)) {
                if(pollinfo[j].events & (POLLIN|POLLHUP))
                    found = 1;
                else
                    FD_CLR(i, readfds);
            }

            if(writefds && FD_ISSET(i, writefds)) {
                if(pollinfo[j].events & (POLLOUT|POLLHUP))
                    found = 1;
                else
                    FD_CLR(i, writefds);
            }

            if(exceptfds && FD_ISSET(i, exceptfds)) {
                if(pollinfo[j].events & POLLERR)
                    found = 1;
                else
                    FD_CLR(i, exceptfds);
            }

            if(found)
                ++rc;
            ++j;
        }
    }

    free(pollinfo);

    return rc;
}

// This is much saner than select.
int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    struct pollfd *fds2;
    int ret = 0;

    if(fds == NULL) {
        errno = EFAULT;
        return -1;
    }

    fds2 = (struct pollfd *)malloc(nfds * sizeof(struct pollfd));
    if(fds2 == NULL) {
        errno = ENOMEM;
        return -1;
    }

    for(nfds_t i = 0; i < nfds; i++) {
        fds2[i].events = fds[i].events;
        fds2[i].revents = fds[i].revents;
        fds2[i].fd = _socketGetFd(fds[i].fd);
        if(fds2[i].fd == -1) {
            ret = -1;
            break;
        }
    }

    if(ret != -1)
        ret = _socketParseBsdResult(NULL, bsdPoll(fds2, nfds, timeout));
    if(ret != -1) {
        for(nfds_t i = 0; i < nfds; i++) {
            fds[i].events = fds2[i].events;
            fds[i].revents = fds2[i].revents;
        }
    }

    free(fds2);
    return ret;
}

int sysctl(const int *name, unsigned int namelen, void *oldp, size_t *oldlenp, const void *newp, size_t newlen) {
    return _socketParseBsdResult(NULL, bsdSysctl(name, namelen, oldp, oldlenp, newp, newlen));
}

int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, const void *newp, size_t newlen) {
    int mib[CTL_MAXNAME + 2]; // +2 because most of the relevant code I've seen uses +2 as well
    size_t miblen = CTL_MAXNAME + 2;

    if(sysctlnametomib(name, mib, &miblen) == -1)
        return -1;

    return sysctl(mib, miblen, oldp, oldlenp, newp, newlen);
}

int sysctlnametomib(const char *name, int *mibp, size_t *sizep) {
    int oid[2] = {0, 3}; // sysctl.name2oid
    int ret;
    size_t oldlenp;

    if(name == NULL || mibp == NULL || sizep == NULL) {
        errno = EFAULT;
        return -1;
    }

    oldlenp = 4 * (*sizep);
    ret = sysctl(oid, 2, mibp, &oldlenp, name, strlen(name));
    *sizep = oldlenp / 4;

    return ret;
}

int socket(int domain, int type, int protocol) {
    int ret, fd, dev;

    dev = FindDevice("soc:");
    if(dev == -1)
        return -1;

    fd = __alloc_handle(dev);
    if(fd == -1)
        return -1;

    ret = _socketParseBsdResult(NULL, bsdSocket(domain, type, protocol));
    if(ret == -1) {
        __release_handle(fd);
        return -1;
    }
    else {
        *(int *)__get_handle(fd)->fileStruct = ret;
        return fd;
    }
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    ssize_t ret;
    sockfd = _socketGetFd(sockfd);
    if(sockfd == -1)
        return -1;
    ret = bsdRecv(sockfd, buf, len, flags);
    return _socketParseBsdResult(NULL, (int)ret);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen) {
    ssize_t ret;
    sockfd = _socketGetFd(sockfd);
    if(sockfd == -1)
        return -1;
    ret = bsdRecvFrom(sockfd, buf, len, flags, src_addr, addrlen);
    return _socketParseBsdResult(NULL, (int)ret);
}

ssize_t send(int sockfd, const void* buf, size_t len, int flags) {
    ssize_t ret;
    sockfd = _socketGetFd(sockfd);
    if(sockfd == -1)
        return -1;
    ret = bsdSend(sockfd, buf, len, flags);
    return _socketParseBsdResult(NULL, (int)ret);
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen) {
    ssize_t ret;
    sockfd = _socketGetFd(sockfd);
    if(sockfd == -1)
        return -1;
    ret = bsdSendTo(sockfd, buf, len, flags, dest_addr, addrlen);
    return _socketParseBsdResult(NULL, (int)ret);
}

int accept(int sockfd, struct sockaddr *address, socklen_t *addrlen) {
    int ret, fd, dev;

    fd = _socketGetFd(sockfd);
    if(fd == -1)
        return -1;

    dev = __get_handle(sockfd)->device;
    sockfd = fd;

    fd = __alloc_handle(dev);
    if(fd == -1)
        return -1;

    ret = _socketParseBsdResult(NULL, bsdAccept(sockfd, address, addrlen));
    if(ret == -1) {
        __release_handle(fd);
        return -1;
    }
    else {
        *(int *)__get_handle(fd)->fileStruct = ret;
        return fd;
    }
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    sockfd = _socketGetFd(sockfd);
    if(sockfd == -1)
        return -1;
    return _socketParseBsdResult(NULL, bsdBind(sockfd, addr, addrlen));
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    sockfd = _socketGetFd(sockfd);
    if(sockfd == -1)
        return -1;
    return _socketParseBsdResult(NULL, bsdConnect(sockfd, addr, addrlen));
}

int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    sockfd = _socketGetFd(sockfd);
    if(sockfd == -1)
        return -1;
    return _socketParseBsdResult(NULL, bsdGetPeerName(sockfd, addr, addrlen));
}

int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    sockfd = _socketGetFd(sockfd);
    if(sockfd == -1)
        return -1;
    return _socketParseBsdResult(NULL, bsdGetSockName(sockfd, addr, addrlen));
}

int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen) {
    sockfd = _socketGetFd(sockfd);
    if(sockfd == -1)
        return -1;
    return _socketParseBsdResult(NULL, bsdGetSockOpt(sockfd, level, optname, optval, optlen));
}

int listen(int sockfd, int backlog) {
    sockfd = _socketGetFd(sockfd);
    if(sockfd == -1)
        return -1;
    return _socketParseBsdResult(NULL, bsdListen(sockfd, backlog));
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
    sockfd = _socketGetFd(sockfd);
    if(sockfd == -1)
        return -1;
    return _socketParseBsdResult(NULL, bsdSetSockOpt(sockfd, level, optname, optval, optlen));
}

int ioctl(int fd, int request, ...) {
    void *data;
    va_list args;

    va_start(args, request);
    data = (request & IOC_INOUT) ? va_arg(args, void *) : NULL;
    va_end(args);

    if(data == NULL && (request & IOC_INOUT) && IOCPARM_LEN(request) != 0) {
        errno = EFAULT;
        return -1;
    }

    fd = request != FIONBIO ? _socketGetFd(fd) : fd;
    if(fd == -1)
        return -1;

    switch(request) {
        case FIONBIO: {
            // See note in fcntl (below)
            int flags = fcntl(fd, F_GETFL, 0);
            if(flags == -1)
                return -1;
            flags = *(int *)data != 0 ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
            return fcntl(fd, F_SETFL, 0);
        }
        case BIOCSETF:
        case BIOCSETWF:
        case BIOCSETFNR: {
            int ret;
            struct bpf_program *prog = (struct bpf_program *)data;
            if(prog->bf_len > BPF_MAXBUFSIZE) {
                errno = EINVAL;
                return -1;
            }

            struct bpf_program_serialized *prog_ser = (struct bpf_program_serialized *)malloc(sizeof(struct bpf_program_serialized));
            if(prog_ser == NULL) {
                errno = ENOMEM;
                return -1;
            }

            prog_ser->bf_len = prog->bf_len;
            memcpy(prog_ser->bf_insns, prog->bf_insns, prog->bf_len);

            request = _IOC(request & IOC_DIRMASK, IOCGROUP(request), IOCBASECMD(request), sizeof(struct bpf_program_serialized));
            ret = bsdIoctl(fd, request, prog_ser);
            free(prog_ser);
            return _socketParseBsdResult(NULL, ret);
        }
        default:
            return _socketParseBsdResult(NULL, bsdIoctl(fd, request, data));
    }
}

int fcntl(int fd, int cmd, ...) {
    va_list args;
    int flags;

    /*
        bsd:u/s only supports F_GETFL and F_SETFL with the O_NONBLOCK flag (or 0).
        F_GETFL is implemented using a custom, non-whitelisted IOCTL, whereas
        F_SETFL is implemented using FIONBIO.
    */
    if(cmd != F_GETFL && cmd != F_SETFL)
        return EOPNOTSUPP;
    va_start(args, cmd);
    flags = va_arg(args, int);
    va_end(args);

    fd = _socketGetFd(fd);
    if(fd == -1)
        return -1;
    return _socketParseBsdResult(NULL, bsdFcntl(fd, cmd, flags));
}

int shutdown(int sockfd, int how) {
    sockfd = _socketGetFd(sockfd);
    if(sockfd == -1)
        return -1;
    return _socketParseBsdResult(NULL, bsdShutdown(sockfd, how));
}

int sockatmark(int sockfd) {
    int atmark;
    return ioctl(sockfd, SIOCATMARK, &atmark) == -1 ? -1 : atmark;
}

int socketpair(int domain, int type, int protocol, int sv[2]) {
    // Unimplementable, function definition written for compliance
    errno = ENOSYS;
    return -1;
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
    if(msg == NULL) {
        errno = EFAULT;
        return -1;
    }

    struct mmsghdr msgvec = {
        .msg_hdr = *msg,
        .msg_len = 1,
    };

    return recvmmsg(sockfd, &msgvec, 1, flags, NULL);
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
    if(msg == NULL) {
        errno = EFAULT;
        return -1;
    }

    struct mmsghdr msgvec = {
        .msg_hdr = *msg,
        .msg_len = 1,
    };

    return sendmmsg(sockfd, &msgvec, 1, flags);
}

int sendmmsg(int sockfd, struct mmsghdr *msgvec, unsigned int vlen, int flags) {
    //TODO: do the necessary RE & implement it
    errno = ENOSYS;
    return -1;
}
int recvmmsg(int sockfd, struct mmsghdr *msgvec, unsigned int vlen, int flags, struct timespec *timeout) {
    //TODO: do the necessary RE & implement it
    errno = ENOSYS;
    return -1;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    switch(af) {
        case AF_INET:
            return inet_ntop4(src, dst, size);
        case AF_INET6:
            return inet_ntop6(src, dst, size);
        default:
            errno = EAFNOSUPPORT;
            return NULL;
    }
}

int inet_pton(int af, const char *src, void *dst) {
    switch(af) {
        case AF_INET:
            return inet_pton4(src, dst);
        case AF_INET6:
            return inet_pton6(src, dst);
        default:
            errno = EAFNOSUPPORT;
            return -1;
    }
}

char *inet_ntoa(struct in_addr in) {
    static char buffer[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &in.s_addr, buffer, INET_ADDRSTRLEN);
    return buffer;
}

int inet_aton(const char *cp, struct in_addr *inp) {
    int base;
    size_t numBytes;
    return _socketInetAtonDetail(&base, &numBytes, cp, inp);
}

in_addr_t inet_addr(const char *cp) {
    struct in_addr addr = { .s_addr = INADDR_BROADCAST };
    inet_aton(cp, &addr);
    return addr.s_addr;
}

long gethostid(void) {
    return INADDR_LOOPBACK; //FIXME
}

int gethostname(char *name, size_t namelen)
{
    //FIXME
    long hostid = gethostid();
    const char *hostname = inet_ntop(AF_INET, &hostid, name, namelen);
    return hostname == NULL ? -1 : 0;
}