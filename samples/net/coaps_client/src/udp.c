/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <net/net_core.h>
#include <net/net_context.h>
#include <net/nbuf.h>
#include <net/net_if.h>
#include <string.h>
#include <errno.h>
#include <misc/printk.h>

#if !defined(CONFIG_MBEDTLS_CFG_FILE)
#include "mbedtls/config.h"
#else
#include CONFIG_MBEDTLS_CFG_FILE
#endif
#include "mbedtls/ssl.h"

#include "udp_cfg.h"
#include "udp.h"

static struct in6_addr server_addr;
static struct in6_addr mcast_addr = MCAST_IP_ADDR;
static const socklen_t addrlen = sizeof(struct sockaddr_in6);

static void set_destination(struct sockaddr *addr)
{
	struct sockaddr_in6 *dst_addr = (struct sockaddr_in6 *)addr;

	net_ipaddr_copy(&dst_addr->sin6_addr, &server_addr);
	dst_addr->sin6_family = AF_INET6;
	dst_addr->sin6_port = htons(SERVER_PORT);
}

static void udp_received(struct net_context *context,
			 struct net_buf *buf, int status, void *user_data)
{
	struct udp_context *ctx = user_data;

	ARG_UNUSED(context);
	ARG_UNUSED(status);

	ctx->rx_nbuf = buf;
	k_sem_give(&ctx->rx_sem);
}

int udp_tx(void *context, const unsigned char *buf, size_t size)
{
	struct udp_context *ctx = context;
	struct net_context *udp_ctx;
	struct net_buf *send_buf;
	struct sockaddr dst_addr;
	int rc, len;

	udp_ctx = ctx->net_ctx;

	send_buf = net_nbuf_get_tx(udp_ctx);
	if (!send_buf) {
		return MBEDTLS_ERR_SSL_ALLOC_FAILED;
	}

	rc = net_nbuf_append(send_buf, size, (uint8_t *) buf);
	if (!rc) {
		return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	}

	set_destination(&dst_addr);
	len = net_buf_frags_len(send_buf);
	k_sleep(UDP_TX_TIMEOUT);

	rc = net_context_sendto(send_buf, &dst_addr,
				addrlen, NULL, K_FOREVER, NULL, NULL);
	if (rc < 0) {
		net_nbuf_unref(send_buf);
		return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	} else {
		return len;
	}
}

int udp_rx(void *context, unsigned char *buf, size_t size, uint32_t timeout)
{
	struct udp_context *ctx = context;
	struct net_buf *rx_buf = NULL;
	uint16_t read_bytes;
	uint8_t *ptr;
	int pos;
	int len;
	int rc;

	rc = k_sem_take(&ctx->rx_sem, timeout == 0 ? K_FOREVER : timeout);
	if (rc != 0) {
		return MBEDTLS_ERR_SSL_TIMEOUT;
	}

	read_bytes = net_nbuf_appdatalen(ctx->rx_nbuf);
	if (read_bytes > size) {
		return MBEDTLS_ERR_SSL_ALLOC_FAILED;
	}

	ptr = net_nbuf_appdata(ctx->rx_nbuf);
	rx_buf = ctx->rx_nbuf->frags;
	len = rx_buf->len - (ptr - rx_buf->data);
	pos = 0;

	while (rx_buf) {
		memcpy(buf + pos, ptr, len);
		pos += len;

		rx_buf = rx_buf->frags;
		if (!rx_buf) {
			break;
		}

		ptr = rx_buf->data;
		len = rx_buf->len;
	}

	net_nbuf_unref(ctx->rx_nbuf);
	ctx->rx_nbuf = NULL;

	if (read_bytes != pos) {
		return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	}

	rc = read_bytes;
	ctx->remaining = 0;

	return rc;
}

int udp_init(struct udp_context *ctx)
{
	struct net_context *udp_ctx = { 0 };
	struct net_context *mcast_ctx = { 0 };
	struct sockaddr_in6 my_addr = { 0 };
	struct sockaddr_in6 my_mcast_addr = { 0 };
	int rc;

	k_sem_init(&ctx->rx_sem, 0, UINT_MAX);

	net_ipaddr_copy(&my_mcast_addr.sin6_addr, &mcast_addr);
	my_mcast_addr.sin6_family = AF_INET6;

	net_ipaddr_copy(&my_addr.sin6_addr, &client_addr);
	my_addr.sin6_family = AF_INET6;
	my_addr.sin6_port = htons(CLIENT_PORT);

	rc = net_context_get(AF_INET6, SOCK_DGRAM, IPPROTO_UDP, &udp_ctx);
	if (rc < 0) {
		printk("Cannot get network context for IPv6 UDP (%d)", rc);
		return -EIO;
	}

	rc = net_context_bind(udp_ctx, (struct sockaddr *)&my_addr,
			      sizeof(struct sockaddr_in6));
	if (rc < 0) {
		printk("Cannot bind IPv6 UDP port %d (%d)", CLIENT_PORT, rc);
		goto error;
	}

	rc = net_context_get(AF_INET6, SOCK_DGRAM, IPPROTO_UDP, &mcast_ctx);
	if (rc < 0) {
		printk("Cannot get receiving IPv6 mcast (%d)", rc);
		goto error;
	}

	rc = net_context_bind(mcast_ctx, (struct sockaddr *)&my_mcast_addr,
			      sizeof(struct sockaddr_in6));
	if (rc < 0) {
		printk("Cannot get bind IPv6 mcast (%d)", rc);
		goto error;
	}

	ctx->rx_nbuf = NULL;
	ctx->remaining = 0;
	ctx->net_ctx = udp_ctx;

#if defined(CONFIG_NET_SAMPLES_PEER_IPV6_ADDR)
	if (net_addr_pton(AF_INET6,
			  CONFIG_NET_SAMPLES_PEER_IPV6_ADDR,
			  (struct sockaddr *)&server_addr) < 0) {
		printk("Invalid peer IPv6 address %s",
		       CONFIG_NET_SAMPLES_PEER_IPV6_ADDR);
	}
#endif

	rc = net_context_recv(ctx->net_ctx, udp_received, K_NO_WAIT, ctx);
	if (rc != 0) {
		return -EIO;
	}

	return 0;

error:
	net_context_put(udp_ctx);
	return -EINVAL;
}
