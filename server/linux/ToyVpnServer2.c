/*
 * Copyright (C) 2011 The Android Open Source Project
 * Copyright (C) 2022 ssrlive
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(_MSC_VER)
#include <WinSock2.h>
#include <WS2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>

#include <uv.h>
#include <c_stl_lib.h>
#include "sockaddr_universal.h"
#include "ref_count_def.h"

#if !defined(CONTAINER_OF)
#define CONTAINER_OF(ptr, type, field) \
  ((type *) ((char *) (ptr) - ((char *) &((type *) 0)->field)))
#endif /* CONTAINER_OF */

#define IDLE_MAX_MS (20 * 1000)

#ifdef __linux__

/*
// There are several ways to play with this program. Here we just give an
// example for the simplest scenario. Let us say that a Linux box has a
// public IPv4 address on eth0. Please try the following steps and adjust
// the parameters when necessary.
//
// # Enable IP forwarding
// echo 1 > /proc/sys/net/ipv4/ip_forward
//
// # Pick a range of private addresses and perform NAT over eth0.
// iptables -t nat -A POSTROUTING -s 10.10.0.0/8 -o eth0 -j MASQUERADE
//
// # Create a TUN interface.
// ip tuntap add dev tun0 mode tun
//
// # Set the addresses and bring up the interface.
// ifconfig tun0 10.10.0.1 dstaddr 10.10.0.2 up
//
// # Create a server on port 8000 with shared secret "test".
// ./ToyVpnServer tun0 8000 test -m 1400 -a 10.10.0.2 32 -d 8.8.8.8 -r 0.0.0.0 0 &
//
// This program only handles a session at a time. To allow multiple sessions,
// multiple servers can be created on the same port, but each of them requires
// its own TUN interface. A short shell script will be sufficient. Since this
// program is designed for demonstration purpose, it performs neither strong
// authentication nor encryption. DO NOT USE IT IN PRODUCTION!
// */

#include <net/if.h>
#include <linux/if_tun.h>

static int get_interface(uv_loop_t* loop, const char *name, uv_fs_t *iface_ctx) {
    int _interface = -1;
    struct ifreq ifr;
    do {
        if ((loop == NULL) || (name == NULL) || (iface_ctx == NULL)) {
            break;
        }
        
        if (uv_fs_open(loop, iface_ctx, "/dev/net/tun", O_RDWR | O_NONBLOCK, 0, NULL) < 0) {
            break;
        }

        memset(&ifr, 0, sizeof(ifr));
        ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
        strncpy(ifr.ifr_name, name, sizeof(ifr.ifr_name));

        if (ioctl(iface_ctx->result, TUNSETIFF, &ifr) < 0) {
            break;
        }

        _interface = iface_ctx->result;
    } while (0);

    return _interface;
}

#else

#error Sorry, you have to implement this part by yourself.

#endif

static size_t build_parameters(char *parameters, size_t size, int argc, char **argv)
{
    /* Well, for simplicity, we just concatenate them (almost) blindly. */
    size_t offset = 0;
    int i;
    for (i = 4; i < argc; ++i) {
        char *parameter = argv[i];
        size_t length = strlen(parameter);
        char delimiter = ',';

        /* If it looks like an option, prepend a space instead of a comma. */
        if (length == 2 && parameter[0] == '-') {
            ++parameter;
            --length;
            delimiter = ' ';
        }

        /* This is just a demo app, really. */
        if (offset + length >= size) {
            puts("Parameters are too large");
            exit(1);
        }

        /* Append the delimiter and the parameter. */
        parameters[offset] = delimiter;
        memcpy(&parameters[offset + 1], parameter, length);
        offset += 1 + length;
    }

    /* Fill the rest of the space with spaces. */
    memset(&parameters[offset], ' ', size - offset);

    /* Control messages always start with zero. */
    parameters[0] = 0;

    return offset;
}

/* ----------------------------------------------------------------------------- */

#define PARAMETERS_MAX 1024
#define SECRET_MAX 256
#define READ_BUFF_MAX 32767

int create_toyvpn_udp_listener(uv_loop_t *loop, const char* address, uint16_t port, uv_udp_t *udp_listener);

struct listener_ctx {
    uv_udp_t udp_listener;
    uv_fs_t tun_iface;
    char parameters[PARAMETERS_MAX];
    size_t param_len;
    char secret[SECRET_MAX];
    struct cstl_set *connections;

    uv_signal_t sigkill;
    uv_signal_t sigterm;
    uv_signal_t sigint;
    bool shutting_down;
};

struct client_node {
    union sockaddr_universal incoming_addr;
    struct listener_ctx *listener; /* weak ptr. */
    uv_timer_t timer;
    uint64_t timeout;
    bool verified;

    bool shutting_down;
    REF_COUNT_MEMBER;
};

static REF_COUNT_ADD_REF_DECL(client_node); /* client_node_add_ref */
static REF_COUNT_RELEASE_DECL(client_node); /* client_node_release */

static void client_node_free_internal(struct client_node *ctx) {
    free(ctx);
}

static REF_COUNT_ADD_REF_IMPL(client_node)
static REF_COUNT_RELEASE_IMPL(client_node, client_node_free_internal)

int node_connection_comparation(const void *left, const void *right) {
    struct client_node *l = *(struct client_node **)left;
    struct client_node *r = *(struct client_node **)right;
    if ( l < r ) {
        return -1;
    } else if ( l > r ) {
        return 1;
    } else {
        return 0;
    }
}

static void listener_ctx_destroy(struct listener_ctx *ctx) {
    /* free memory */
    cstl_set_delete(ctx->connections);
    uv_fs_req_cleanup(&ctx->tun_iface);
    free(ctx);
}

static void udp_listener_close_done(uv_handle_t* handle) {
    struct listener_ctx *ctx = CONTAINER_OF(handle, struct listener_ctx, udp_listener);
    listener_ctx_destroy(ctx);
}

static void client_node_shutdown(struct client_node *client);

static void connection_release(struct cstl_set *set, const void *obj, cstl_bool *stop, void *p) {
    (void)set; (void)obj; (void)stop; (void)p;
    client_node_shutdown((struct client_node *)obj);
}

/* signal call this function. */
void listener_ctx_shutdown(struct listener_ctx *ctx) {
    if (ctx == NULL) {
        return;
    }
    if (ctx->shutting_down) {
        return;
    }
    ctx->shutting_down = true;

    cstl_set_container_traverse(ctx->connections, &connection_release, NULL);
    uv_close((uv_handle_t *)&ctx->udp_listener, udp_listener_close_done);

    uv_close((uv_handle_t *)&ctx->sigint, NULL);
    uv_close((uv_handle_t *)&ctx->sigkill, NULL);
    uv_close((uv_handle_t *)&ctx->sigterm, NULL);
}

static void on_signal(uv_signal_t* signal, int signum) {
    struct listener_ctx *ctx = signal->data;
    printf("\tFired signal %d, exiting\n", signum);
    uv_signal_stop(signal);
    listener_ctx_shutdown(ctx);
}

static void init_signal(uv_loop_t *loop, uv_signal_t* signal, int signum) {
    uv_signal_init(loop, signal);
    uv_signal_start(signal, on_signal, signum);
}

#define UDP_SERVER_ADDR "0.0.0.0"

int main(int argc, char **argv)
{
    int tunnel, _interface;
    uv_loop_t *loop = uv_default_loop();
    struct listener_ctx *ctx = NULL;
    int ret = 0;
    uint16_t port = 0;

    if (argc < 5) {
        printf("Usage: %s <tunN> <port> <secret> options...\n"
               "\n"
               "Options:\n"
               "  -m <MTU> for the maximum transmission unit\n"
               "  -a <address> <prefix-length> for the private address\n"
               "  -r <address> <prefix-length> for the forwarding route\n"
               "  -d <address> for the domain name server\n"
               "  -s <domain> for the search domain\n"
               "\n"
               "Note that TUN interface needs to be configured properly\n"
               "BEFORE running this program. For more information, please\n"
               "read the comments in the source code.\n\n", argv[0]);
        exit(1);
    }

    ctx = (struct listener_ctx *)calloc(1, sizeof(*ctx));

    strncpy(ctx->secret, argv[3], sizeof(ctx->secret));

    /* Parse the arguments and set the parameters. */
    ctx->param_len = build_parameters(ctx->parameters, sizeof(ctx->parameters), argc, argv);

    /* Get TUN interface. */
    _interface = get_interface(loop, argv[1], &ctx->tun_iface);
    if (_interface < 0) {
        perror("Cannot get TUN interface");
        exit(1);
    }

    port = atoi(argv[2]);

    if (create_toyvpn_udp_listener(loop, UDP_SERVER_ADDR, port, &ctx->udp_listener) < 0) {
        perror("Can't create UDP listener");
        exit(-1);
    }

    ctx->connections = cstl_set_new(node_connection_comparation, NULL);

    printf("Server listening on: UDP address: %s, port: %d\n", UDP_SERVER_ADDR, port);
    fflush(stdout);

    ctx->sigkill.data = ctx->sigterm.data = ctx->sigint.data = ctx;
    init_signal(loop, &ctx->sigkill, SIGKILL);
    init_signal(loop, &ctx->sigterm, SIGTERM);
    init_signal(loop, &ctx->sigint, SIGINT);

    ret = uv_run(loop, UV_RUN_DEFAULT);

    (void)tunnel;

    return ret;
}

void release_uv_buffer(uv_buf_t *buf) {
    if (buf) {
        free(buf->base);
        buf->base = NULL;
        buf->len = 0;
    }
}

static void on_send_to_incoming_udp_done(uv_udp_send_t* req, int status) {
    uint8_t *info = req->data;
    free(info);
    free(req);
    if (status) {
        fprintf(stderr, "%s error: %s\n", __FUNCTION__, uv_strerror(status));
    }
}

struct matching_connect {
    union sockaddr_universal incoming_addr;
    struct client_node *client;
};

static void find_matching_connection(struct cstl_set *set, const void *obj, cstl_bool *stop, void *p) {
    struct matching_connect *match = (struct matching_connect *)p;
    struct client_node *client = (struct client_node *)obj;
    if (memcmp(&match->incoming_addr, &client->incoming_addr, sizeof(match->incoming_addr)) == 0) {
        match->client = client;
        if (stop) {
            *stop = cstl_true;
        }
    }
    (void)set;
}

struct client_node *
create_client_node(uv_loop_t* loop, uint64_t timeout, uv_timer_cb cb) {
    uv_timer_t *timer;
    struct client_node *client;

    client = (struct client_node *) calloc(1, sizeof(*client));
    client->timeout = timeout;

    timer = &client->timer;
    uv_timer_init(loop, timer);
    timer->data = client;
    timer->timer_cb = cb;

    return client;
}

static void client_node_timer_close_done(uv_handle_t* handle) {
    struct client_node *ctx = (struct client_node *)handle->data;
    assert(ctx == CONTAINER_OF(handle, struct client_node, timer));
    client_node_release(ctx);
}

static void client_node_shutdown(struct client_node *client) {
    if (client == NULL) {
        return;
    }
    if (client->shutting_down) {
        return;
    }
    client->shutting_down = true;

    cstl_set_container_remove(client->listener->connections, client);
    {
        uv_timer_t *timer = &client->timer;
        uv_timer_stop(timer);
        uv_close((uv_handle_t *)timer, client_node_timer_close_done);
        client_node_add_ref(client);
    }
    {
        char *info = universal_address_to_string(&client->incoming_addr, &malloc, true);
        fprintf(stderr, "session %s shutting down\n", info);
        free(info);
    }

    client_node_release(client);
}

static void common_restart_timer(uv_timer_t *timer, uint64_t timeout) {
    assert(timer);
    assert(timer->timer_cb != NULL);
    assert(timeout > 0);
    uv_timer_stop(timer);
    uv_timer_start(timer, timer->timer_cb, timeout, 0);
}

static void client_timeout_cb(uv_timer_t* handle) {
    struct client_node *client = CONTAINER_OF(handle, struct client_node, timer);

    client_node_shutdown(client);
}

static void _send_to_incoming_node(struct client_node *client, const uint8_t *packet, size_t plen) {
    uint8_t *info = (uint8_t *)calloc(plen, sizeof(*info));
    uv_buf_t buff = uv_buf_init((char *)info, (unsigned int)plen);
    uv_udp_send_t *req = (uv_udp_send_t *)calloc(1, sizeof(*req));
    uv_udp_t *udp = &client->listener->udp_listener;
    struct sockaddr *addr = &client->incoming_addr.addr;

    memcpy(info, packet, plen);
    req->data = info;

    uv_udp_send(req, udp, &buff, 1, addr, on_send_to_incoming_udp_done);
}

struct fs_traffic_obj {
    uv_fs_t fs_req;
    uint8_t *data;
    struct client_node *client; /* weak reference */
};

static void client_iface_read_done(uv_fs_t *req) {
    struct fs_traffic_obj *obj = CONTAINER_OF(req, struct fs_traffic_obj, fs_req);
    struct listener_ctx *listener = obj->client->listener;
    uv_file file = listener->tun_iface.result;
    ssize_t nread = req->result;

    if (nread < 0) {
        fprintf(stderr, "Read error: %s\n", uv_strerror(nread));
    }
    else if (nread == 0) {
        uv_fs_t close_req;
        // synchronous
        uv_fs_close(req->loop, &close_req, file, NULL);
    }
    else if (nread > 0) {
        _send_to_incoming_node(obj->client, obj->data, (size_t)nread);
    }
    free(obj->data);
    free(obj);
}

static void client_iface_write_done(uv_fs_t *req) {
    struct fs_traffic_obj *fs_obj = CONTAINER_OF(req, struct fs_traffic_obj, fs_req);
    struct listener_ctx *listener = fs_obj->client->listener;
    uv_file file = listener->tun_iface.result;

    if (req->result < 0) {
        fprintf(stderr, "Write error: %s\n", uv_strerror((int)req->result));
    } else {
        struct fs_traffic_obj *obj = (struct fs_traffic_obj *)calloc(1, sizeof(*obj));
        uv_buf_t buf;
        
        obj->data = (uint8_t*)calloc(READ_BUFF_MAX, sizeof(uint8_t));
        buf = uv_buf_init((char*)obj->data, (unsigned int)READ_BUFF_MAX);

        uv_fs_read(req->loop, &obj->fs_req, file, &buf, 1, -1, client_iface_read_done);
    }
    free(fs_obj->data);
    free(fs_obj);
}

static void client_node_handle_incoming_packet(struct client_node *client, const uint8_t *packet, size_t plen) {
    struct listener_ctx *listener = client->listener;
    bool status_ok = true;

    if ((packet == NULL) || (plen == 0) || (client==NULL)) {
        return;
    }

    if (client->verified == false) {
        size_t slen = strlen(listener->secret);
        if ((packet[0] == 0) && (memcmp(listener->secret, &packet[1], slen) == 0)) {
            client->verified = true;
            /* Send back the parameters. begin traffic */
            _send_to_incoming_node(client, (uint8_t*)listener->parameters, listener->param_len);
        } else {
            /* error data, just drop it. */
            status_ok = false;
        }
    } else {
        if (packet[0] == 0) {
            /* heartbeat packet, just response it with the same packet */
            _send_to_incoming_node(client, packet, plen);
        } else {
            /* data stream, write to TUN interface. */
            uv_buf_t buf;
            uv_loop_t *loop = listener->udp_listener.loop;
            uv_file file = listener->tun_iface.result;
            struct fs_traffic_obj *obj = (struct fs_traffic_obj *)calloc(1, sizeof(*obj));

            obj->data = (uint8_t*)calloc(plen, sizeof(uint8_t));
            buf = uv_buf_init((char*)obj->data, (unsigned int)plen);
            memcpy(obj->data, packet, plen);

            uv_fs_write(loop, &obj->fs_req, file, &buf, 1, -1, client_iface_write_done);
        }
    }

    if (status_ok) {
        /* reset timeout timer. */
        common_restart_timer(&client->timer, client->timeout);
    } else {
        client_node_shutdown(client);
    }
}

static void on_incoming_read(uv_udp_t *udp, ssize_t nread, const uv_buf_t *buf,
             const struct sockaddr *addr, unsigned flags)
{
    struct listener_ctx *ctx;
    do {
        if (nread <= 0) {
            if (nread < 0) {
                fprintf(stderr, "Read error %s\n", uv_err_name(nread));
            }
            break;
        }

        if (nread > 0) {
            struct client_node *client;
            char *info;

            struct matching_connect match = { {{0}}, 0 };
            match.incoming_addr.addr = *addr;

            ctx = CONTAINER_OF(udp, struct listener_ctx, udp_listener);

            cstl_set_container_traverse(ctx->connections, &find_matching_connection, &match);
            client = match.client;

            info = universal_address_to_string(&match.incoming_addr, &malloc, true);
            fprintf(stderr, client ? "session %s reused\n" : "session %s starting\n", info);
            free(info);

            if (client == NULL) {
                client = create_client_node(udp->loop, IDLE_MAX_MS, client_timeout_cb);
                client->listener = ctx;
                client->incoming_addr = match.incoming_addr;

                client_node_add_ref(client);
                cstl_set_container_add(ctx->connections, client);
            }

            client_node_handle_incoming_packet(client, (uint8_t*)buf->base, (size_t)nread);
        }
    } while (0);

    release_uv_buffer((uv_buf_t *)buf);
    (void)flags;
}

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    buf->base = malloc(suggested_size);
    buf->len = suggested_size;
    (void)handle;
}

int create_toyvpn_udp_listener(uv_loop_t *loop, const char* listen_addr, uint16_t port, uv_udp_t *listener)
{
    int result = -1;
    do {
        struct sockaddr_in recv_addr;
        if ((loop == NULL) || (listen_addr == NULL) || (port == 0) || (listener == NULL)) {
            break;
        }
        if (uv_ip4_addr(listen_addr, port, &recv_addr) < 0) {
            break;
        }
        if (uv_udp_init(loop, listener) < 0) {
            break;
        }
        if (uv_udp_bind(listener, (const struct sockaddr *)&recv_addr, UV_UDP_REUSEADDR) < 0) {
            break;
        }
        if (uv_udp_recv_start(listener, alloc_buffer, on_incoming_read) < 0) {
            break;
        }
        result = 0;
    } while (0);
    return result;
}
