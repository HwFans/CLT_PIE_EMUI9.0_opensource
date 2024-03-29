/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Haivision Open SRT (Secure Reliable Transport) protocol
 */

#include <srt/srt.h>

#include "libavutil/avassert.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/avtime.h"

#include "avformat.h"
#include "internal.h"
#include "network.h"
#include "os_support.h"
#include "url.h"

enum SRTMode {
    SRT_MODE_CALLER = 0,
    SRT_MODE_LISTENER = 1,
    SRT_MODE_RENDEZVOUS = 2
};

typedef struct SRTContext {
    const AVClass *class;
    int fd;
    int eid;
    int64_t rw_timeout;
    int64_t listen_timeout;
    int recv_buffer_size;
    int send_buffer_size;

    int64_t maxbw;
    int pbkeylen;
    char *passphrase;
    int mss;
    int ffs;
    int ipttl;
    int iptos;
    int64_t inputbw;
    int oheadbw;
    int64_t tsbpddelay;
    int tlpktdrop;
    int nakreport;
    int64_t connect_timeout;
    enum SRTMode mode;
} SRTContext;

#define D AV_OPT_FLAG_DECODING_PARAM
#define E AV_OPT_FLAG_ENCODING_PARAM
#define OFFSET(x) offsetof(SRTContext, x)
static const AVOption libsrt_options[] = {
    { "rw_timeout",     "Timeout of socket I/O operations",                                     OFFSET(rw_timeout),       AV_OPT_TYPE_INT64, { .i64 = -1 }, -1, INT64_MAX, .flags = D|E },
    { "listen_timeout", "Connection awaiting timeout",                                          OFFSET(listen_timeout),   AV_OPT_TYPE_INT64, { .i64 = -1 }, -1, INT64_MAX, .flags = D|E },
    { "send_buffer_size", "Socket send buffer size (in bytes)",                                 OFFSET(send_buffer_size), AV_OPT_TYPE_INT,      { .i64 = -1 }, -1, INT_MAX,   .flags = D|E },
    { "recv_buffer_size", "Socket receive buffer size (in bytes)",                              OFFSET(recv_buffer_size), AV_OPT_TYPE_INT,      { .i64 = -1 }, -1, INT_MAX,   .flags = D|E },
    { "maxbw",          "Maximum bandwidth (bytes per second) that the connection can use",     OFFSET(maxbw),            AV_OPT_TYPE_INT64,    { .i64 = -1 }, -1, INT64_MAX, .flags = D|E },
    { "pbkeylen",       "Crypto key len in bytes {16,24,32} Default: 16 (128-bit)",             OFFSET(pbkeylen),         AV_OPT_TYPE_INT,      { .i64 = -1 }, -1, 32,        .flags = D|E },
    { "passphrase",     "Crypto PBKDF2 Passphrase size[0,10..64] 0:disable crypto",             OFFSET(passphrase),       AV_OPT_TYPE_STRING,   { .str = NULL },              .flags = D|E },
    { "mss",            "The Maximum Segment Size",                                             OFFSET(mss),              AV_OPT_TYPE_INT,      { .i64 = -1 }, -1, 1500,      .flags = D|E },
    { "ffs",            "Flight flag size (window size) (in bytes)",                            OFFSET(ffs),              AV_OPT_TYPE_INT,      { .i64 = -1 }, -1, INT_MAX,   .flags = D|E },
    { "ipttl",          "IP Time To Live",                                                      OFFSET(ipttl),            AV_OPT_TYPE_INT,      { .i64 = -1 }, -1, 255,       .flags = D|E },
    { "iptos",          "IP Type of Service",                                                   OFFSET(iptos),            AV_OPT_TYPE_INT,      { .i64 = -1 }, -1, 255,       .flags = D|E },
    { "inputbw",        "Estimated input stream rate",                                          OFFSET(inputbw),          AV_OPT_TYPE_INT64,    { .i64 = -1 }, -1, INT64_MAX, .flags = D|E },
    { "oheadbw",        "MaxBW ceiling based on % over input stream rate",                      OFFSET(oheadbw),          AV_OPT_TYPE_INT,      { .i64 = -1 }, -1, 100,       .flags = D|E },
    { "tsbpddelay",     "TsbPd receiver delay to absorb burst of missed packet retransmission", OFFSET(tsbpddelay),       AV_OPT_TYPE_INT64, { .i64 = -1 }, -1, INT64_MAX, .flags = D|E },
    { "tlpktdrop",      "Enable receiver pkt drop",                                             OFFSET(tlpktdrop),        AV_OPT_TYPE_INT,      { .i64 = -1 }, -1, 1,         .flags = D|E },
    { "nakreport",      "Enable receiver to send periodic NAK reports",                         OFFSET(nakreport),        AV_OPT_TYPE_INT,      { .i64 = -1 }, -1, 1,         .flags = D|E },
    { "connect_timeout", "Connect timeout. Caller default: 3000, rendezvous (x 10)",            OFFSET(connect_timeout),  AV_OPT_TYPE_INT64, { .i64 = -1 }, -1, INT64_MAX, .flags = D|E },
    { "mode",           "Connection mode (caller, listener, rendezvous)",                       OFFSET(mode),             AV_OPT_TYPE_INT,      { .i64 = SRT_MODE_CALLER }, SRT_MODE_CALLER, SRT_MODE_RENDEZVOUS, .flags = D|E, "mode" },
    { "caller",         NULL, 0, AV_OPT_TYPE_CONST,  { .i64 = SRT_MODE_CALLER },     INT_MIN, INT_MAX, .flags = D|E, "mode" },
    { "listener",       NULL, 0, AV_OPT_TYPE_CONST,  { .i64 = SRT_MODE_LISTENER },   INT_MIN, INT_MAX, .flags = D|E, "mode" },
    { "rendezvous",     NULL, 0, AV_OPT_TYPE_CONST,  { .i64 = SRT_MODE_RENDEZVOUS }, INT_MIN, INT_MAX, .flags = D|E, "mode" },
    { NULL }
};

static int libsrt_neterrno(URLContext *h)
{
    int err = srt_getlasterror(NULL);
    av_log(h, AV_LOG_ERROR, "%s\n", srt_getlasterror_str());
    if (err == SRT_EASYNCRCV)
        return AVERROR(EAGAIN);
    return AVERROR_UNKNOWN;
}

static int libsrt_socket_nonblock(int socket, int enable)
{
    int ret = srt_setsockopt(socket, 0, SRTO_SNDSYN, &enable, sizeof(enable));
    if (ret < 0)
        return ret;
    return srt_setsockopt(socket, 0, SRTO_RCVSYN, &enable, sizeof(enable));
}

static int libsrt_network_wait_fd(URLContext *h, int eid, int fd, int write)
{
    int ret, len = 1;
    int modes = write ? SRT_EPOLL_OUT : SRT_EPOLL_IN;
    SRTSOCKET ready[1];

    if (srt_epoll_add_usock(eid, fd, &modes) < 0)
        return libsrt_neterrno(h);
    if (write) {
        ret = srt_epoll_wait(eid, 0, 0, ready, &len, POLLING_TIME, 0, 0, 0, 0);
    } else {
        ret = srt_epoll_wait(eid, ready, &len, 0, 0, POLLING_TIME, 0, 0, 0, 0);
    }
    if (ret < 0) {
        if (srt_getlasterror(NULL) == SRT_ETIMEOUT)
            ret = AVERROR(EAGAIN);
        else
            ret = libsrt_neterrno(h);
    } else {
        ret = 0;
    }
    if (srt_epoll_remove_usock(eid, fd) < 0)
        return libsrt_neterrno(h);
    return ret;
}

/* TODO de-duplicate code from ff_network_wait_fd_timeout() */

static int libsrt_network_wait_fd_timeout(URLContext *h, int eid, int fd, int write, int64_t timeout, AVIOInterruptCB *int_cb)
{
    int ret;
    int64_t wait_start = 0;

    while (1) {
        if (ff_check_interrupt(int_cb))
            return AVERROR_EXIT;
        ret = libsrt_network_wait_fd(h, eid, fd, write);
        if (ret != AVERROR(EAGAIN))
            return ret;
        if (timeout > 0) {
            if (!wait_start)
                wait_start = av_gettime_relative();
            else if (av_gettime_relative() - wait_start > timeout)
                return AVERROR(ETIMEDOUT);
        }
    }
}

static int libsrt_listen(int eid, int fd, const struct sockaddr *addr, socklen_t addrlen, URLContext *h, int timeout)
{
    int ret;
    int reuse = 1;
    if (srt_setsockopt(fd, SOL_SOCKET, SRTO_REUSEADDR, &reuse, sizeof(reuse))) {
        av_log(h, AV_LOG_WARNING, "setsockopt(SRTO_REUSEADDR) failed\n");
    }
    ret = srt_bind(fd, addr, addrlen);
    if (ret)
        return libsrt_neterrno(h);

    ret = srt_listen(fd, 1);
    if (ret)
        return libsrt_neterrno(h);

    while ((ret = libsrt_network_wait_fd_timeout(h, eid, fd, 1, timeout, &h->interrupt_callback))) {
        switch (ret) {
        case AVERROR(ETIMEDOUT):
            continue;
        default:
            return ret;
        }
    }

    ret = srt_accept(fd, NULL, NULL);
    if (ret < 0)
        return libsrt_neterrno(h);
    if (libsrt_socket_nonblock(ret, 1) < 0)
        av_log(h, AV_LOG_DEBUG, "libsrt_socket_nonblock failed\n");

    return ret;
}

static int libsrt_listen_connect(int eid, int fd, const struct sockaddr *addr, socklen_t addrlen, int timeout, URLContext *h, int will_try_next)
{
    int ret;

    if (libsrt_socket_nonblock(fd, 1) < 0)
        av_log(h, AV_LOG_DEBUG, "ff_socket_nonblock failed\n");

    while ((ret = srt_connect(fd, addr, addrlen))) {
        ret = libsrt_neterrno(h);
        switch (ret) {
        case AVERROR(EINTR):
            if (ff_check_interrupt(&h->interrupt_callback))
                return AVERROR_EXIT;
            continue;
        case AVERROR(EINPROGRESS):
        case AVERROR(EAGAIN):
            ret = libsrt_network_wait_fd_timeout(h, eid, fd, 1, timeout, &h->interrupt_callback);
            if (ret < 0)
                return ret;
            ret = srt_getlasterror(NULL);
            srt_clearlasterror();
            if (ret != 0) {
                char buf[128];
                ret = AVERROR(ret);
                av_strerror(ret, buf, sizeof(buf));
                if (will_try_next)
                    av_log(h, AV_LOG_WARNING,
                           "Connection to %s failed (%s), trying next address\n",
                           h->filename, buf);
                else
                    av_log(h, AV_LOG_ERROR, "Connection to %s failed: %s\n",
                           h->filename, buf);
            }
        default:
            return ret;
        }
    }
    return ret;
}

static int libsrt_setsockopt(URLContext *h, int fd, SRT_SOCKOPT optname, const char * optnamestr, const void * optval, int optlen)
{
    if (srt_setsockopt(fd, 0, optname, optval, optlen) < 0) {
        av_log(h, AV_LOG_ERROR, "failed to set option %s on socket: %s\n", optnamestr, srt_getlasterror_str());
        return AVERROR(EIO);
    }
    return 0;
}

/* - The "POST" options can be altered any time on a connected socket.
     They MAY have also some meaning when set prior to connecting; such
     option is SRTO_RCVSYN, which makes connect/accept call asynchronous.
     Because of that this option is treated special way in this app. */
static int libsrt_set_options_post(URLContext *h, int fd)
{
    SRTContext *s = h->priv_data;

    if ((s->inputbw >= 0 && libsrt_setsockopt(h, fd, SRTO_INPUTBW, "SRTO_INPUTBW", &s->inputbw, sizeof(s->inputbw)) < 0) ||
        (s->oheadbw >= 0 && libsrt_setsockopt(h, fd, SRTO_OHEADBW, "SRTO_OHEADBW", &s->oheadbw, sizeof(s->oheadbw)) < 0)) {
        return AVERROR(EIO);
    }
    return 0;
}

/* - The "PRE" options must be set prior to connecting and can't be altered
     on a connected socket, however if set on a listening socket, they are
     derived by accept-ed socket. */
static int libsrt_set_options_pre(URLContext *h, int fd)
{
    SRTContext *s = h->priv_data;
    int yes = 1;
    int tsbpddelay = s->tsbpddelay / 1000;
    int connect_timeout = s->connect_timeout;

    if ((s->mode == SRT_MODE_RENDEZVOUS && libsrt_setsockopt(h, fd, SRTO_RENDEZVOUS, "SRTO_RENDEZVOUS", &yes, sizeof(yes)) < 0) ||
        (s->maxbw >= 0 && libsrt_setsockopt(h, fd, SRTO_MAXBW, "SRTO_MAXBW", &s->maxbw, sizeof(s->maxbw)) < 0) ||
        (s->pbkeylen >= 0 && libsrt_setsockopt(h, fd, SRTO_PBKEYLEN, "SRTO_PBKEYLEN", &s->pbkeylen, sizeof(s->pbkeylen)) < 0) ||
        (s->passphrase && libsrt_setsockopt(h, fd, SRTO_PASSPHRASE, "SRTO_PASSPHRASE", &s->passphrase, sizeof(s->passphrase)) < 0) ||
        (s->mss >= 0 && libsrt_setsockopt(h, fd, SRTO_MSS, "SRTO_MMS", &s->mss, sizeof(s->mss)) < 0) ||
        (s->ffs >= 0 && libsrt_setsockopt(h, fd, SRTO_FC, "SRTO_FC", &s->ffs, sizeof(s->ffs)) < 0) ||
        (s->ipttl >= 0 && libsrt_setsockopt(h, fd, SRTO_IPTTL, "SRTO_UPTTL", &s->ipttl, sizeof(s->ipttl)) < 0) ||
        (s->iptos >= 0 && libsrt_setsockopt(h, fd, SRTO_IPTOS, "SRTO_IPTOS", &s->iptos, sizeof(s->iptos)) < 0) ||
        (tsbpddelay >= 0 && libsrt_setsockopt(h, fd, SRTO_TSBPDDELAY, "SRTO_TSBPDELAY", &tsbpddelay, sizeof(tsbpddelay)) < 0) ||
        (s->tlpktdrop >= 0 && libsrt_setsockopt(h, fd, SRTO_TLPKTDROP, "SRTO_TLPKDROP", &s->tlpktdrop, sizeof(s->tlpktdrop)) < 0) ||
        (s->nakreport >= 0 && libsrt_setsockopt(h, fd, SRTO_NAKREPORT, "SRTO_NAKREPORT", &s->nakreport, sizeof(s->nakreport)) < 0) ||
        (connect_timeout >= 0 && libsrt_setsockopt(h, fd, SRTO_CONNTIMEO, "SRTO_CONNTIMEO", &connect_timeout, sizeof(connect_timeout)) <0 )) {
        return AVERROR(EIO);
    }
    return 0;
}


static int libsrt_setup(URLContext *h, const char *uri, int flags)
{
    struct addrinfo hints = { 0 }, *ai, *cur_ai;
    int port, fd = -1;
    SRTContext *s = h->priv_data;
    const char *p;
    char buf[256];
    int ret;
    char hostname[1024],proto[1024],path[1024];
    char portstr[10];
    int open_timeout = 5000000;
    int eid;

    eid = srt_epoll_create();
    if (eid < 0)
        return libsrt_neterrno(h);
    s->eid = eid;

    av_url_split(proto, sizeof(proto), NULL, 0, hostname, sizeof(hostname),
        &port, path, sizeof(path), uri);
    if (strcmp(proto, "srt"))
        return AVERROR(EINVAL);
    if (port <= 0 || port >= 65536) {
        av_log(h, AV_LOG_ERROR, "Port missing in uri\n");
        return AVERROR(EINVAL);
    }
    p = strchr(uri, '?');
    if (p) {
        if (av_find_info_tag(buf, sizeof(buf), "timeout", p)) {
            s->rw_timeout = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "listen_timeout", p)) {
            s->listen_timeout = strtol(buf, NULL, 10);
        }
    }
    if (s->rw_timeout >= 0) {
        open_timeout = h->rw_timeout = s->rw_timeout;
    }
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (s->mode == SRT_MODE_LISTENER)
        hints.ai_flags |= AI_PASSIVE;
    ret = getaddrinfo(hostname[0] ? hostname : NULL, portstr, &hints, &ai);
    if (ret) {
        av_log(h, AV_LOG_ERROR,
               "Failed to resolve hostname %s: %s\n",
               hostname, gai_strerror(ret));
        return AVERROR(EIO);
    }

    cur_ai = ai;

 restart:

    fd = srt_socket(cur_ai->ai_family, cur_ai->ai_socktype, 0);
    if (fd < 0) {
        ret = libsrt_neterrno(h);
        goto fail;
    }

    if ((ret = libsrt_set_options_pre(h, fd)) < 0) {
        goto fail;
    }

    /* Set the socket's send or receive buffer sizes, if specified.
       If unspecified or setting fails, system default is used. */
    if (s->recv_buffer_size > 0) {
        srt_setsockopt(fd, SOL_SOCKET, SRTO_UDP_RCVBUF, &s->recv_buffer_size, sizeof (s->recv_buffer_size));
    }
    if (s->send_buffer_size > 0) {
        srt_setsockopt(fd, SOL_SOCKET, SRTO_UDP_SNDBUF, &s->send_buffer_size, sizeof (s->send_buffer_size));
    }
    if (s->mode == SRT_MODE_LISTENER) {
        // multi-client
        if ((ret = libsrt_listen(s->eid, fd, cur_ai->ai_addr, cur_ai->ai_addrlen, h, open_timeout / 1000)) < 0)
            goto fail1;
        fd = ret;
    } else {
        if (s->mode == SRT_MODE_RENDEZVOUS) {
            ret = srt_bind(fd, cur_ai->ai_addr, cur_ai->ai_addrlen);
            if (ret)
                goto fail1;
        }

        if ((ret = libsrt_listen_connect(s->eid, fd, cur_ai->ai_addr, cur_ai->ai_addrlen,
                                          open_timeout / 1000, h, !!cur_ai->ai_next)) < 0) {
            if (ret == AVERROR_EXIT)
                goto fail1;
            else
                goto fail;
        }
    }
    if ((ret = libsrt_set_options_post(h, fd)) < 0) {
        goto fail;
    }

    h->is_streamed = 1;
    s->fd = fd;

    freeaddrinfo(ai);
    return 0;

 fail:
    if (cur_ai->ai_next) {
        /* Retry with the next sockaddr */
        cur_ai = cur_ai->ai_next;
        if (fd >= 0)
            srt_close(fd);
        ret = 0;
        goto restart;
    }
 fail1:
    if (fd >= 0)
        srt_close(fd);
    freeaddrinfo(ai);
    return ret;
}

static int libsrt_open(URLContext *h, const char *uri, int flags)
{
    SRTContext *s = h->priv_data;
    const char * p;
    char buf[256];

    if (srt_startup() < 0) {
        return AVERROR_UNKNOWN;
    }

    /* SRT options (srt/srt.h) */
    p = strchr(uri, '?');
    if (p) {
        if (av_find_info_tag(buf, sizeof(buf), "maxbw", p)) {
            s->maxbw = strtoll(buf, NULL, 0);
        }
        if (av_find_info_tag(buf, sizeof(buf), "pbkeylen", p)) {
            s->pbkeylen = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "passphrase", p)) {
            s->passphrase = av_strndup(buf, strlen(buf));
        }
        if (av_find_info_tag(buf, sizeof(buf), "mss", p)) {
            s->mss = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "ffs", p)) {
            s->ffs = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "ipttl", p)) {
            s->ipttl = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "iptos", p)) {
            s->iptos = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "inputbw", p)) {
            s->inputbw = strtoll(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "oheadbw", p)) {
            s->oheadbw = strtoll(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "tsbpddelay", p)) {
            s->tsbpddelay = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "tlpktdrop", p)) {
            s->tlpktdrop = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "nakreport", p)) {
            s->nakreport = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "connect_timeout", p)) {
            s->connect_timeout = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "mode", p)) {
            if (!strcmp(buf, "caller")) {
                s->mode = SRT_MODE_CALLER;
            } else if (!strcmp(buf, "listener")) {
                s->mode = SRT_MODE_LISTENER;
            } else if (!strcmp(buf, "rendezvous")) {
                s->mode = SRT_MODE_RENDEZVOUS;
            } else {
                return AVERROR(EIO);
            }
        }
    }
    return libsrt_setup(h, uri, flags);
}

static int libsrt_read(URLContext *h, uint8_t *buf, int size)
{
    SRTContext *s = h->priv_data;
    int ret;

    if (!(h->flags & AVIO_FLAG_NONBLOCK)) {
        ret = libsrt_network_wait_fd_timeout(h, s->eid, s->fd, 0, h->rw_timeout, &h->interrupt_callback);
        if (ret)
            return ret;
    }

    ret = srt_recvmsg(s->fd, buf, size);
    if (ret < 0) {
        ret = libsrt_neterrno(h);
    }

    return ret;
}

static int libsrt_write(URLContext *h, const uint8_t *buf, int size)
{
    SRTContext *s = h->priv_data;
    int ret;

    if (!(h->flags & AVIO_FLAG_NONBLOCK)) {
        ret = libsrt_network_wait_fd_timeout(h, s->eid, s->fd, 1, h->rw_timeout, &h->interrupt_callback);
        if (ret)
            return ret;
    }

    ret = srt_sendmsg(s->fd, buf, size, -1, 0);
    if (ret < 0) {
        ret = libsrt_neterrno(h);
    }

    return ret;
}

static int libsrt_close(URLContext *h)
{
    SRTContext *s = h->priv_data;

    srt_close(s->fd);

    srt_epoll_release(s->eid);

    srt_cleanup();

    return 0;
}

static int libsrt_get_file_handle(URLContext *h)
{
    SRTContext *s = h->priv_data;
    return s->fd;
}

static const AVClass libsrt_class = {
    .class_name = "libsrt",
    .item_name  = av_default_item_name,
    .option     = libsrt_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const URLProtocol ff_libsrt_protocol = {
    .name                = "srt",
    .url_open            = libsrt_open,
    .url_read            = libsrt_read,
    .url_write           = libsrt_write,
    .url_close           = libsrt_close,
    .url_get_file_handle = libsrt_get_file_handle,
    .priv_data_size      = sizeof(SRTContext),
    .flags               = URL_PROTOCOL_FLAG_NETWORK,
    .priv_data_class     = &libsrt_class,
};
