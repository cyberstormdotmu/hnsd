#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "addr.h"
#include "cache.h"
#include "constants.h"
#include "dns.h"
#include "ec.h"
#include "error.h"
#include "resource.h"
#include "ns.h"
#include "pool.h"
#include "req.h"
#include "uv.h"

/*
 * Types
 */

typedef struct {
  hsk_ns_t *ns;
  void *data;
  bool should_free;
} hsk_send_data_t;

/*
 * Prototypes
 */

static void
hsk_ns_log(hsk_ns_t *ns, const char *fmt, ...);

static void
after_resolve(
  const char *name,
  int status,
  bool exists,
  const uint8_t *data,
  size_t data_len,
  const void *arg
);

int
hsk_ns_send(
  hsk_ns_t *ns,
  uint8_t *data,
  size_t data_len,
  const struct sockaddr *addr,
  bool should_free
);

static void
alloc_buffer(uv_handle_t *handle, size_t size, uv_buf_t *buf);

static void
after_send(uv_udp_send_t *req, int status);

static void
after_recv(
  uv_udp_t *socket,
  ssize_t nread,
  const uv_buf_t *buf,
  const struct sockaddr *addr,
  unsigned flags
);

static void
after_close(uv_handle_t *handle);

/*
 * Root Nameserver
 */

int
hsk_ns_init(hsk_ns_t *ns, const uv_loop_t *loop, const hsk_pool_t *pool) {
  if (!ns || !loop || !pool)
    return HSK_EBADARGS;

  hsk_ec_t *ec = hsk_ec_alloc();

  if (!ec)
    return HSK_ENOMEM;

  ns->loop = (uv_loop_t *)loop;
  ns->pool = (hsk_pool_t *)pool;
  hsk_addr_init(&ns->ip_);
  ns->ip = NULL;
  ns->socket.data = (void *)ns;
  ns->ec = ec;
  hsk_cache_init(&ns->cache);
  memset(ns->key_, 0x00, sizeof(ns->key_));
  ns->key = NULL;
  memset(ns->pubkey, 0x00, sizeof(ns->pubkey));
  memset(ns->read_buffer, 0x00, sizeof(ns->read_buffer));
  ns->bound = false;
  ns->receiving = false;

  return HSK_SUCCESS;
}

void
hsk_ns_uninit(hsk_ns_t *ns) {
  if (!ns)
    return;

  ns->socket.data = NULL;

  if (ns->ec) {
    hsk_ec_free(ns->ec);
    ns->ec = NULL;
  }

  hsk_cache_uninit(&ns->cache);
}

bool
hsk_ns_set_ip(hsk_ns_t *ns, const struct sockaddr *addr) {
  assert(ns);

  if (!addr) {
    hsk_addr_init(&ns->ip_);
    ns->ip = NULL;
    return true;
  }

  if (!hsk_addr_from_sa(&ns->ip_, addr))
    return false;

  if (!hsk_addr_localize(&ns->ip_))
    return false;

  ns->ip = &ns->ip_;

  return true;
}

bool
hsk_ns_set_key(hsk_ns_t *ns, const uint8_t *key) {
  assert(ns);

  if (!key) {
    memset(ns->key_, 0x00, 32);
    ns->key = NULL;
    memset(ns->pubkey, 0x00, sizeof(ns->pubkey));
    return true;
  }

  if (!hsk_ec_create_pubkey(ns->ec, key, ns->pubkey))
    return false;

  memcpy(&ns->key_[0], key, 32);
  ns->key = &ns->key_[0];

  return true;
}

int
hsk_ns_open(hsk_ns_t *ns, const struct sockaddr *addr) {
  if (!ns || !addr)
    return HSK_EBADARGS;

  if (uv_udp_init(ns->loop, &ns->socket) != 0)
    return HSK_EFAILURE;

  ns->socket.data = (void *)ns;

  if (uv_udp_bind(&ns->socket, addr, 0) != 0)
    return HSK_EFAILURE;

  ns->bound = true;

  int value = sizeof(ns->read_buffer);

  if (uv_send_buffer_size((uv_handle_t *)&ns->socket, &value) != 0)
    return HSK_EFAILURE;

  if (uv_recv_buffer_size((uv_handle_t *)&ns->socket, &value) != 0)
    return HSK_EFAILURE;

  if (uv_udp_recv_start(&ns->socket, alloc_buffer, after_recv) != 0)
    return HSK_EFAILURE;

  ns->receiving = true;

  if (!ns->ip)
    hsk_ns_set_ip(ns, addr);

  char host[HSK_MAX_HOST];
  assert(hsk_sa_to_string(addr, host, HSK_MAX_HOST, HSK_NS_PORT));

  hsk_ns_log(ns, "root nameserver listening on: %s\n", host);

  return HSK_SUCCESS;
}

int
hsk_ns_close(hsk_ns_t *ns) {
  if (!ns)
    return HSK_EBADARGS;

  if (ns->receiving) {
    if (uv_udp_recv_stop(&ns->socket) != 0)
      return HSK_EFAILURE;
    ns->receiving = false;
  }

  if (ns->bound) {
    uv_close((uv_handle_t *)&ns->socket, after_close);
    ns->bound = false;
  }

  ns->socket.data = NULL;

  return HSK_SUCCESS;
}

hsk_ns_t *
hsk_ns_alloc(const uv_loop_t *loop, const hsk_pool_t *pool) {
  hsk_ns_t *ns = malloc(sizeof(hsk_ns_t));

  if (!ns)
    return NULL;

  if (hsk_ns_init(ns, loop, pool) != HSK_SUCCESS) {
    free(ns);
    return NULL;
  }

  return ns;
}

void
hsk_ns_free(hsk_ns_t *ns) {
  if (!ns)
    return;

  hsk_ns_uninit(ns);
  free(ns);
}

int
hsk_ns_destroy(hsk_ns_t *ns) {
  if (!ns)
    return HSK_EBADARGS;

  int rc = hsk_ns_close(ns);

  if (rc != 0)
    return rc;

  hsk_ns_free(ns);

  return HSK_SUCCESS;
}

static void
hsk_ns_log(hsk_ns_t *ns, const char *fmt, ...) {
  printf("ns: ");

  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);
}

static void
hsk_ns_onrecv(
  hsk_ns_t *ns,
  const uint8_t *data,
  size_t data_len,
  const struct sockaddr *addr,
  uint32_t flags
) {
  hsk_dns_req_t *req = hsk_dns_req_create(data, data_len, addr);

  if (!req) {
    hsk_ns_log(ns, "failed processing dns request\n");
    return;
  }

  hsk_dns_req_print(req, "ns: ");

  uint8_t *wire = NULL;
  size_t wire_len = 0;
  hsk_dns_msg_t *msg = NULL;

  // Hit cache first.
  msg = hsk_cache_get(&ns->cache, req);

  if (msg) {
    if (!hsk_dns_msg_finalize(&msg, req, ns->ec, ns->key, &wire, &wire_len)) {
      hsk_ns_log(ns, "could not reply\n");
      goto fail;
    }

    hsk_ns_log(ns, "sending cached msg (%u): %u\n", req->id, wire_len);

    hsk_ns_send(ns, wire, wire_len, addr, true);

    goto done;
  }

  // Requesting a lookup.
  if (req->labels > 0) {
    req->ns = (void *)ns;

    int rc = hsk_pool_resolve(
      ns->pool,
      req->tld,
      after_resolve,
      (void *)req
    );

    if (rc != HSK_SUCCESS) {
      hsk_ns_log(ns, "pool resolve error: %s\n", hsk_strerror(rc));
      goto fail;
    }

    return;
  }

  // Querying the root zone.
  msg = hsk_resource_root(req->type, ns->ip);

  if (!msg) {
    hsk_ns_log(ns, "could not create root soa\n");
    goto fail;
  }

  hsk_cache_insert(&ns->cache, req, msg);

  if (!hsk_dns_msg_finalize(&msg, req, ns->ec, ns->key, &wire, &wire_len)) {
    hsk_ns_log(ns, "could not reply\n");
    goto fail;
  }

  hsk_ns_log(ns, "sending root soa (%u): %u\n", req->id, wire_len);

  hsk_ns_send(ns, wire, wire_len, addr, true);

  goto done;

fail:
  assert(!msg);

  msg = hsk_resource_to_servfail();

  if (!msg) {
    hsk_ns_log(ns, "failed creating servfail\n");
    goto done;
  }

  if (!hsk_dns_msg_finalize(&msg, req, ns->ec, ns->key, &wire, &wire_len)) {
    hsk_ns_log(ns, "could not reply\n");
    goto done;
  }

  hsk_ns_log(ns, "sending servfail (%u): %u\n", req->id, wire_len);

  hsk_ns_send(ns, wire, wire_len, addr, true);

done:
  if (req)
    hsk_dns_req_free(req);
}

static void
hsk_ns_respond(
  hsk_ns_t *ns,
  const hsk_dns_req_t *req,
  int status,
  const hsk_resource_t *res
) {
  hsk_dns_msg_t *msg = NULL;
  uint8_t *wire = NULL;
  size_t wire_len = 0;

  if (status != HSK_SUCCESS) {
    // Pool resolve error.
    hsk_ns_log(ns, "resolve response error: %s\n", hsk_strerror(status));
  } else if (!res) {
    // Doesn't exist.
    //
    // We should be giving a real NSEC proof
    // here, but I don't think it's possible
    // with the current construction.
    //
    // I imagine this would only be possible
    // if NSEC3 begins to support BLAKE2b for
    // name hashing. Even then, it's still
    // not possible for SPV nodes since they
    // can't arbitrarily iterate over the tree.
    //
    // Instead, we give a phony proof, which
    // makes the root zone look empty.
    msg = hsk_resource_to_nx();

    if (!msg)
      hsk_ns_log(ns, "could not create nx response (%u)\n", req->id);
    else
      hsk_ns_log(ns, "sending nxdomain (%u)\n", req->id);
  } else {
    // Exists!
    msg = hsk_resource_to_dns(res, req->name, req->type);

    if (!msg)
      hsk_ns_log(ns, "could not create dns response (%u)\n", req->id);
    else
      hsk_ns_log(ns, "sending msg (%u)\n", req->id);
  }

  if (msg) {
    hsk_cache_insert(&ns->cache, req, msg);

    if (!hsk_dns_msg_finalize(&msg, req, ns->ec, ns->key, &wire, &wire_len)) {
      assert(!msg && !wire);
      hsk_ns_log(ns, "could not finalize\n");
    }
  }

  if (!wire) {
    // Send SERVFAIL in case of error.
    assert(!msg);

    msg = hsk_resource_to_servfail();

    if (!msg) {
      hsk_ns_log(ns, "could not create servfail response\n");
      return;
    }

    if (!hsk_dns_msg_finalize(&msg, req, ns->ec, ns->key, &wire, &wire_len)) {
      hsk_ns_log(ns, "could not create servfail\n");
      return;
    }

    hsk_ns_log(ns, "sending servfail (%u): %u\n", req->id, wire_len);
  }

  hsk_ns_send(ns, wire, wire_len, req->addr, true);
}

int
hsk_ns_send(
  hsk_ns_t *ns,
  uint8_t *data,
  size_t data_len,
  const struct sockaddr *addr,
  bool should_free
) {
  int rc = HSK_SUCCESS;
  hsk_send_data_t *sd = NULL;
  uv_udp_send_t *req = NULL;

  sd = (hsk_send_data_t *)malloc(sizeof(hsk_send_data_t));

  if (!sd) {
    rc = HSK_ENOMEM;
    goto fail;
  }

  req = (uv_udp_send_t *)malloc(sizeof(uv_udp_send_t));

  if (!req) {
    rc = HSK_ENOMEM;
    goto fail;
  }

  sd->ns = ns;
  sd->data = (void *)data;
  sd->should_free = should_free;

  req->data = (void *)sd;

  uv_buf_t bufs[] = {
    { .base = (char *)data, .len = data_len }
  };

  int status = uv_udp_send(req, &ns->socket, bufs, 1, addr, after_send);

  if (status != 0) {
    hsk_ns_log(ns, "failed sending: %s\n", uv_strerror(status));
    rc = HSK_EFAILURE;
    goto fail;
  }

  return rc;

fail:
  if (sd)
    free(sd);

  if (req)
    free(req);

  if (data && should_free)
    free(data);

  return rc;
}

/*
 * UV behavior
 */

static void
alloc_buffer(uv_handle_t *handle, size_t size, uv_buf_t *buf) {
  hsk_ns_t *ns = (hsk_ns_t *)handle->data;

  if (!ns) {
    buf->base = NULL;
    buf->len = 0;
    return;
  }

  buf->base = (char *)ns->read_buffer;
  buf->len = sizeof(ns->read_buffer);
}

static void
after_send(uv_udp_send_t *req, int status) {
  hsk_send_data_t *sd = (hsk_send_data_t *)req->data;
  hsk_ns_t *ns = sd->ns;

  if (sd->data && sd->should_free)
    free(sd->data);

  free(sd);
  free(req);

  if (!ns)
    return;

  if (status != 0) {
    hsk_ns_log(ns, "send error: %s\n", uv_strerror(status));
    return;
  }
}

static void
after_recv(
  uv_udp_t *socket,
  ssize_t nread,
  const uv_buf_t *buf,
  const struct sockaddr *addr,
  unsigned flags
) {
  hsk_ns_t *ns = (hsk_ns_t *)socket->data;

  if (!ns)
    return;

  if (nread < 0) {
    hsk_ns_log(ns, "udp read error: %s\n", uv_strerror(nread));
    return;
  }

  // No more data to read.
  // Happens after every msg?
  if (nread == 0 && addr == NULL)
    return;

  // Never seems to happen on its own.
  if (addr == NULL)
    return;

  hsk_ns_onrecv(
    ns,
    (uint8_t *)buf->base,
    (size_t)nread,
    (struct sockaddr *)addr,
    (uint32_t)flags
  );
}

static void
after_close(uv_handle_t *handle) {
  // hsk_ns_t *ns = (hsk_ns_t *)handle->data;
  // assert(ns);
  // handle->data = NULL;
  // ns->bound = false;
  // hsk_ns_free(peer);
}

static void
after_resolve(
  const char *name,
  int status,
  bool exists,
  const uint8_t *data,
  size_t data_len,
  const void *arg
) {
  hsk_dns_req_t *req = (hsk_dns_req_t *)arg;
  hsk_ns_t *ns = (hsk_ns_t *)req->ns;
  hsk_resource_t *res = NULL;

  if (status == HSK_SUCCESS && exists) {
    if (!hsk_resource_decode(data, data_len, &res)) {
      hsk_ns_log(ns, "could not decode resource for: %s\n", name);
      status = HSK_EFAILURE;
      res = NULL;
    }
  }

  hsk_ns_respond(ns, req, status, res);

  if (res)
    hsk_resource_free(res);

  hsk_dns_req_free(req);
}
