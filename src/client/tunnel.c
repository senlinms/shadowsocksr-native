/* Copyright StrongLoop, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "defs.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include "common.h"
#include "dump_info.h"
#include "ssr_executive.h"
#include "encrypt.h"
#include "ssrbuffer.h"

/* A connection is modeled as an abstraction on top of two simple state
 * machines, one for reading and one for writing.  Either state machine
 * is, when active, in one of three states: busy, done or stop; the fourth
 * and final state, dead, is an end state and only relevant when shutting
 * down the connection.  A short overview:
 *
 *                          busy                  done           stop
 *  ----------|---------------------------|--------------------|------|
 *  readable  | waiting for incoming data | have incoming data | idle |
 *  writable  | busy writing out data     | completed write    | idle |
 *
 * We could remove the done state from the writable state machine. For our
 * purposes, it's functionally equivalent to the stop state.
 *
 * When the connection with upstream has been established, the struct tunnel_ctx
 * moves into a state where incoming data from the client is sent upstream
 * and vice versa, incoming data from upstream is sent to the client.  In
 * other words, we're just piping data back and forth.  See do_proxy()
 * for details.
 *
 * An interesting deviation from libuv's I/O model is that reads are discrete
 * rather than continuous events.  In layman's terms, when a read operation
 * completes, the connection stops reading until further notice.
 *
 * The rationale for this approach is that we have to wait until the data
 * has been sent out again before we can reuse the read buffer.
 *
 * It also pleasingly unifies with the request model that libuv uses for
 * writes and everything else; libuv may switch to a request model for
 * reads in the future.
 */

void tunnel_connected_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket);
void tunnel_read_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket);
void tunnel_write_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket);

static bool tunnel_is_dead(struct tunnel_ctx *tunnel);
static void tunnel_add_ref(struct tunnel_ctx *tunnel);
static void tunnel_release(struct tunnel_ctx *tunnel);
static void do_next(struct tunnel_ctx *tunnel, struct socket_ctx *socket);
static void do_handshake(struct tunnel_ctx *tunnel);
static void do_handshake_auth(struct tunnel_ctx *tunnel);
static void do_req_start(struct tunnel_ctx *tunnel);
static void do_req_parse(struct tunnel_ctx *tunnel);
static void do_req_lookup(struct tunnel_ctx *tunnel);
static void do_req_connect_start(struct tunnel_ctx *tunnel);
static void do_req_connect(struct tunnel_ctx *tunnel);
static void do_ssr_auth_sent(struct tunnel_ctx *tunnel);
static void do_proxy_start(struct tunnel_ctx *tunnel);
static void do_proxy(struct tunnel_ctx *tunnel, struct socket_ctx *socket);
static void socket_timer_start(struct socket_ctx *c);
static void socket_timer_stop(struct socket_ctx *c);
static void socket_timer_expire_cb(uv_timer_t *handle);
static void socket_getaddrinfo(struct socket_ctx *c, const char *hostname);
static void socket_getaddrinfo_done_cb(uv_getaddrinfo_t *req, int status, struct addrinfo *ai);
static int socket_connect(struct socket_ctx *c);
static void socket_connect_done_cb(uv_connect_t *req, int status);
static void socket_read(struct socket_ctx *c);
static void socket_read_done_cb(uv_stream_t *handle, ssize_t nread, const uv_buf_t *buf);
static void socket_read_stop(struct socket_ctx *c);
static void socket_alloc_cb(uv_handle_t *handle, size_t size, uv_buf_t *buf);
static void socket_write(struct socket_ctx *c, const void *data, size_t len);
static void socket_write_done_cb(uv_write_t *req, int status);
static void socket_close(struct socket_ctx *c);
static void socket_close_done_cb(uv_handle_t *handle);
static struct buffer_t * initial_package_create(const s5_ctx *parser);
uint8_t * build_udp_assoc_package(bool allow, const char *addr_str, int port, uint8_t *buf, size_t *buf_len);

static bool tunnel_is_dead(struct tunnel_ctx *tunnel) {
    return (tunnel->state == session_dead);
}

static void tunnel_add_ref(struct tunnel_ctx *tunnel) {
    tunnel->ref_count++;
}

static void tunnel_release(struct tunnel_ctx *tunnel) {
    tunnel->ref_count--;
    if (tunnel->ref_count == 0) {
        objects_container_remove(tunnel->env->tunnel_set, tunnel);
        if (tunnel->cipher) {
            tunnel_cipher_release(tunnel->cipher);
        }
        buffer_free(tunnel->init_pkg);
        free(tunnel);
    }
}

/* |incoming| has been initialized by listener.c when this is called. */
void tunnel_initialize(uv_tcp_t *listener) {
    struct socket_ctx *incoming;
    struct socket_ctx *outgoing;
    struct tunnel_ctx *tunnel;
    uv_loop_t *loop = listener->loop;
    struct server_env_t *env = (struct server_env_t *)loop->data;
    struct server_config *config = env->config;

    tunnel = (struct tunnel_ctx *) calloc(1, sizeof(*tunnel));

    tunnel->env = env;
    tunnel->cipher = NULL;
    tunnel->listener = listener;
    tunnel->state = session_handshake;
    tunnel->ref_count = 0;
    s5_init(&tunnel->parser);

    incoming = &tunnel->incoming;
    incoming->tunnel = tunnel;
    incoming->result = 0;
    incoming->rdstate = socket_stop;
    incoming->wrstate = socket_stop;
    incoming->idle_timeout = config->idle_timeout;
    VERIFY(0 == uv_timer_init(loop, &incoming->timer_handle));
    VERIFY(0 == uv_tcp_init(loop, &incoming->handle.tcp));
    VERIFY(0 == uv_accept((uv_stream_t *)listener, &incoming->handle.stream));

    outgoing = &tunnel->outgoing;
    outgoing->tunnel = tunnel;
    outgoing->result = 0;
    outgoing->rdstate = socket_stop;
    outgoing->wrstate = socket_stop;
    outgoing->idle_timeout = config->idle_timeout;
    VERIFY(0 == uv_timer_init(loop, &outgoing->timer_handle));
    VERIFY(0 == uv_tcp_init(loop, &outgoing->handle.tcp));

    /* Wait for the initial packet. */
    socket_read(incoming);

    objects_container_add(tunnel->env->tunnel_set, tunnel);

    tunnel->tunnel_connected_done = &tunnel_connected_done;
    tunnel->tunnel_read_done = &tunnel_read_done;
    tunnel->tunnel_write_done = &tunnel_write_done;
}

void tunnel_connected_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket) {
    do_next(tunnel, socket);
}

void tunnel_read_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket) {
    do_next(tunnel, socket);
}

void tunnel_write_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket) {
    if (tunnel->state != session_proxy) {
        do_next(tunnel, socket);
    }
}

/* This is the core state machine that drives the client <-> upstream proxy.
 * We move through the initial handshake and authentication steps first and
 * end up (if all goes well) in the proxy state where we're just proxying
 * data between the client and upstream.
 */
static void do_next(struct tunnel_ctx *tunnel, struct socket_ctx *socket) {
    switch (tunnel->state) {
    case session_handshake:
        do_handshake(tunnel);
        break;
    case session_handshake_auth:
        do_handshake_auth(tunnel);
        break;
    case session_req_start:
        do_req_start(tunnel);
        break;
    case session_req_parse:
        do_req_parse(tunnel);
        break;
    case session_req_udp_accoc:
        tunnel_shutdown(tunnel);
        break;
    case session_req_lookup:
        do_req_lookup(tunnel);
        break;
    case session_req_connect:
        do_req_connect(tunnel);
        break;
    case session_ssr_auth_sent:
        do_ssr_auth_sent(tunnel);
        break;
    case session_proxy_start:
        do_proxy_start(tunnel);
        break;
    case session_proxy:
        do_proxy(tunnel, socket);
        break;
    case session_kill:
        tunnel_shutdown(tunnel);
        break;
    default:
        UNREACHABLE();
    }
}

static void do_handshake(struct tunnel_ctx *tunnel) {
    enum s5_auth_method methods;
    struct socket_ctx *incoming;
    s5_ctx *parser;
    uint8_t *data;
    size_t size;
    enum s5_err err;

    parser = &tunnel->parser;
    incoming = &tunnel->incoming;
    ASSERT(incoming->rdstate == socket_done);
    ASSERT(incoming->wrstate == socket_stop);
    incoming->rdstate = socket_stop;

    if (incoming->result < 0) {
        pr_err("read error: %s", uv_strerror((int)incoming->result));
        tunnel_shutdown(tunnel);
        return;
    }

    data = (uint8_t *)incoming->t.buf;
    size = (size_t)incoming->result;
    err = s5_parse(parser, &data, &size);
    if (err == s5_ok) {
        socket_read(incoming);
        tunnel->state = session_handshake;  /* Need more data. */
        return;
    }

    if (size != 0) {
        /* Could allow a round-trip saving shortcut here if the requested auth
        * method is s5_auth_none (provided unauthenticated traffic is allowed.)
        * Requires client support however.
        */
        pr_err("junk in handshake");
        tunnel_shutdown(tunnel);
        return;
    }

    if (err != s5_auth_select) {
        pr_err("handshake error: %s", s5_strerror(err));
        tunnel_shutdown(tunnel);
        return;
    }

    methods = s5_auth_methods(parser);
    if ((methods & s5_auth_none) && can_auth_none(tunnel->listener, tunnel)) {
        s5_select_auth(parser, s5_auth_none);
        socket_write(incoming, "\5\0", 2);  /* No auth required. */
        tunnel->state = session_req_start;
        return;
    }

    if ((methods & s5_auth_passwd) && can_auth_passwd(tunnel->listener, tunnel)) {
        /* TODO(bnoordhuis) Implement username/password auth. */
        tunnel_shutdown(tunnel);
        return;
    }

    socket_write(incoming, "\5\377", 2);  /* No acceptable auth. */
    tunnel->state = session_kill;
}

/* TODO(bnoordhuis) Implement username/password auth. */
static void do_handshake_auth(struct tunnel_ctx *tunnel) {
    UNREACHABLE();
    tunnel_shutdown(tunnel);
}

static void do_req_start(struct tunnel_ctx *tunnel) {
    struct socket_ctx *incoming;

    incoming = &tunnel->incoming;
    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_done);
    incoming->wrstate = socket_stop;

    if (incoming->result < 0) {
        pr_err("write error: %s", uv_strerror((int)incoming->result));
        tunnel_shutdown(tunnel);
        return;
    }

    socket_read(incoming);
    tunnel->state = session_req_parse;
}

static void do_req_parse(struct tunnel_ctx *tunnel) {
    struct socket_ctx *incoming;
    struct socket_ctx *outgoing;
    s5_ctx *parser;
    uint8_t *data;
    size_t size;
    enum s5_err err;
    struct server_env_t *env;
    struct server_config *config;

    env = tunnel->env;
    config = env->config;

    parser = &tunnel->parser;
    incoming = &tunnel->incoming;
    outgoing = &tunnel->outgoing;

    ASSERT(incoming->rdstate == socket_done);
    ASSERT(incoming->wrstate == socket_stop);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_stop);
    incoming->rdstate = socket_stop;

    if (incoming->result < 0) {
        pr_err("read error: %s", uv_strerror((int)incoming->result));
        tunnel_shutdown(tunnel);
        return;
    }

    data = (uint8_t *)incoming->t.buf;
    size = (size_t)incoming->result;
    err = s5_parse(parser, &data, &size);
    if (err == s5_ok) {
        socket_read(incoming);
        tunnel->state = session_req_parse;  /* Need more data. */
        return;
    }

    if (size != 0) {
        pr_err("junk in request %u", (unsigned)size);
        tunnel_shutdown(tunnel);
        return;
    }

    if (err != s5_exec_cmd) {
        pr_err("request error: %s", s5_strerror(err));
        tunnel_shutdown(tunnel);
        return;
    }

    if (parser->cmd == s5_cmd_tcp_bind) {
        /* Not supported but relatively straightforward to implement. */
        pr_warn("BIND requests are not supported.");
        tunnel_shutdown(tunnel);
        return;
    }

    if (parser->cmd == s5_cmd_udp_assoc) {
        // UDP ASSOCIATE requests
        size_t len = sizeof(incoming->t.buf);
        uint8_t *buf = build_udp_assoc_package(config->udp, config->listen_host, config->listen_port,
                                               (uint8_t *)incoming->t.buf, &len);
        socket_write(incoming, buf, len);
        tunnel->state = session_req_udp_accoc;
        return;
    }

    ASSERT(parser->cmd == s5_cmd_tcp_connect);

    tunnel->init_pkg = initial_package_create(parser);
    tunnel->cipher = tunnel_cipher_create(tunnel->env, tunnel->init_pkg);

    union sockaddr_universal remote_addr = { 0 };
    if (convert_address(config->remote_host, config->remote_port, &remote_addr) != 0) {
        socket_getaddrinfo(outgoing, config->remote_host);
        tunnel->state = session_req_lookup;
        return;
    }

    memcpy(&outgoing->t.addr, &remote_addr, sizeof(remote_addr));

    do_req_connect_start(tunnel);
}

static void do_req_lookup(struct tunnel_ctx *tunnel) {
    s5_ctx *parser;
    struct socket_ctx *incoming;
    struct socket_ctx *outgoing;

    parser = &tunnel->parser;
    incoming = &tunnel->incoming;
    outgoing = &tunnel->outgoing;
    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_stop);

    if (outgoing->result < 0) {
        /* TODO(bnoordhuis) Escape control characters in parser->daddr. */
        pr_err("lookup error for \"%s\": %s",
            parser->daddr,
            uv_strerror((int)outgoing->result));
        /* Send back a 'Host unreachable' reply. */
        socket_write(incoming, "\5\4\0\1\0\0\0\0\0\0", 10);
        tunnel->state = session_kill;
        return;
    }

    /* Don't make assumptions about the offset of sin_port/sin6_port. */
    switch (outgoing->t.addr.addr.sa_family) {
    case AF_INET:
        outgoing->t.addr.addr4.sin_port = htons(parser->dport);
        break;
    case AF_INET6:
        outgoing->t.addr.addr6.sin6_port = htons(parser->dport);
        break;
    default:
        UNREACHABLE();
    }

    do_req_connect_start(tunnel);
}

/* Assumes that cx->outgoing.t.sa contains a valid AF_INET/AF_INET6 address. */
static void do_req_connect_start(struct tunnel_ctx *tunnel) {
    struct socket_ctx *incoming;
    struct socket_ctx *outgoing;
    int err;

    incoming = &tunnel->incoming;
    outgoing = &tunnel->outgoing;
    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_stop);

    if (!can_access(tunnel->listener, tunnel, &outgoing->t.addr.addr)) {
        pr_warn("connection not allowed by ruleset");
        /* Send a 'Connection not allowed by ruleset' reply. */
        socket_write(incoming, "\5\2\0\1\0\0\0\0\0\0", 10);
        tunnel->state = session_kill;
        return;
    }

    err = socket_connect(outgoing);
    if (err != 0) {
        pr_err("connect error: %s", uv_strerror(err));
        tunnel_shutdown(tunnel);
        return;
    }

    tunnel->state = session_req_connect;
}

static void do_req_connect(struct tunnel_ctx *tunnel) {
    struct socket_ctx *incoming;
    struct socket_ctx *outgoing;

    incoming = &tunnel->incoming;
    outgoing = &tunnel->outgoing;

    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_stop);

    if (outgoing->result == 0) {
        struct buffer_t *tmp = buffer_clone(tunnel->init_pkg);
        if (ssr_ok != tunnel_encrypt(tunnel->cipher, tmp)) {
            buffer_free(tmp);
            tunnel_shutdown(tunnel);
            return;
        }
        socket_write(outgoing, tmp->buffer, tmp->len);
        buffer_free(tmp);

        tunnel->state = session_ssr_auth_sent;
        return;
    } else {
        s5_ctx *parser = &tunnel->parser;
        char *addr = NULL;
        char ip_str[INET6_ADDRSTRLEN] = { 0 };

        if (parser->atyp == s5_atyp_host) {
            addr = (char *)parser->daddr;
        } else if (parser->atyp == s5_atyp_ipv4) {
            uv_inet_ntop(AF_INET, parser->daddr, ip_str, sizeof(ip_str));
            addr = ip_str;
        } else {
            uv_inet_ntop(AF_INET6, parser->daddr, ip_str, sizeof(ip_str));
            addr = ip_str;
        }
        const char *fmt = "upstream connection \"%s\" error: %s";
        pr_err(fmt, addr, uv_strerror((int)outgoing->result));
        /* Send a 'Connection refused' reply. */
        socket_write(incoming, "\5\5\0\1\0\0\0\0\0\0", 10);
        tunnel->state = session_kill;
        return;
    }

    UNREACHABLE();
    tunnel_shutdown(tunnel);
}

static void do_ssr_auth_sent(struct tunnel_ctx *tunnel) {
    struct socket_ctx *incoming;
    struct socket_ctx *outgoing;

    incoming = &tunnel->incoming;
    outgoing = &tunnel->outgoing;
    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_done);
    outgoing->wrstate = socket_stop;

    if (outgoing->result < 0) {
        pr_err("write error: %s", uv_strerror((int)outgoing->result));
        tunnel_shutdown(tunnel);
        return;
    }

    uint8_t *buf;
    struct buffer_t *init_pkg;
    buf = (uint8_t *)incoming->t.buf;
    init_pkg = tunnel->init_pkg;

    buf[0] = 5;  // Version.
    buf[1] = 0;  // Success.
    buf[2] = 0;  // Reserved.
    memcpy(buf + 3, init_pkg->buffer, init_pkg->len);
    socket_write(incoming, buf, 3 + init_pkg->len);
    tunnel->state = session_proxy_start;
}

static void do_proxy_start(struct tunnel_ctx *tunnel) {
    struct socket_ctx *incoming;
    struct socket_ctx *outgoing;

    incoming = &tunnel->incoming;
    outgoing = &tunnel->outgoing;
    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_done);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_stop);
    incoming->wrstate = socket_stop;

    if (incoming->result < 0) {
        pr_err("write error: %s", uv_strerror((int)incoming->result));
        tunnel_shutdown(tunnel);
        return;
    }

    socket_read(incoming);
    socket_read(outgoing);
    tunnel->state = session_proxy;
}

/* Proxy incoming data back and forth. */
static void do_proxy(struct tunnel_ctx *tunnel, struct socket_ctx *socket) {
    struct socket_ctx *incoming;
    struct socket_ctx *outgoing;

    incoming = &tunnel->incoming;
    outgoing = &tunnel->outgoing;
    ASSERT(socket==incoming || socket==outgoing);

    if (socket == outgoing) {
        struct tunnel_cipher_ctx *tc;
        struct buffer_t *buf = NULL;
        do {
            tc = tunnel->cipher;

            buf = buffer_alloc(SSR_BUFF_SIZE);
            buffer_store(buf, outgoing->t.buf, (size_t)outgoing->result);

            struct buffer_t *feedback = NULL;
            if (ssr_ok != tunnel_decrypt(tc, buf, &feedback)) {
                tunnel_shutdown(tunnel);
                break;
            }
            if (feedback) {
                // SSR logic
                ASSERT(buf->len == 0);
                socket_write(outgoing, feedback->buffer, feedback->len);
                buffer_free(feedback);

                socket_read_stop(incoming);
                socket_read(incoming);
            }
            if (buf->len > 0) {
                socket_write(incoming, buf->buffer, buf->len);
            }
        } while (0);
        buffer_free(buf);
    }

    if (socket == incoming) {
        struct tunnel_cipher_ctx *tc;
        struct buffer_t *buf = NULL;
        do {
            tc = tunnel->cipher;

            buf = buffer_alloc(SSR_BUFF_SIZE);
            buffer_store(buf, incoming->t.buf, (size_t)incoming->result);
            if (ssr_ok != tunnel_encrypt(tc, buf)) {
                tunnel_shutdown(tunnel);
                break;
            }
            if (buf->len > 0) {
                socket_write(outgoing, buf->buffer, buf->len);
            } else if (buf->len == 0) {
                // SSR logic
                socket_read_stop(incoming);
            }
        } while (0);
        buffer_free(buf);
    }
}

void tunnel_shutdown(struct tunnel_ctx *tunnel) {
    if (tunnel_is_dead(tunnel) != false) {
        return;
    }

    /* Try to cancel the request. The callback still runs but if the
    * cancellation succeeded, it gets called with status=UV_ECANCELED.
    */
    if (tunnel->state == session_req_lookup) {
        uv_cancel(&tunnel->outgoing.t.req);
    }

    socket_close(&tunnel->incoming);
    socket_close(&tunnel->outgoing);

    tunnel->state = session_dead;
}

static void socket_timer_start(struct socket_ctx *c) {
    VERIFY(0 == uv_timer_start(&c->timer_handle,
        socket_timer_expire_cb,
        c->idle_timeout,
        0));
}

static void socket_timer_stop(struct socket_ctx *c) {
    VERIFY(0 == uv_timer_stop(&c->timer_handle));
}

static void socket_timer_expire_cb(uv_timer_t *handle) {
    struct socket_ctx *c;
    struct tunnel_ctx *tunnel;

    c = CONTAINER_OF(handle, struct socket_ctx, timer_handle);
    c->result = UV_ETIMEDOUT;

    tunnel = c->tunnel;

    if (tunnel_is_dead(tunnel)) {
        return;
    }

    tunnel_shutdown(tunnel);
}

static void socket_getaddrinfo(struct socket_ctx *c, const char *hostname) {
    struct addrinfo hints;
    struct tunnel_ctx *tunnel;

    tunnel = c->tunnel;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    VERIFY(0 == uv_getaddrinfo(tunnel->listener->loop,
        &c->t.addrinfo_req,
        socket_getaddrinfo_done_cb,
        hostname,
        NULL,
        &hints));
    socket_timer_start(c);
}

static void socket_getaddrinfo_done_cb(uv_getaddrinfo_t *req, int status, struct addrinfo *ai) {
    struct socket_ctx *c;
    struct tunnel_ctx *tunnel;

    c = CONTAINER_OF(req, struct socket_ctx, t.addrinfo_req);
    c->result = status;

    tunnel = c->tunnel;

    if (tunnel_is_dead(tunnel)) {
        return;
    }

    socket_timer_stop(c);

    if (status == 0) {
        /* FIXME(bnoordhuis) Should try all addresses. */
        if (ai->ai_family == AF_INET) {
            c->t.addr.addr4 = *(const struct sockaddr_in *) ai->ai_addr;
        } else if (ai->ai_family == AF_INET6) {
            c->t.addr.addr6 = *(const struct sockaddr_in6 *) ai->ai_addr;
        } else {
            UNREACHABLE();
        }
    }

    uv_freeaddrinfo(ai);
    do_next(tunnel, c);
}

/* Assumes that c->t.sa contains a valid AF_INET or AF_INET6 address. */
static int socket_connect(struct socket_ctx *c) {
    ASSERT(c->t.addr.addr.sa_family == AF_INET || c->t.addr.addr.sa_family == AF_INET6);
    socket_timer_start(c);
    return uv_tcp_connect(&c->t.connect_req,
        &c->handle.tcp,
        &c->t.addr.addr,
        socket_connect_done_cb);
}

static void socket_connect_done_cb(uv_connect_t *req, int status) {
    struct socket_ctx *c;
    struct tunnel_ctx *tunnel;

    c = CONTAINER_OF(req, struct socket_ctx, t.connect_req);
    c->result = status;

    tunnel = c->tunnel;

    if (tunnel_is_dead(tunnel)) {
        return;
    }

    socket_timer_stop(c);

    if (status == UV_ECANCELED || status == UV_ECONNREFUSED) {
        tunnel_shutdown(tunnel);
        return;  /* Handle has been closed. */
    }

    ASSERT(tunnel->tunnel_connected_done);
    tunnel->tunnel_connected_done(tunnel, c);
}

static void socket_read(struct socket_ctx *c) {
    ASSERT(c->rdstate == socket_stop);
    VERIFY(0 == uv_read_start(&c->handle.stream, socket_alloc_cb, socket_read_done_cb));
    c->rdstate = socket_busy;
    socket_timer_start(c);
}

static void socket_read_done_cb(uv_stream_t *handle, ssize_t nread, const uv_buf_t *buf) {
    struct socket_ctx *c;
    struct tunnel_ctx *tunnel;

    c = CONTAINER_OF(handle, struct socket_ctx, handle);
    tunnel = c->tunnel;

    if (tunnel_is_dead(tunnel)) {
        return;
    }

    if (tunnel->state != session_proxy) {
        uv_read_stop(&c->handle.stream);
    }

    socket_timer_stop(c);

    if (nread == 0) {
        return;
    }
    if (nread < 0) {
        // http://docs.libuv.org/en/v1.x/stream.html
        ASSERT(nread == UV_EOF || nread == UV_ECONNRESET);
        tunnel_shutdown(tunnel);
        return;
    }

    ASSERT(c->t.buf == (uint8_t *) buf->base);
    if (tunnel->state != session_proxy) {
        ASSERT(c->rdstate == socket_busy);
    }
    c->rdstate = socket_done;
    c->result = nread;

    ASSERT(tunnel->tunnel_read_done);
    tunnel->tunnel_read_done(tunnel, c);
}

static void socket_read_stop(struct socket_ctx *c) {
    uv_read_stop(&c->handle.stream);
    c->rdstate = socket_stop;
}

static void socket_alloc_cb(uv_handle_t *handle, size_t size, uv_buf_t *buf) {
    struct socket_ctx *c;

    c = CONTAINER_OF(handle, struct socket_ctx, handle);

    if (c->tunnel->state != session_proxy) {
        ASSERT(c->rdstate == socket_busy);
    }
    buf->base = (char *) c->t.buf;
    buf->len = sizeof(c->t.buf);
}

static void socket_write(struct socket_ctx *c, const void *data, size_t len) {
    uv_buf_t buf;
    struct tunnel_ctx *tunnel = c->tunnel;

    if (tunnel->state != session_proxy) {
        ASSERT(c->wrstate == socket_stop || c->wrstate == socket_done);
    }
    c->wrstate = socket_busy;

    // It's okay to cast away constness here, uv_write() won't modify the memory.
    buf = uv_buf_init((char *)data, (unsigned int)len);

    uv_write_t *req = (uv_write_t *)calloc(1, sizeof(uv_write_t));
    req->data = c;

    VERIFY(0 == uv_write(req, &c->handle.stream, &buf, 1, socket_write_done_cb));
    socket_timer_start(c);
}

static void socket_write_done_cb(uv_write_t *req, int status) {
    struct socket_ctx *c;
    struct tunnel_ctx *tunnel;

    c = (struct socket_ctx *)req->data;
    free(req);
    tunnel = c->tunnel;

    if (tunnel_is_dead(tunnel)) {
        return;
    }

    socket_timer_stop(c);

    if (status == UV_ECANCELED) {
        tunnel_shutdown(tunnel);
        return;  /* Handle has been closed. */
    }

    if (tunnel->state != session_proxy) {
        ASSERT(c->wrstate == socket_busy);
    }
    c->wrstate = socket_done;
    c->result = status;

    ASSERT(tunnel->tunnel_write_done);
    tunnel->tunnel_write_done(tunnel, c);
}

static void socket_close(struct socket_ctx *c) {
    struct tunnel_ctx *tunnel = c->tunnel;
    ASSERT(c->rdstate != socket_dead);
    ASSERT(c->wrstate != socket_dead);
    c->rdstate = socket_dead;
    c->wrstate = socket_dead;
    c->timer_handle.data = c;
    c->handle.handle.data = c;

    tunnel_add_ref(tunnel);
    uv_close(&c->handle.handle, socket_close_done_cb);
    tunnel_add_ref(tunnel);
    uv_close((uv_handle_t *)&c->timer_handle, socket_close_done_cb);
}

static void socket_close_done_cb(uv_handle_t *handle) {
    struct socket_ctx *c;
    struct tunnel_ctx *tunnel;

    c = (struct socket_ctx *) handle->data;
    tunnel = c->tunnel;

    tunnel_release(tunnel);
}

static struct buffer_t * initial_package_create(const s5_ctx *parser) {
    struct buffer_t *buffer = buffer_alloc(SSR_BUFF_SIZE);

    char *iter = buffer->buffer;
    char len;
    iter[0] = (char) parser->atyp;
    iter++;

    switch (parser->atyp) {
    case s5_atyp_ipv4:  // IPv4
        memcpy(iter, parser->daddr, sizeof(struct in_addr));
        iter += sizeof(struct in_addr);
        break;
    case s5_atyp_ipv6:  // IPv6
        memcpy(iter, parser->daddr, sizeof(struct in6_addr));
        iter += sizeof(struct in6_addr);
        break;
    case s5_atyp_host:
        len = (char)strlen((char *)parser->daddr);
        iter[0] = len;
        iter++;
        memcpy(iter, parser->daddr, len);
        iter += len;
        break;
    default:
        ASSERT(0);
        break;
    }
    *((unsigned short *)iter) = htons(parser->dport);
    iter += sizeof(unsigned short);

    buffer->len = iter - buffer->buffer;

    return buffer;
}

uint8_t * build_udp_assoc_package(bool allow, const char *addr_str, int port, uint8_t *buf, size_t *buf_len) {
    if (addr_str == NULL || buf==NULL || buf_len==NULL) {
        return NULL;
    }

    bool ipV6 = false;

    union sockaddr_universal addr;
    if (uv_ip4_addr(addr_str, port, &addr.addr4) != 0) {
        if (uv_ip6_addr(addr_str, port, &addr.addr6) != 0) {
            return NULL;
        }
        ipV6 = true;
    }

    if (ipV6) {
        if (*buf_len < 22) {
            return NULL;
        }
    } else {
        if (*buf_len < 10) {
            return NULL;
        }
    }

    buf[0] = 5;  // Version.
    if (allow) {
        buf[1] = 0;  // Success.
    } else {
        buf[1] = 0x07;  // Command not supported.
    }
    buf[2] = 0;  // Reserved.
    buf[3] = (uint8_t) (ipV6 ? 0x04 : 0x01);  // atyp

    size_t in6_addr_w = sizeof(addr.addr6.sin6_addr);
    size_t in4_addr_w = sizeof(addr.addr4.sin_addr);
    size_t port_w = sizeof(addr.addr4.sin_port);

    if (ipV6) {
        *buf_len = 4 + in6_addr_w + port_w;
        memcpy(buf + 4, &addr.addr6.sin6_addr, in6_addr_w);
        memcpy(buf + 4 + in6_addr_w, &addr.addr6.sin6_port, port_w);
    } else {
        *buf_len = 4 + in4_addr_w + port_w;
        memcpy(buf + 4, &addr.addr4.sin_addr, in4_addr_w);
        memcpy(buf + 4 + in4_addr_w, &addr.addr4.sin_port, port_w);
    }
    return buf;
}
