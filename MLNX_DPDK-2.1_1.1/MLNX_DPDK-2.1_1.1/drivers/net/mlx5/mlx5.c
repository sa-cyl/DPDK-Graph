/*-
 *   BSD LICENSE
 *
 *   Copyright 2012-2015 6WIND S.A.
 *   Copyright 2012 Mellanox.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of 6WIND S.A. nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Known limitations:
 * - Hardware counters aren't implemented.
 */

/* System headers. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <assert.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/if.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>

/* Verbs header. */
/* ISO C doesn't support unnamed structs/unions, disabling -pedantic. */
#ifdef PEDANTIC
#pragma GCC diagnostic ignored "-pedantic"
#endif
#include <infiniband/verbs.h>
#ifdef PEDANTIC
#pragma GCC diagnostic error "-pedantic"
#endif

/* DPDK headers don't like -pedantic. */
#ifdef PEDANTIC
#pragma GCC diagnostic ignored "-pedantic"
#endif
#include <rte_config.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_dev.h>
#include <rte_mbuf.h>
#include <rte_errno.h>
#include <rte_mempool.h>
#include <rte_prefetch.h>
#include <rte_malloc.h>
#include <rte_spinlock.h>
#include <rte_atomic.h>
#include <rte_version.h>
#include <rte_log.h>
#ifdef PEDANTIC
#pragma GCC diagnostic error "-pedantic"
#endif

/* Generated configuration header. */
#include "mlx5_autoconf.h"

/* PMD header. */
#include "mlx5.h"

/* Runtime logging through RTE_LOG() is enabled when not in debugging mode.
 * Intermediate LOG_*() macros add the required end-of-line characters. */
#ifndef NDEBUG
#define INFO(...) DEBUG(__VA_ARGS__)
#define WARN(...) DEBUG(__VA_ARGS__)
#define ERROR(...) DEBUG(__VA_ARGS__)
#else
#define LOG__(level, m, ...) \
	RTE_LOG(level, PMD, MLX5_DRIVER_NAME ": " m "%c", __VA_ARGS__)
#define LOG_(level, ...) LOG__(level, __VA_ARGS__, '\n')
#define INFO(...) LOG_(INFO, __VA_ARGS__)
#define WARN(...) LOG_(WARNING, __VA_ARGS__)
#define ERROR(...) LOG_(ERR, __VA_ARGS__)
#endif

/* Convenience macros for accessing mbuf fields. */
#define NEXT(m) ((m)->next)
#define DATA_LEN(m) ((m)->data_len)
#define PKT_LEN(m) ((m)->pkt_len)
#define DATA_OFF(m) ((m)->data_off)
#define SET_DATA_OFF(m, o) ((m)->data_off = (o))
#define NB_SEGS(m) ((m)->nb_segs)
#define PORT(m) ((m)->port)

/* RSS Indirection table size. */
#define RSS_INDIRECTION_TABLE_SIZE 128

/* Transpose flags. Useful to convert IBV to DPDK flags. */
#define TRANSPOSE(val, from, to) \
	(((from) >= (to)) ? \
	 (((val) & (from)) / ((from) / (to))) : \
	 (((val) & (from)) * ((to) / (from))))

struct mlx5_rxq_stats {
	unsigned int idx; /**< Mapping index. */
#ifdef MLX5_PMD_SOFT_COUNTERS
	uint64_t ipackets;  /**< Total of successfully received packets. */
	uint64_t ibytes;    /**< Total of successfully received bytes. */
#endif
	uint64_t idropped;  /**< Total of packets dropped when RX ring full. */
	uint64_t rx_nombuf; /**< Total of RX mbuf allocation failures. */
};

struct mlx5_txq_stats {
	unsigned int idx; /**< Mapping index. */
#ifdef MLX5_PMD_SOFT_COUNTERS
	uint64_t opackets; /**< Total of successfully sent packets. */
	uint64_t obytes;   /**< Total of successfully sent bytes. */
#endif
	uint64_t odropped; /**< Total of packets not sent when TX ring full. */
};

/* RX element (scattered packets). */
struct rxq_elt_sp {
	struct ibv_sge sges[MLX5_PMD_SGE_WR_N]; /* Scatter/Gather Elements. */
	struct rte_mbuf *bufs[MLX5_PMD_SGE_WR_N]; /* SGEs buffers. */
};

/* RX element. */
struct rxq_elt {
	struct ibv_sge sge; /* Scatter/Gather Element. */
	struct rte_mbuf *buf; /* SGE buffer. */
};

/* RX queue descriptor. */
struct rxq {
	struct priv *priv; /* Back pointer to private data. */
	struct rte_mempool *mp; /* Memory Pool for allocations. */
	struct ibv_mr *mr; /* Memory Region (for mp). */
	struct ibv_cq *cq; /* Completion Queue. */
	struct ibv_exp_wq *wq; /* Work Queue. */
	struct ibv_exp_wq_family *if_wq; /* WQ burst interface. */
	struct ibv_exp_cq_family *if_cq; /* CQ interface. */
	unsigned int port_id; /* Port ID for incoming packets. */
	unsigned int elts_n; /* (*elts)[] length. */
	unsigned int elts_head; /* Current index in (*elts)[]. */
	union {
		struct rxq_elt_sp (*sp)[]; /* Scattered RX elements. */
		struct rxq_elt (*no_sp)[]; /* RX elements. */
	} elts;
	unsigned int sp:1; /* Use scattered RX elements. */
	unsigned int csum:1; /* Enable checksum offloading. */
	unsigned int csum_l2tun:1; /* Same for L2 tunnels. */
	uint32_t mb_len; /* Length of a mp-issued mbuf. */
	struct mlx5_rxq_stats stats; /* RX queue counters. */
	unsigned int socket; /* CPU socket ID for allocations. */
	struct ibv_exp_res_domain *rd; /* Resource Domain. */
};

/* TX element. */
struct txq_elt {
	struct rte_mbuf *buf;
};

/* Linear buffer type. It is used when transmitting buffers with too many
 * segments that do not fit the hardware queue (see max_send_sge).
 * Extra segments are copied (linearized) in such buffers, replacing the
 * last SGE during TX.
 * The size is arbitrary but large enough to hold a jumbo frame with
 * 8 segments considering mbuf.buf_len is about 2048 bytes. */
typedef uint8_t linear_t[16384];

/* TX queue descriptor. */
struct txq {
	struct priv *priv; /* Back pointer to private data. */
	struct {
		struct rte_mempool *mp; /* Cached Memory Pool. */
		struct ibv_mr *mr; /* Memory Region (for mp). */
		uint32_t lkey; /* mr->lkey */
	} mp2mr[MLX5_PMD_TX_MP_CACHE]; /* MP to MR translation table. */
	struct ibv_cq *cq; /* Completion Queue. */
	struct ibv_qp *qp; /* Queue Pair. */
	struct ibv_exp_qp_burst_family *if_qp; /* QP burst interface. */
	struct ibv_exp_cq_family *if_cq; /* CQ interface. */
#if MLX5_PMD_MAX_INLINE > 0
	uint32_t max_inline; /* Max inline send size <= MLX5_PMD_MAX_INLINE. */
#endif
	unsigned int elts_n; /* (*elts)[] length. */
	struct txq_elt (*elts)[]; /* TX elements. */
	unsigned int elts_head; /* Current index in (*elts)[]. */
	unsigned int elts_tail; /* First element awaiting completion. */
	unsigned int elts_comp; /* Number of completion requests. */
	unsigned int elts_comp_cd; /* Countdown for next completion request. */
	unsigned int elts_comp_cd_init; /* Initial value for countdown. */
	struct mlx5_txq_stats stats; /* TX queue counters. */
	linear_t (*elts_linear)[]; /* Linearized buffers. */
	struct ibv_mr *mr_linear; /* Memory Region for linearized buffers. */
	unsigned int socket; /* CPU socket ID for allocations. */
	struct ibv_exp_res_domain *rd; /* Resource Domain. */
};

/* Hash RX queue types. */
enum hash_rxq_type {
	HASH_RXQ_TCPv4,
	HASH_RXQ_UDPv4,
	HASH_RXQ_IPv4,
	HASH_RXQ_TCPv6,
	HASH_RXQ_UDPv6,
	HASH_RXQ_IPv6,
	HASH_RXQ_ETH,
};

/* Initialization data for hash RX queue. */
struct hash_rxq_init {
	uint64_t hash_fields; /* Fields that participate in the hash. */
	uint64_t dpdk_rss_hf; /* Matching DPDK RSS hash fields. */
	unsigned int flow_priority; /* Flow priority to use. */
	struct ibv_exp_flow_spec flow_spec; /* Flow specification template. */
	const struct hash_rxq_init *underlayer; /* Pointer to underlayer. */
};

/* Indirection table types. */
enum ind_table_type {
	IND_TABLE_GENERIC, /* TCP, UDP, IPv4, IPv6. */
	IND_TABLE_DRAIN, /* Everything else. */
};

/* Initialization data for indirection table. */
struct ind_table_init {
	unsigned int max_size; /* Maximum number of WQs. */
	/* Hash RX queues using this table. */
	const enum hash_rxq_type (*hash_types)[];
	unsigned int hash_types_n;
};

struct hash_rxq {
	struct priv *priv; /* Back pointer to private data. */
	struct ibv_qp *qp; /* Hash RX QP. */
	enum hash_rxq_type type; /* Hash RX queue type. */
	/* Each VLAN ID requires a separate flow steering rule. */
	BITFIELD_DECLARE(mac_configured, uint32_t, MLX5_MAX_MAC_ADDRESSES);
	struct ibv_exp_flow *mac_flow[MLX5_MAX_MAC_ADDRESSES][MLX5_MAX_VLAN_IDS];
	struct ibv_exp_flow *promisc_flow; /* Promiscuous flow. */
	struct ibv_exp_flow *allmulti_flow; /* Multicast flow. */
};

struct priv {
	struct rte_eth_dev *dev; /* Ethernet device. */
	struct ibv_context *ctx; /* Verbs context. */
	struct ibv_device_attr device_attr; /* Device properties. */
	struct ibv_pd *pd; /* Protection Domain. */
	/*
	 * MAC addresses array and configuration bit-field.
	 * An extra entry that cannot be modified by the DPDK is reserved
	 * for broadcast frames (destination MAC address ff:ff:ff:ff:ff:ff).
	 */
	struct ether_addr mac[MLX5_MAX_MAC_ADDRESSES];
	BITFIELD_DECLARE(mac_configured, uint32_t, MLX5_MAX_MAC_ADDRESSES);
	/* VLAN filters. */
	struct {
		unsigned int enabled:1; /* If enabled. */
		unsigned int id:12; /* VLAN ID (0-4095). */
	} vlan_filter[MLX5_MAX_VLAN_IDS]; /* VLAN filters table. */
	/* Device properties. */
	uint16_t mtu; /* Configured MTU. */
	uint8_t port; /* Physical port number. */
	unsigned int started:1; /* Device started, flows enabled. */
	unsigned int promisc_req:1; /* Promiscuous mode requested. */
	unsigned int promisc:1; /* Device in promiscuous mode. */
	unsigned int allmulti_req:1; /* All multicast mode requested. */
	unsigned int allmulti:1; /* Device receives all multicast packets. */
	unsigned int hw_csum:1; /* Checksum offload is supported. */
	unsigned int hw_csum_l2tun:1; /* Same for L2 tunnels. */
	unsigned int vf:1; /* This is a VF device. */
	/* RX/TX queues. */
	unsigned int rxqs_n; /* RX queues array size. */
	unsigned int txqs_n; /* TX queues array size. */
	struct rxq *(*rxqs)[]; /* RX queues. */
	struct txq *(*txqs)[]; /* TX queues. */
	/* Indirection tables referencing all RX WQs. */
	struct ibv_exp_rwq_ind_table *(*ind_tables)[];
	unsigned int ind_tables_n; /* Number of indirection tables. */
	unsigned int ind_table_max_size; /* Maximum indirection table size. */
	/* Hash RX QPs feeding the indirection table. */
	struct hash_rxq (*hash_rxqs)[];
	unsigned int hash_rxqs_n; /* Hash RX QPs array size. */
	/* RSS configuration array indexed by hash RX queue type. */
	struct rte_eth_rss_conf *(*rss_conf)[];
	rte_spinlock_t lock; /* Lock for control functions. */
};

/* Initialization data for hash RX queues. */
static const struct hash_rxq_init hash_rxq_init[] = {
	[HASH_RXQ_TCPv4] = {
		.hash_fields = (IBV_EXP_RX_HASH_SRC_IPV4 |
				IBV_EXP_RX_HASH_DST_IPV4 |
				IBV_EXP_RX_HASH_SRC_PORT_TCP |
				IBV_EXP_RX_HASH_DST_PORT_TCP),
		.dpdk_rss_hf = ETH_RSS_NONFRAG_IPV4_TCP,
		.flow_priority = 0,
		.flow_spec = {
			{.tcp_udp = {
				.type = IBV_EXP_FLOW_SPEC_TCP,
				.size = sizeof(hash_rxq_init[0].flow_spec.tcp_udp),
			}},
		},
		.underlayer = &hash_rxq_init[HASH_RXQ_IPv4],
	},
	[HASH_RXQ_UDPv4] = {
		.hash_fields = (IBV_EXP_RX_HASH_SRC_IPV4 |
				IBV_EXP_RX_HASH_DST_IPV4 |
				IBV_EXP_RX_HASH_SRC_PORT_UDP |
				IBV_EXP_RX_HASH_DST_PORT_UDP),
		.dpdk_rss_hf = ETH_RSS_NONFRAG_IPV4_UDP,
		.flow_priority = 0,
		.flow_spec = {
			{.tcp_udp = {
				.type = IBV_EXP_FLOW_SPEC_UDP,
				.size = sizeof(hash_rxq_init[0].flow_spec.tcp_udp),
			}},
		},
		.underlayer = &hash_rxq_init[HASH_RXQ_IPv4],
	},
	[HASH_RXQ_IPv4] = {
		.hash_fields = (IBV_EXP_RX_HASH_SRC_IPV4 |
				IBV_EXP_RX_HASH_DST_IPV4),
		.dpdk_rss_hf = (ETH_RSS_IPV4 |
				ETH_RSS_FRAG_IPV4),
		.flow_priority = 1,
		.flow_spec = {
			{.ipv4 = {
				.type = IBV_EXP_FLOW_SPEC_IPV4,
				.size = sizeof(hash_rxq_init[0].flow_spec.ipv4),
			}},
		},
		.underlayer = &hash_rxq_init[HASH_RXQ_ETH],
	},
#ifdef HASH_RXQ_IPV6
	[HASH_RXQ_TCPv6] = {
		.hash_fields = (IBV_EXP_RX_HASH_SRC_IPV6 |
				IBV_EXP_RX_HASH_DST_IPV6 |
				IBV_EXP_RX_HASH_SRC_PORT_TCP |
				IBV_EXP_RX_HASH_DST_PORT_TCP),
		.dpdk_rss_hf = ETH_RSS_NONFRAG_IPV6_TCP,
		.flow_priority = 0,
		.flow_spec = {
			{.tcp_udp = {
				.type = IBV_EXP_FLOW_SPEC_TCP,
				.size = sizeof(hash_rxq_init[0].flow_spec.tcp_udp),
			}},
		},
		.underlayer = &hash_rxq_init[HASH_RXQ_IPv6],
	},
	[HASH_RXQ_UDPv6] = {
		.hash_fields = (IBV_EXP_RX_HASH_SRC_IPV6 |
				IBV_EXP_RX_HASH_DST_IPV6 |
				IBV_EXP_RX_HASH_SRC_PORT_UDP |
				IBV_EXP_RX_HASH_DST_PORT_UDP),
		.dpdk_rss_hf = ETH_RSS_NONFRAG_IPV6_UDP,
		.flow_priority = 0,
		.flow_spec = {
			{.tcp_udp = {
				.type = IBV_EXP_FLOW_SPEC_UDP,
				.size = sizeof(hash_rxq_init[0].flow_spec.tcp_udp),
			}},
		},
		.underlayer = &hash_rxq_init[HASH_RXQ_IPv6],
	},
	[HASH_RXQ_IPv6] = {
		.hash_fields = (IBV_EXP_RX_HASH_SRC_IPV6 |
				IBV_EXP_RX_HASH_DST_IPV6),
		.dpdk_rss_hf = (ETH_RSS_IPV6 |
				ETH_RSS_FRAG_IPV6),
		.flow_priority = 1,
		.flow_spec = {
			{.ipv6 = {
				.type = IBV_EXP_FLOW_SPEC_IPV6,
				.size = sizeof(hash_rxq_init[0].flow_spec.ipv6),
			}},
		},
		.underlayer = &hash_rxq_init[HASH_RXQ_ETH],
	},
#endif /* HASH_RXQ_IPV6 */
	[HASH_RXQ_ETH] = {
		.hash_fields = 0,
		.dpdk_rss_hf = 0,
		.flow_priority = 2,
		.flow_spec = {
			{.eth = {
				.type = IBV_EXP_FLOW_SPEC_ETH,
				.size = sizeof(hash_rxq_init[0].flow_spec.eth),
			}},
		},
		.underlayer = NULL,
	},
};

/* Initialization data for hash RX queue indirection tables. */
static const struct ind_table_init ind_table_init[] = {
	[IND_TABLE_GENERIC] = {
		.max_size = -1u, /* Superseded by HW limitations. */
		.hash_types = &(const enum hash_rxq_type []){
			HASH_RXQ_TCPv4,
			HASH_RXQ_UDPv4,
			HASH_RXQ_IPv4,
#ifdef HASH_RXQ_IPV6
			HASH_RXQ_TCPv6,
			HASH_RXQ_UDPv6,
			HASH_RXQ_IPv6,
#endif /* HASH_RXQ_IPV6 */
		},
#ifdef HASH_RXQ_IPV6
		.hash_types_n = 6,
#else /* HASH_RXQ_IPV6 */
		.hash_types_n = 3,
#endif
	},
	[IND_TABLE_DRAIN] = {
		.max_size = 1,
		.hash_types = &(const enum hash_rxq_type []){
			HASH_RXQ_ETH,
		},
		.hash_types_n = 1,
	},
};

/* Indirection tables to initialize when RSS is enabled. */
static const struct ind_table_init *const ind_table_init_rss[] = {
	&ind_table_init[IND_TABLE_GENERIC],
	&ind_table_init[IND_TABLE_DRAIN],
	NULL,
};

/* Indirection tables to initialize when RSS is disabled. */
static const struct ind_table_init *const ind_table_init_no_rss[] = {
	&ind_table_init[IND_TABLE_DRAIN],
	NULL,
};

/* Default RSS hash key also used for ConnectX-3. */
static uint8_t rss_hash_default_key[] = {
	0x2c, 0xc6, 0x81, 0xd1,
	0x5b, 0xdb, 0xf4, 0xf7,
	0xfc, 0xa2, 0x83, 0x19,
	0xdb, 0x1a, 0x3e, 0x94,
	0x6b, 0x9e, 0x38, 0xd9,
	0x2c, 0x9c, 0x03, 0xd1,
	0xad, 0x99, 0x44, 0xa7,
	0xd9, 0x56, 0x3d, 0x59,
	0x06, 0x3c, 0x25, 0xf3,
	0xfc, 0x1f, 0xdc, 0x2a,
};

/* Flow structure with Ethernet specification. It is packed to prevent padding
 * between attr and spec as this layout is expected by libibverbs. */
struct flow_attr_spec_eth {
	struct ibv_exp_flow_attr attr;
	struct ibv_exp_flow_spec_eth spec;
} __attribute__((packed));

/* Define a struct flow_attr_spec_eth object as an array of at least
 * "size" bytes. Room after the first index is normally used to store
 * extra flow specifications. */
#define FLOW_ATTR_SPEC_ETH(name, size) \
	struct flow_attr_spec_eth name \
		[((size) / sizeof(struct flow_attr_spec_eth)) + \
		 !!((size) % sizeof(struct flow_attr_spec_eth))]

/**
 * Lock private structure to protect it from concurrent access in the
 * control path.
 *
 * @param priv
 *   Pointer to private structure.
 */
static void
priv_lock(struct priv *priv)
{
	rte_spinlock_lock(&priv->lock);
}

/**
 * Unlock private structure.
 *
 * @param priv
 *   Pointer to private structure.
 */
static void
priv_unlock(struct priv *priv)
{
	rte_spinlock_unlock(&priv->lock);
}

/* Allocate a buffer on the stack and fill it with a printf format string. */
#define MKSTR(name, ...) \
	char name[snprintf(NULL, 0, __VA_ARGS__) + 1]; \
	\
	snprintf(name, sizeof(name), __VA_ARGS__)

/**
 * Get interface name from private structure.
 *
 * @param[in] priv
 *   Pointer to private structure.
 * @param[out] ifname
 *   Interface name output buffer.
 *
 * @return
 *   0 on success, -1 on failure and errno is set.
 */
static int
priv_get_ifname(const struct priv *priv, char (*ifname)[IF_NAMESIZE])
{
	DIR *dir;
	struct dirent *dent;
	unsigned int dev_type = 0;
	unsigned int dev_port_prev = ~0u;
	char match[IF_NAMESIZE] = "";

	{
		MKSTR(path, "%s/device/net", priv->ctx->device->ibdev_path);

		dir = opendir(path);
		if (dir == NULL)
			return -1;
	}
	while ((dent = readdir(dir)) != NULL) {
		char *name = dent->d_name;
		FILE *file;
		unsigned int dev_port;
		int r;

		if ((name[0] == '.') &&
		    ((name[1] == '\0') ||
		     ((name[1] == '.') && (name[2] == '\0'))))
			continue;

		MKSTR(path, "%s/device/net/%s/%s",
		      priv->ctx->device->ibdev_path, name,
		      (dev_type ? "dev_id" : "dev_port"));

		file = fopen(path, "rb");
		if (file == NULL) {
			if (errno != ENOENT)
				continue;
			/*
			 * Switch to dev_id when dev_port does not exist as
			 * is the case with Linux kernel versions < 3.15.
			 */
try_dev_id:
			match[0] = '\0';
			if (dev_type)
				break;
			dev_type = 1;
			dev_port_prev = ~0u;
			rewinddir(dir);
			continue;
		}
		r = fscanf(file, (dev_type ? "%x" : "%u"), &dev_port);
		fclose(file);
		if (r != 1)
			continue;
		/*
		 * Switch to dev_id when dev_port returns the same value for
		 * all ports. May happen when using a MOFED release older than
		 * 3.0 with a Linux kernel >= 3.15.
		 */
		if (dev_port == dev_port_prev)
			goto try_dev_id;
		dev_port_prev = dev_port;
		if (dev_port == (priv->port - 1u))
			snprintf(match, sizeof(match), "%s", name);
	}
	closedir(dir);
	if (match[0] == '\0')
		return -1;
	strncpy(*ifname, match, sizeof(*ifname));
	return 0;
}

/**
 * Read from sysfs entry.
 *
 * @param[in] priv
 *   Pointer to private structure.
 * @param[in] entry
 *   Entry name relative to sysfs path.
 * @param[out] buf
 *   Data output buffer.
 * @param size
 *   Buffer size.
 *
 * @return
 *   0 on success, -1 on failure and errno is set.
 */
static int
priv_sysfs_read(const struct priv *priv, const char *entry,
		char *buf, size_t size)
{
	char ifname[IF_NAMESIZE];
	FILE *file;
	int ret;
	int err;

	if (priv_get_ifname(priv, &ifname))
		return -1;

	MKSTR(path, "%s/device/net/%s/%s", priv->ctx->device->ibdev_path,
	      ifname, entry);

	file = fopen(path, "rb");
	if (file == NULL)
		return -1;
	ret = fread(buf, 1, size, file);
	err = errno;
	if (((size_t)ret < size) && (ferror(file)))
		ret = -1;
	else
		ret = size;
	fclose(file);
	errno = err;
	return ret;
}

/**
 * Write to sysfs entry.
 *
 * @param[in] priv
 *   Pointer to private structure.
 * @param[in] entry
 *   Entry name relative to sysfs path.
 * @param[in] buf
 *   Data buffer.
 * @param size
 *   Buffer size.
 *
 * @return
 *   0 on success, -1 on failure and errno is set.
 */
static int
priv_sysfs_write(const struct priv *priv, const char *entry,
		 char *buf, size_t size)
{
	char ifname[IF_NAMESIZE];
	FILE *file;
	int ret;
	int err;

	if (priv_get_ifname(priv, &ifname))
		return -1;

	MKSTR(path, "%s/device/net/%s/%s", priv->ctx->device->ibdev_path,
	      ifname, entry);

	file = fopen(path, "wb");
	if (file == NULL)
		return -1;
	ret = fwrite(buf, 1, size, file);
	err = errno;
	if (((size_t)ret < size) || (ferror(file)))
		ret = -1;
	else
		ret = size;
	fclose(file);
	errno = err;
	return ret;
}

/**
 * Get unsigned long sysfs property.
 *
 * @param priv
 *   Pointer to private structure.
 * @param[in] name
 *   Entry name relative to sysfs path.
 * @param[out] value
 *   Value output buffer.
 *
 * @return
 *   0 on success, -1 on failure and errno is set.
 */
static int
priv_get_sysfs_ulong(struct priv *priv, const char *name, unsigned long *value)
{
	int ret;
	unsigned long value_ret;
	char value_str[32];

	ret = priv_sysfs_read(priv, name, value_str, (sizeof(value_str) - 1));
	if (ret == -1) {
		DEBUG("cannot read %s value from sysfs: %s",
		      name, strerror(errno));
		return -1;
	}
	value_str[ret] = '\0';
	errno = 0;
	value_ret = strtoul(value_str, NULL, 0);
	if (errno) {
		DEBUG("invalid %s value `%s': %s", name, value_str,
		      strerror(errno));
		return -1;
	}
	*value = value_ret;
	return 0;
}

/**
 * Set unsigned long sysfs property.
 *
 * @param priv
 *   Pointer to private structure.
 * @param[in] name
 *   Entry name relative to sysfs path.
 * @param value
 *   Value to set.
 *
 * @return
 *   0 on success, -1 on failure and errno is set.
 */
static int
priv_set_sysfs_ulong(struct priv *priv, const char *name, unsigned long value)
{
	int ret;
	MKSTR(value_str, "%lu", value);

	ret = priv_sysfs_write(priv, name, value_str, (sizeof(value_str) - 1));
	if (ret == -1) {
		DEBUG("cannot write %s `%s' (%lu) to sysfs: %s",
		      name, value_str, value, strerror(errno));
		return -1;
	}
	return 0;
}

/**
 * Perform ifreq ioctl() on associated Ethernet device.
 *
 * @param[in] priv
 *   Pointer to private structure.
 * @param req
 *   Request number to pass to ioctl().
 * @param[out] ifr
 *   Interface request structure output buffer.
 *
 * @return
 *   0 on success, -1 on failure and errno is set.
 */
static int
priv_ifreq(const struct priv *priv, int req, struct ifreq *ifr)
{
	int sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
	int ret = -1;

	if (sock == -1)
		return ret;
	if (priv_get_ifname(priv, &ifr->ifr_name) == 0)
		ret = ioctl(sock, req, ifr);
	close(sock);
	return ret;
}

/**
 * Get device MTU.
 *
 * @param priv
 *   Pointer to private structure.
 * @param[out] mtu
 *   MTU value output buffer.
 *
 * @return
 *   0 on success, -1 on failure and errno is set.
 */
static int
priv_get_mtu(struct priv *priv, uint16_t *mtu)
{
	unsigned long ulong_mtu;

	if (priv_get_sysfs_ulong(priv, "mtu", &ulong_mtu) == -1)
		return -1;
	*mtu = ulong_mtu;
	return 0;
}

/**
 * Set device MTU.
 *
 * @param priv
 *   Pointer to private structure.
 * @param mtu
 *   MTU value to set.
 *
 * @return
 *   0 on success, -1 on failure and errno is set.
 */
static int
priv_set_mtu(struct priv *priv, uint16_t mtu)
{
	return priv_set_sysfs_ulong(priv, "mtu", mtu);
}

/**
 * Set device flags.
 *
 * @param priv
 *   Pointer to private structure.
 * @param keep
 *   Bitmask for flags that must remain untouched.
 * @param flags
 *   Bitmask for flags to modify.
 *
 * @return
 *   0 on success, -1 on failure and errno is set.
 */
static int
priv_set_flags(struct priv *priv, unsigned int keep, unsigned int flags)
{
	unsigned long tmp;

	if (priv_get_sysfs_ulong(priv, "flags", &tmp) == -1)
		return -1;
	tmp &= keep;
	tmp |= flags;
	return priv_set_sysfs_ulong(priv, "flags", tmp);
}

/**
 * Return nearest power of two above input value.
 *
 * @param v
 *   Input value.
 *
 * @return
 *   Nearest power of two above input value.
 */
static unsigned int
log2above(unsigned int v)
{
	unsigned int l;
	unsigned int r;

	for (l = 0, r = 0; (v >> 1); ++l, v >>= 1)
		r |= (v & 1);
	return (l + r);
}

/**
 * Initialize RX hash queues and indirection table.
 *
 * @param priv
 *   Pointer to private structure.
 *
 * @return
 *   0 on success, errno value on failure.
 */
static int
priv_create_hash_rxqs(struct priv *priv)
{
	/* If the requested number of WQs is not a power of two, use the
	 * maximum indirection table size for better balancing.
	 * The result is always rounded to the next power of two. */
	unsigned int wqs_n =
		(1 << log2above((priv->rxqs_n & (priv->rxqs_n - 1)) ?
				priv->ind_table_max_size :
				priv->rxqs_n));
	struct ibv_exp_wq *wqs[wqs_n];
	/* If only one RX queue is configured, RSS is not needed. */
	const struct ind_table_init *const *ind_table_init =
		((priv->rxqs_n == 1) ?
		 ind_table_init_no_rss :
		 ind_table_init_rss);
	unsigned int hash_rxqs_n = 0;
	struct hash_rxq (*hash_rxqs)[] = NULL;
	unsigned int ind_tables_n = 0;
	struct ibv_exp_rwq_ind_table *(*ind_tables)[] = NULL;
	unsigned int i;
	unsigned int j;
	unsigned int k;
	int err = 0;

	assert(priv->ind_tables == NULL);
	assert(priv->ind_tables_n == 0);
	assert(priv->hash_rxqs == NULL);
	assert(priv->hash_rxqs_n == 0);
	assert(priv->pd != NULL);
	assert(priv->ctx != NULL);
	if (priv->rxqs_n == 0)
		return EINVAL;
	assert(priv->rxqs != NULL);
	if ((wqs_n < priv->rxqs_n) || (wqs_n > priv->ind_table_max_size)) {
		ERROR("cannot handle this many RX queues (%u)", priv->rxqs_n);
		err = ERANGE;
		goto error;
	}
	if (wqs_n != priv->rxqs_n) {
		INFO("%u RX queues are configured, consider rounding this"
		     " number to the next power of two for better balancing",
		     priv->rxqs_n);
		DEBUG("indirection table extended to assume %u WQs", wqs_n);
	}
	/* When the number of RX queues is not a power of two, the remaining
	 * table entries are padded with reused WQs and hashes are not spread
	 * uniformly. */
	for (i = 0, j = 0; (i != wqs_n); ++i) {
		wqs[i] = (*priv->rxqs)[j]->wq;
		if (++j == priv->rxqs_n)
			j = 0;
	}
	/* Get number of indirection tables and hash RX queues to configure. */
	for (ind_tables_n = 0, hash_rxqs_n = 0;
	     (ind_table_init[ind_tables_n] != NULL);
	     ++ind_tables_n)
		hash_rxqs_n += ind_table_init[ind_tables_n]->hash_types_n;
	DEBUG("allocating %u RX hash queues for %u WQs, %u indirection tables",
	      hash_rxqs_n, priv->rxqs_n, ind_tables_n);
	/* Create indirection tables. */
	ind_tables = rte_calloc(__func__, ind_tables_n,
				sizeof((*ind_tables)[0]), 0);
	if (ind_tables == NULL) {
		err = ENOMEM;
		ERROR("cannot allocate indirection tables container: %s",
		      strerror(err));
		goto error;
	}
	for (i = 0; (i != ind_tables_n); ++i) {
		struct ibv_exp_rwq_ind_table_init_attr ind_init_attr = {
			.pd = priv->pd,
			.log_ind_tbl_size = 0, /* Set below. */
			.ind_tbl = wqs,
			.comp_mask = 0,
		};
		unsigned int ind_tbl_size = ind_table_init[i]->max_size;
		struct ibv_exp_rwq_ind_table *ind_table;

		if (wqs_n < ind_tbl_size)
			ind_tbl_size = wqs_n;
		ind_init_attr.log_ind_tbl_size = log2above(ind_tbl_size);
		errno = 0;
		ind_table = ibv_exp_create_rwq_ind_table(priv->ctx,
							 &ind_init_attr);
		if (ind_table != NULL) {
			(*ind_tables)[i] = ind_table;
			continue;
		}
		/* Not clear whether errno is set. */
		err = (errno ? errno : EINVAL);
		ERROR("RX indirection table creation failed with error %d: %s",
		      err, strerror(err));
		goto error;
	}
	/* Allocate array that holds hash RX queues and related data. */
	hash_rxqs = rte_calloc(__func__, hash_rxqs_n,
			       sizeof((*hash_rxqs)[0]), 0);
	if (hash_rxqs == NULL) {
		err = ENOMEM;
		ERROR("cannot allocate hash RX queues container: %s",
		      strerror(err));
		goto error;
	}
	for (i = 0, j = 0, k = 0;
	     ((i != hash_rxqs_n) && (j != ind_tables_n));
	     ++i) {
		struct hash_rxq *hash_rxq = &(*hash_rxqs)[i];
		enum hash_rxq_type type = (*ind_table_init[j]->hash_types)[k];
		struct rte_eth_rss_conf *priv_rss_conf =
			(*priv->rss_conf)[type];
		struct ibv_exp_rx_hash_conf hash_conf = {
			.rx_hash_function = IBV_EXP_RX_HASH_FUNC_TOEPLITZ,
			.rx_hash_key_len = (priv_rss_conf ?
					    priv_rss_conf->rss_key_len :
					    sizeof(rss_hash_default_key)),
			.rx_hash_key = (priv_rss_conf ?
					priv_rss_conf->rss_key :
					rss_hash_default_key),
			.rx_hash_fields_mask = hash_rxq_init[type].hash_fields,
			.rwq_ind_tbl = (*ind_tables)[j],
		};
		struct ibv_exp_qp_init_attr qp_init_attr = {
			.max_inl_recv = 0, /* Currently not supported. */
			.qp_type = IBV_QPT_RAW_PACKET,
			.comp_mask = (IBV_EXP_QP_INIT_ATTR_PD |
				      IBV_EXP_QP_INIT_ATTR_RX_HASH),
			.pd = priv->pd,
			.rx_hash_conf = &hash_conf,
			.port_num = priv->port,
		};

		DEBUG("using indirection table %u for RX hash queue %u",
		      j, i);
		*hash_rxq = (struct hash_rxq){
			.priv = priv,
			.qp = ibv_exp_create_qp(priv->ctx, &qp_init_attr),
			.type = type,
		};
		if (hash_rxq->qp == NULL) {
			err = (errno ? errno : EINVAL);
			ERROR("RX hash QP creation failure: %s",
			      strerror(err));
			goto error;
		}
		if (++k < ind_table_init[j]->hash_types_n)
			continue;
		/* Switch to the next indirection table and reset hash RX
		 * queue type array index. */
		++j;
		k = 0;
	}
	priv->ind_tables = ind_tables;
	priv->ind_tables_n = ind_tables_n;
	priv->hash_rxqs = hash_rxqs;
	priv->hash_rxqs_n = hash_rxqs_n;
	assert(err == 0);
	return 0;
error:
	if (hash_rxqs != NULL) {
		for (i = 0; (i != hash_rxqs_n); ++i) {
			struct ibv_qp *qp = (*hash_rxqs)[i].qp;

			if (qp == NULL)
				continue;
			claim_zero(ibv_destroy_qp(qp));
		}
		rte_free(hash_rxqs);
	}
	if (ind_tables != NULL) {
		for (j = 0; (j != ind_tables_n); ++j) {
			struct ibv_exp_rwq_ind_table *ind_table =
				(*ind_tables)[j];

			if (ind_table == NULL)
				continue;
			claim_zero(ibv_exp_destroy_rwq_ind_table(ind_table));
		}
		rte_free(ind_tables);
	}
	return err;
}

/**
 * Clean up RX hash queues and indirection table.
 *
 * @param priv
 *   Pointer to private structure.
 */
static void
priv_destroy_hash_rxqs(struct priv *priv)
{
	unsigned int i;

	DEBUG("destroying %u RX hash queues", priv->hash_rxqs_n);
	if (priv->hash_rxqs_n == 0) {
		assert(priv->hash_rxqs == NULL);
		assert(priv->ind_tables == NULL);
		return;
	}
	for (i = 0; (i != priv->hash_rxqs_n); ++i) {
		struct hash_rxq *hash_rxq = &(*priv->hash_rxqs)[i];
		unsigned int j, k;

		assert(hash_rxq->priv == priv);
		assert(hash_rxq->qp != NULL);
		/* Also check that there are no remaining flows. */
		assert(hash_rxq->allmulti_flow == NULL);
		assert(hash_rxq->promisc_flow == NULL);
		for (j = 0; (j != elemof(hash_rxq->mac_flow)); ++j)
			for (k = 0; (k != elemof(hash_rxq->mac_flow[j])); ++k)
				assert(hash_rxq->mac_flow[j][k] == NULL);
		claim_zero(ibv_destroy_qp(hash_rxq->qp));
	}
	priv->hash_rxqs_n = 0;
	rte_free(priv->hash_rxqs);
	priv->hash_rxqs = NULL;
	for (i = 0; (i != priv->ind_tables_n); ++i) {
		struct ibv_exp_rwq_ind_table *ind_table =
			(*priv->ind_tables)[i];

		assert(ind_table != NULL);
		claim_zero(ibv_exp_destroy_rwq_ind_table(ind_table));
	}
	priv->ind_tables_n = 0;
	rte_free(priv->ind_tables);
	priv->ind_tables = NULL;
}

/* Device configuration. */

static int
rxq_setup(struct rte_eth_dev *dev, struct rxq *rxq, uint16_t desc,
	  unsigned int socket, const struct rte_eth_rxconf *conf,
	  struct rte_mempool *mp);

static void
rxq_cleanup(struct rxq *rxq);

/**
 * Ethernet device configuration.
 *
 * Prepare the driver for a given number of TX and RX queues.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 *
 * @return
 *   0 on success, errno value on failure.
 */
static int
dev_configure(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int rxqs_n = dev->data->nb_rx_queues;
	unsigned int txqs_n = dev->data->nb_tx_queues;
	unsigned int i;

	priv->rxqs = (void *)dev->data->rx_queues;
	priv->txqs = (void *)dev->data->tx_queues;
	if (txqs_n != priv->txqs_n) {
		INFO("%p: TX queues number update: %u -> %u",
		     (void *)dev, priv->txqs_n, txqs_n);
		priv->txqs_n = txqs_n;
	}
	if (rxqs_n == priv->rxqs_n)
		return 0;
	INFO("%p: RX queues number update: %u -> %u",
	     (void *)dev, priv->rxqs_n, rxqs_n);
	/* Fail if at least one RX queue is still allocated. */
	for (i = 0; (i != priv->rxqs_n); ++i)
		if ((*priv->rxqs)[i] != NULL)
			return EINVAL;
	priv->rxqs_n = rxqs_n;
	return 0;
}

/**
 * DPDK callback for Ethernet device configuration.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 *
 * @return
 *   0 on success, negative errno value on failure.
 */
static int
mlx5_dev_configure(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	int ret;

	priv_lock(priv);
	ret = dev_configure(dev);
	assert(ret >= 0);
	priv_unlock(priv);
	return -ret;
}

/* TX queues handling. */

/**
 * Allocate TX queue elements.
 *
 * @param txq
 *   Pointer to TX queue structure.
 * @param elts_n
 *   Number of elements to allocate.
 *
 * @return
 *   0 on success, errno value on failure.
 */
static int
txq_alloc_elts(struct txq *txq, unsigned int elts_n)
{
	unsigned int i;
	struct txq_elt (*elts)[elts_n] =
		rte_calloc_socket("TXQ", 1, sizeof(*elts), 0, txq->socket);
	linear_t (*elts_linear)[elts_n] =
		rte_calloc_socket("TXQ", 1, sizeof(*elts_linear), 0,
				  txq->socket);
	struct ibv_mr *mr_linear = NULL;
	int ret = 0;

	if ((elts == NULL) || (elts_linear == NULL)) {
		ERROR("%p: can't allocate packets array", (void *)txq);
		ret = ENOMEM;
		goto error;
	}
	mr_linear =
		ibv_reg_mr(txq->priv->pd, elts_linear, sizeof(*elts_linear),
			   (IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE));
	if (mr_linear == NULL) {
		ERROR("%p: unable to configure MR, ibv_reg_mr() failed",
		      (void *)txq);
		ret = EINVAL;
		goto error;
	}
	for (i = 0; (i != elts_n); ++i) {
		struct txq_elt *elt = &(*elts)[i];

		elt->buf = NULL;
	}
	DEBUG("%p: allocated and configured %u WRs", (void *)txq, elts_n);
	txq->elts_n = elts_n;
	txq->elts = elts;
	txq->elts_head = 0;
	txq->elts_tail = 0;
	txq->elts_comp = 0;
	/* Request send completion every MLX5_PMD_TX_PER_COMP_REQ packets or
	 * at least 4 times per ring. */
	txq->elts_comp_cd_init =
		((MLX5_PMD_TX_PER_COMP_REQ < (elts_n / 4)) ?
		 MLX5_PMD_TX_PER_COMP_REQ : (elts_n / 4));
	txq->elts_comp_cd = txq->elts_comp_cd_init;
	txq->elts_linear = elts_linear;
	txq->mr_linear = mr_linear;
	assert(ret == 0);
	return 0;
error:
	if (mr_linear != NULL)
		claim_zero(ibv_dereg_mr(mr_linear));

	rte_free(elts_linear);
	rte_free(elts);

	DEBUG("%p: failed, freed everything", (void *)txq);
	assert(ret > 0);
	return ret;
}

/**
 * Free TX queue elements.
 *
 * @param txq
 *   Pointer to TX queue structure.
 */
static void
txq_free_elts(struct txq *txq)
{
	unsigned int i;
	unsigned int elts_n = txq->elts_n;
	struct txq_elt (*elts)[elts_n] = txq->elts;
	linear_t (*elts_linear)[elts_n] = txq->elts_linear;
	struct ibv_mr *mr_linear = txq->mr_linear;

	DEBUG("%p: freeing WRs", (void *)txq);
	txq->elts_n = 0;
	txq->elts = NULL;
	txq->elts_linear = NULL;
	txq->mr_linear = NULL;
	if (mr_linear != NULL)
		claim_zero(ibv_dereg_mr(mr_linear));

	rte_free(elts_linear);
	if (elts == NULL)
		return;
	for (i = 0; (i != elemof(*elts)); ++i) {
		struct txq_elt *elt = &(*elts)[i];

		if (elt->buf == NULL)
			continue;
		rte_pktmbuf_free(elt->buf);
	}
	rte_free(elts);
}


/**
 * Clean up a TX queue.
 *
 * Destroy objects, free allocated memory and reset the structure for reuse.
 *
 * @param txq
 *   Pointer to TX queue structure.
 */
static void
txq_cleanup(struct txq *txq)
{
	struct ibv_exp_release_intf_params params;
	size_t i;

	DEBUG("cleaning up %p", (void *)txq);
	txq_free_elts(txq);
	if (txq->if_qp != NULL) {
		assert(txq->priv != NULL);
		assert(txq->priv->ctx != NULL);
		assert(txq->qp != NULL);
		params = (struct ibv_exp_release_intf_params){
			.comp_mask = 0,
		};
		claim_zero(ibv_exp_release_intf(txq->priv->ctx,
						txq->if_qp,
						&params));
	}
	if (txq->if_cq != NULL) {
		assert(txq->priv != NULL);
		assert(txq->priv->ctx != NULL);
		assert(txq->cq != NULL);
		params = (struct ibv_exp_release_intf_params){
			.comp_mask = 0,
		};
		claim_zero(ibv_exp_release_intf(txq->priv->ctx,
						txq->if_cq,
						&params));
	}
	if (txq->qp != NULL)
		claim_zero(ibv_destroy_qp(txq->qp));
	if (txq->cq != NULL)
		claim_zero(ibv_destroy_cq(txq->cq));
	if (txq->rd != NULL) {
		struct ibv_exp_destroy_res_domain_attr attr = {
			.comp_mask = 0,
		};

		assert(txq->priv != NULL);
		assert(txq->priv->ctx != NULL);
		claim_zero(ibv_exp_destroy_res_domain(txq->priv->ctx,
						      txq->rd,
						      &attr));
	}
	for (i = 0; (i != elemof(txq->mp2mr)); ++i) {
		if (txq->mp2mr[i].mp == NULL)
			break;
		assert(txq->mp2mr[i].mr != NULL);
		claim_zero(ibv_dereg_mr(txq->mp2mr[i].mr));
	}
	memset(txq, 0, sizeof(*txq));
}

/**
 * Manage TX completions.
 *
 * When sending a burst, mlx5_tx_burst() posts several WRs.
 * To improve performance, a completion event is only required once every
 * MLX5_PMD_TX_PER_COMP_REQ sends. Doing so discards completion information
 * for other WRs, but this information would not be used anyway.
 *
 * @param txq
 *   Pointer to TX queue structure.
 *
 * @return
 *   0 on success, -1 on failure.
 */
static int
txq_complete(struct txq *txq)
{
	unsigned int elts_comp = txq->elts_comp;
	unsigned int elts_tail = txq->elts_tail;
	const unsigned int elts_n = txq->elts_n;
	int wcs_n;

	if (unlikely(elts_comp == 0))
		return 0;
#ifdef DEBUG_SEND
	DEBUG("%p: processing %u work requests completions",
	      (void *)txq, elts_comp);
#endif
	wcs_n = txq->if_cq->poll_cnt(txq->cq, elts_comp);
	if (unlikely(wcs_n == 0))
		return 0;
	if (unlikely(wcs_n < 0)) {
		DEBUG("%p: ibv_poll_cq() failed (wcs_n=%d)",
		      (void *)txq, wcs_n);
		return -1;
	}
	elts_comp -= wcs_n;
	assert(elts_comp <= txq->elts_comp);
	/*
	 * Assume WC status is successful as nothing can be done about it
	 * anyway.
	 */
	elts_tail += wcs_n * txq->elts_comp_cd_init;
	if (elts_tail >= elts_n)
		elts_tail -= elts_n;
	txq->elts_tail = elts_tail;
	txq->elts_comp = elts_comp;
	return 0;
}

/**
 * Get Memory Region (MR) <-> Memory Pool (MP) association from txq->mp2mr[].
 * Add MP to txq->mp2mr[] if it's not registered yet. If mp2mr[] is full,
 * remove an entry first.
 *
 * @param txq
 *   Pointer to TX queue structure.
 * @param[in] mp
 *   Memory Pool for which a Memory Region lkey must be returned.
 *
 * @return
 *   mr->lkey on success, (uint32_t)-1 on failure.
 */
static uint32_t
txq_mp2mr(struct txq *txq, struct rte_mempool *mp)
{
	unsigned int i;
	struct ibv_mr *mr;

	for (i = 0; (i != elemof(txq->mp2mr)); ++i) {
		if (unlikely(txq->mp2mr[i].mp == NULL)) {
			/* Unknown MP, add a new MR for it. */
			break;
		}
		if (txq->mp2mr[i].mp == mp) {
			assert(txq->mp2mr[i].lkey != (uint32_t)-1);
			assert(txq->mp2mr[i].mr->lkey == txq->mp2mr[i].lkey);
			return txq->mp2mr[i].lkey;
		}
	}
	/* Add a new entry, register MR first. */
	DEBUG("%p: discovered new memory pool %p", (void *)txq, (void *)mp);
	mr = ibv_reg_mr(txq->priv->pd,
			(void *)mp->elt_va_start,
			(mp->elt_va_end - mp->elt_va_start),
			(IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE));
	if (unlikely(mr == NULL)) {
		DEBUG("%p: unable to configure MR, ibv_reg_mr() failed.",
		      (void *)txq);
		return (uint32_t)-1;
	}
	if (unlikely(i == elemof(txq->mp2mr))) {
		/* Table is full, remove oldest entry. */
		DEBUG("%p: MR <-> MP table full, dropping oldest entry.",
		      (void *)txq);
		--i;
		claim_zero(ibv_dereg_mr(txq->mp2mr[i].mr));
		memmove(&txq->mp2mr[0], &txq->mp2mr[1],
			(sizeof(txq->mp2mr) - sizeof(txq->mp2mr[0])));
	}
	/* Store the new entry. */
	txq->mp2mr[i].mp = mp;
	txq->mp2mr[i].mr = mr;
	txq->mp2mr[i].lkey = mr->lkey;
	DEBUG("%p: new MR lkey for MP %p: 0x%08" PRIu32,
	      (void *)txq, (void *)mp, txq->mp2mr[i].lkey);
	return txq->mp2mr[i].lkey;
}

#if MLX5_PMD_SGE_WR_N > 1

/**
 * Copy scattered mbuf contents to a single linear buffer.
 *
 * @param[out] linear
 *   Linear output buffer.
 * @param[in] buf
 *   Scattered input buffer.
 *
 * @return
 *   Number of bytes copied to the output buffer or 0 if not large enough.
 */
static unsigned int
linearize_mbuf(linear_t *linear, struct rte_mbuf *buf)
{
	unsigned int size = 0;
	unsigned int offset;

	do {
		unsigned int len = DATA_LEN(buf);

		offset = size;
		size += len;
		if (unlikely(size > sizeof(*linear)))
			return 0;
		memcpy(&(*linear)[offset],
		       rte_pktmbuf_mtod(buf, uint8_t *),
		       len);
		buf = NEXT(buf);
	} while (buf != NULL);
	return size;
}

/**
 * Handle scattered buffers for mlx5_tx_burst().
 *
 * @param txq
 *   TX queue structure.
 * @param segs
 *   Number of segments in buf.
 * @param elt
 *   TX queue element to fill.
 * @param[in] buf
 *   Buffer to process.
 * @param elts_head
 *   Index of the linear buffer to use if necessary (normally txq->elts_head).
 * @param[out] sges
 *   Array filled with SGEs on success.
 *
 * @return
 *   A structure containing the processed packet size in bytes and the
 *   number of SGEs. Both fields are set to (unsigned int)-1 in case of
 *   failure.
 */
static struct tx_burst_sg_ret {
	unsigned int length;
	unsigned int num;
}
tx_burst_sg(struct txq *txq, unsigned int segs, struct txq_elt *elt,
	    struct rte_mbuf *buf, unsigned int elts_head,
	    struct ibv_sge (*sges)[MLX5_PMD_SGE_WR_N])
{
	unsigned int sent_size = 0;
	unsigned int j;
	int linearize = 0;

	/* When there are too many segments, extra segments are
	 * linearized in the last SGE. */
	if (unlikely(segs > elemof(*sges))) {
		segs = (elemof(*sges) - 1);
		linearize = 1;
	}
	/* Update element. */
	elt->buf = buf;
	/* Register segments as SGEs. */
	for (j = 0; (j != segs); ++j) {
		struct ibv_sge *sge = &(*sges)[j];
		uint32_t lkey;

		/* Retrieve Memory Region key for this memory pool. */
		lkey = txq_mp2mr(txq, buf->pool);
		if (unlikely(lkey == (uint32_t)-1)) {
			/* MR does not exist. */
			DEBUG("%p: unable to get MP <-> MR association",
			      (void *)txq);
			/* Clean up TX element. */
			elt->buf = NULL;
			goto stop;
		}
		/* Update SGE. */
		sge->addr = rte_pktmbuf_mtod(buf, uintptr_t);
		if (txq->priv->vf)
			rte_prefetch0((volatile void *)
				      (uintptr_t)sge->addr);
		sge->length = DATA_LEN(buf);
		sge->lkey = lkey;
		sent_size += sge->length;
		buf = NEXT(buf);
	}
	/* If buf is not NULL here and is not going to be linearized,
	 * nb_segs is not valid. */
	assert(j == segs);
	assert((buf == NULL) || (linearize));
	/* Linearize extra segments. */
	if (linearize) {
		struct ibv_sge *sge = &(*sges)[segs];
		linear_t *linear = &(*txq->elts_linear)[elts_head];
		unsigned int size = linearize_mbuf(linear, buf);

		assert(segs == (elemof(*sges) - 1));
		if (size == 0) {
			/* Invalid packet. */
			DEBUG("%p: packet too large to be linearized.",
			      (void *)txq);
			/* Clean up TX element. */
			elt->buf = NULL;
			goto stop;
		}
		/* If MLX5_PMD_SGE_WR_N is 1, free mbuf immediately. */
		if (elemof(*sges) == 1) {
			do {
				struct rte_mbuf *next = NEXT(buf);

				rte_pktmbuf_free_seg(buf);
				buf = next;
			} while (buf != NULL);
			elt->buf = NULL;
		}
		/* Update SGE. */
		sge->addr = (uintptr_t)&(*linear)[0];
		sge->length = size;
		sge->lkey = txq->mr_linear->lkey;
		sent_size += size;
	}
	return (struct tx_burst_sg_ret){
		.length = sent_size,
		.num = segs,
	};
stop:
	return (struct tx_burst_sg_ret){
		.length = -1,
		.num = -1,
	};
}

#endif /* MLX5_PMD_SGE_WR_N > 1 */

/**
 * DPDK callback for TX.
 *
 * @param dpdk_txq
 *   Generic pointer to TX queue structure.
 * @param[in] pkts
 *   Packets to transmit.
 * @param pkts_n
 *   Number of packets in array.
 *
 * @return
 *   Number of packets successfully transmitted (<= pkts_n).
 */
static uint16_t
mlx5_tx_burst(void *dpdk_txq, struct rte_mbuf **pkts, uint16_t pkts_n)
{
	struct txq *txq = (struct txq *)dpdk_txq;
	unsigned int elts_head = txq->elts_head;
	const unsigned int elts_tail = txq->elts_tail;
	const unsigned int elts_n = txq->elts_n;
	unsigned int elts_comp_cd = txq->elts_comp_cd;
	unsigned int elts_comp = 0;
	unsigned int i;
	unsigned int max;
	int err;

	assert(elts_comp_cd != 0);
	txq_complete(txq);
	max = (elts_n - (elts_head - elts_tail));
	if (max > elts_n)
		max -= elts_n;
	assert(max >= 1);
	assert(max <= elts_n);
	/* Always leave one free entry in the ring. */
	--max;
	if (max == 0)
		return 0;
	if (max > pkts_n)
		max = pkts_n;
	for (i = 0; (i != max); ++i) {
		struct rte_mbuf *buf = pkts[i];
		unsigned int elts_head_next =
			(((elts_head + 1) == elts_n) ? 0 : elts_head + 1);
		struct txq_elt *elt_next = &(*txq->elts)[elts_head_next];
		struct txq_elt *elt = &(*txq->elts)[elts_head];
		unsigned int segs = NB_SEGS(buf);
#ifdef MLX5_PMD_SOFT_COUNTERS
		unsigned int sent_size = 0;
#endif
		uint32_t send_flags = 0;

		/* Clean up old buffer. */
		if (likely(elt->buf != NULL)) {
			struct rte_mbuf *tmp = elt->buf;

			/* Faster than rte_pktmbuf_free(). */
			do {
				struct rte_mbuf *next = NEXT(tmp);

				rte_pktmbuf_free_seg(tmp);
				tmp = next;
			} while (tmp != NULL);
		}
		/* Request TX completion. */
		if (unlikely(--elts_comp_cd == 0)) {
			elts_comp_cd = txq->elts_comp_cd_init;
			++elts_comp;
			send_flags |= IBV_EXP_QP_BURST_SIGNALED;
		}
		/* Should we enable HW CKSUM offload */
		if (buf->ol_flags &
		    (PKT_TX_IP_CKSUM | PKT_TX_TCP_CKSUM | PKT_TX_UDP_CKSUM)) {
			send_flags |= IBV_EXP_QP_BURST_IP_CSUM;
			/* HW does not support checksum offloads at arbitrary
			 * offsets but automatically recognizes the packet
			 * type. For inner L3/L4 checksums, only VXLAN (UDP)
			 * tunnels are currently supported. */
#ifdef RTE_NEXT_ABI
			if (RTE_ETH_IS_TUNNEL_PKT(buf->packet_type))
#else
			/* FIXME: since PKT_TX_UDP_TUNNEL_PKT has been removed,
			 * the outer packet type is unknown. All we know is
			 * that the L2 header is of unusual length (not
			 * ETHER_HDR_LEN with or without 802.1Q header). */
			if ((buf->l2_len != ETHER_HDR_LEN) &&
			    (buf->l2_len != (ETHER_HDR_LEN + 4)))
#endif
				send_flags |= IBV_EXP_QP_BURST_TUNNEL;
		}
		if (likely(segs == 1)) {
			uintptr_t addr;
			uint32_t length;
			uint32_t lkey;

			/* Retrieve buffer information. */
			addr = rte_pktmbuf_mtod(buf, uintptr_t);
			length = DATA_LEN(buf);
			/* Retrieve Memory Region key for this memory pool. */
			lkey = txq_mp2mr(txq, buf->pool);
			if (unlikely(lkey == (uint32_t)-1)) {
				/* MR does not exist. */
				DEBUG("%p: unable to get MP <-> MR"
				      " association", (void *)txq);
				/* Clean up TX element. */
				elt->buf = NULL;
				goto stop;
			}
			/* Update element. */
			elt->buf = buf;
			if (txq->priv->vf)
				rte_prefetch0((volatile void *)
					      (uintptr_t)addr);
			RTE_MBUF_PREFETCH_TO_FREE(elt_next->buf);
			/* Put packet into send queue. */
#if MLX5_PMD_MAX_INLINE > 0
			if (length <= txq->max_inline)
				err = txq->if_qp->send_pending_inline
					(txq->qp,
					 (void *)addr,
					 length,
					 send_flags);
			else
#endif
				err = txq->if_qp->send_pending
					(txq->qp,
					 addr,
					 length,
					 lkey,
					 send_flags);
			if (unlikely(err))
				goto stop;
#ifdef MLX5_PMD_SOFT_COUNTERS
			sent_size += length;
#endif
		} else {
#if MLX5_PMD_SGE_WR_N > 1
			struct ibv_sge sges[MLX5_PMD_SGE_WR_N];
			struct tx_burst_sg_ret ret;

			ret = tx_burst_sg(txq, segs, elt, buf, elts_head,
					  &sges);
			if (ret.length == (unsigned int)-1)
				goto stop;
			RTE_MBUF_PREFETCH_TO_FREE(elt_next->buf);
			/* Put SG list into send queue. */
			err = txq->if_qp->send_pending_sg_list
				(txq->qp,
				 sges,
				 ret.num,
				 send_flags);
			if (unlikely(err))
				goto stop;
#ifdef MLX5_PMD_SOFT_COUNTERS
			sent_size += ret.length;
#endif
#else /* MLX5_PMD_SGE_WR_N > 1 */
			DEBUG("%p: TX scattered buffers support not"
			      " compiled in", (void *)txq);
			goto stop;
#endif /* MLX5_PMD_SGE_WR_N > 1 */
		}
		elts_head = elts_head_next;
#ifdef MLX5_PMD_SOFT_COUNTERS
		/* Increment sent bytes counter. */
		txq->stats.obytes += sent_size;
#endif
	}
stop:
	/* Take a shortcut if nothing must be sent. */
	if (unlikely(i == 0))
		return 0;
#ifdef MLX5_PMD_SOFT_COUNTERS
	/* Increment sent packets counter. */
	txq->stats.opackets += i;
#endif
	/* Ring QP doorbell. */
	err = txq->if_qp->send_flush(txq->qp);
	if (unlikely(err)) {
		/* A nonzero value is not supposed to be returned.
		 * Nothing can be done about it. */
		DEBUG("%p: send_flush() failed with error %d",
		      (void *)txq, err);
	}
	txq->elts_head = elts_head;
	txq->elts_comp += elts_comp;
	txq->elts_comp_cd = elts_comp_cd;
	return i;
}

/**
 * Configure a TX queue.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param txq
 *   Pointer to TX queue structure.
 * @param desc
 *   Number of descriptors to configure in queue.
 * @param socket
 *   NUMA socket on which memory must be allocated.
 * @param[in] conf
 *   Thresholds parameters.
 *
 * @return
 *   0 on success, errno value on failure.
 */
static int
txq_setup(struct rte_eth_dev *dev, struct txq *txq, uint16_t desc,
	  unsigned int socket, const struct rte_eth_txconf *conf)
{
	struct priv *priv = dev->data->dev_private;
	struct txq tmpl = {
		.priv = priv,
		.socket = socket
	};
	union {
		struct ibv_exp_query_intf_params params;
		struct ibv_exp_qp_init_attr init;
		struct ibv_exp_res_domain_init_attr rd;
		struct ibv_exp_cq_init_attr cq;
		struct ibv_exp_qp_attr mod;
	} attr;
	enum ibv_exp_query_intf_status status;
	int ret = 0;

	(void)conf; /* Thresholds configuration (ignored). */
	if ((desc == 0) || (desc % MLX5_PMD_SGE_WR_N)) {
		ERROR("%p: invalid number of TX descriptors (must be a"
		      " multiple of %d)", (void *)dev, MLX5_PMD_SGE_WR_N);
		return EINVAL;
	}
	desc /= MLX5_PMD_SGE_WR_N;
	/* MRs will be registered in mp2mr[] later. */
	attr.rd = (struct ibv_exp_res_domain_init_attr){
		.comp_mask = (IBV_EXP_RES_DOMAIN_THREAD_MODEL |
			      IBV_EXP_RES_DOMAIN_MSG_MODEL),
		.thread_model = IBV_EXP_THREAD_SINGLE,
		.msg_model = IBV_EXP_MSG_HIGH_BW,
	};
	tmpl.rd = ibv_exp_create_res_domain(priv->ctx, &attr.rd);
	if (tmpl.rd == NULL) {
		ret = ENOMEM;
		ERROR("%p: RD creation failure: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	attr.cq = (struct ibv_exp_cq_init_attr){
		.comp_mask = IBV_EXP_CQ_INIT_ATTR_RES_DOMAIN,
		.res_domain = tmpl.rd,
	};
	tmpl.cq = ibv_exp_create_cq(priv->ctx, desc, NULL, NULL, 0, &attr.cq);
	if (tmpl.cq == NULL) {
		ret = ENOMEM;
		ERROR("%p: CQ creation failure: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	DEBUG("priv->device_attr.max_qp_wr is %d",
	      priv->device_attr.max_qp_wr);
	DEBUG("priv->device_attr.max_sge is %d",
	      priv->device_attr.max_sge);
	attr.init = (struct ibv_exp_qp_init_attr){
		/* CQ to be associated with the send queue. */
		.send_cq = tmpl.cq,
		/* CQ to be associated with the receive queue. */
		.recv_cq = tmpl.cq,
		.cap = {
			/* Max number of outstanding WRs. */
			.max_send_wr = ((priv->device_attr.max_qp_wr < desc) ?
					priv->device_attr.max_qp_wr :
					desc),
			/* Max number of scatter/gather elements in a WR. */
			.max_send_sge = ((priv->device_attr.max_sge <
					  MLX5_PMD_SGE_WR_N) ?
					 priv->device_attr.max_sge :
					 MLX5_PMD_SGE_WR_N),
#if MLX5_PMD_MAX_INLINE > 0
			.max_inline_data = MLX5_PMD_MAX_INLINE,
#endif
		},
		.qp_type = IBV_QPT_RAW_PACKET,
		/* Do *NOT* enable this, completions events are managed per
		 * TX burst. */
		.sq_sig_all = 0,
		.pd = priv->pd,
		.res_domain = tmpl.rd,
		.comp_mask = (IBV_EXP_QP_INIT_ATTR_PD |
			      IBV_EXP_QP_INIT_ATTR_RES_DOMAIN),
	};
	tmpl.qp = ibv_exp_create_qp(priv->ctx, &attr.init);
	if (tmpl.qp == NULL) {
		ret = (errno ? errno : EINVAL);
		ERROR("%p: QP creation failure: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
#if MLX5_PMD_MAX_INLINE > 0
	/* ibv_create_qp() updates this value. */
	tmpl.max_inline = attr.init.cap.max_inline_data;
#endif
	attr.mod = (struct ibv_exp_qp_attr){
		/* Move the QP to this state. */
		.qp_state = IBV_QPS_INIT,
		/* Primary port number. */
		.port_num = priv->port
	};
	ret = ibv_exp_modify_qp(tmpl.qp, &attr.mod,
				(IBV_EXP_QP_STATE | IBV_EXP_QP_PORT));
	if (ret) {
		ERROR("%p: QP state to IBV_QPS_INIT failed: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	ret = txq_alloc_elts(&tmpl, desc);
	if (ret) {
		ERROR("%p: TXQ allocation failed: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	attr.mod = (struct ibv_exp_qp_attr){
		.qp_state = IBV_QPS_RTR
	};
	ret = ibv_exp_modify_qp(tmpl.qp, &attr.mod, IBV_EXP_QP_STATE);
	if (ret) {
		ERROR("%p: QP state to IBV_QPS_RTR failed: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	attr.mod.qp_state = IBV_QPS_RTS;
	ret = ibv_exp_modify_qp(tmpl.qp, &attr.mod, IBV_EXP_QP_STATE);
	if (ret) {
		ERROR("%p: QP state to IBV_QPS_RTS failed: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	attr.params = (struct ibv_exp_query_intf_params){
		.intf_scope = IBV_EXP_INTF_GLOBAL,
		.intf = IBV_EXP_INTF_CQ,
		.obj = tmpl.cq,
	};
	tmpl.if_cq = ibv_exp_query_intf(priv->ctx, &attr.params, &status);
	if (tmpl.if_cq == NULL) {
		ret = EINVAL;
		ERROR("%p: CQ interface family query failed with status %d",
		      (void *)dev, status);
		goto error;
	}
	attr.params = (struct ibv_exp_query_intf_params){
		.intf_scope = IBV_EXP_INTF_GLOBAL,
		.intf = IBV_EXP_INTF_QP_BURST,
		.obj = tmpl.qp,
#ifdef HAVE_EXP_QP_BURST_CREATE_ENABLE_MULTI_PACKET_SEND_WR
		/* Multi packet send WR can only be used outside of VF. */
		.family_flags =
			(!priv->vf ?
			 IBV_EXP_QP_BURST_CREATE_ENABLE_MULTI_PACKET_SEND_WR :
			 0),
#endif
	};
	tmpl.if_qp = ibv_exp_query_intf(priv->ctx, &attr.params, &status);
	if (tmpl.if_qp == NULL) {
		ret = EINVAL;
		ERROR("%p: QP interface family query failed with status %d",
		      (void *)dev, status);
		goto error;
	}
	/* Clean up txq in case we're reinitializing it. */
	DEBUG("%p: cleaning-up old txq just in case", (void *)txq);
	txq_cleanup(txq);
	*txq = tmpl;
	DEBUG("%p: txq updated with %p", (void *)txq, (void *)&tmpl);
	assert(ret == 0);
	return 0;
error:
	txq_cleanup(&tmpl);
	assert(ret > 0);
	return ret;
}

/**
 * DPDK callback to configure a TX queue.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param idx
 *   TX queue index.
 * @param desc
 *   Number of descriptors to configure in queue.
 * @param socket
 *   NUMA socket on which memory must be allocated.
 * @param[in] conf
 *   Thresholds parameters.
 *
 * @return
 *   0 on success, negative errno value on failure.
 */
static int
mlx5_tx_queue_setup(struct rte_eth_dev *dev, uint16_t idx, uint16_t desc,
		    unsigned int socket, const struct rte_eth_txconf *conf)
{
	struct priv *priv = dev->data->dev_private;
	struct txq *txq = (*priv->txqs)[idx];
	int ret;

	priv_lock(priv);
	DEBUG("%p: configuring queue %u for %u descriptors",
	      (void *)dev, idx, desc);
	if (idx >= priv->txqs_n) {
		ERROR("%p: queue index out of range (%u >= %u)",
		      (void *)dev, idx, priv->txqs_n);
		priv_unlock(priv);
		return -EOVERFLOW;
	}
	if (txq != NULL) {
		DEBUG("%p: reusing already allocated queue index %u (%p)",
		      (void *)dev, idx, (void *)txq);
		if (priv->started) {
			priv_unlock(priv);
			return -EEXIST;
		}
		(*priv->txqs)[idx] = NULL;
		txq_cleanup(txq);
	} else {
		txq = rte_calloc_socket("TXQ", 1, sizeof(*txq), 0, socket);
		if (txq == NULL) {
			ERROR("%p: unable to allocate queue index %u",
			      (void *)dev, idx);
			priv_unlock(priv);
			return -ENOMEM;
		}
	}
	ret = txq_setup(dev, txq, desc, socket, conf);
	if (ret)
		rte_free(txq);
	else {
		txq->stats.idx = idx;
		DEBUG("%p: adding TX queue %p to list",
		      (void *)dev, (void *)txq);
		(*priv->txqs)[idx] = txq;
		/* Update send callback. */
		dev->tx_pkt_burst = mlx5_tx_burst;
	}
	priv_unlock(priv);
	return -ret;
}

/**
 * DPDK callback to release a TX queue.
 *
 * @param dpdk_txq
 *   Generic TX queue pointer.
 */
static void
mlx5_tx_queue_release(void *dpdk_txq)
{
	struct txq *txq = (struct txq *)dpdk_txq;
	struct priv *priv;
	unsigned int i;

	if (txq == NULL)
		return;
	priv = txq->priv;
	priv_lock(priv);
	for (i = 0; (i != priv->txqs_n); ++i)
		if ((*priv->txqs)[i] == txq) {
			DEBUG("%p: removing TX queue %p from list",
			      (void *)priv->dev, (void *)txq);
			(*priv->txqs)[i] = NULL;
			break;
		}
	txq_cleanup(txq);
	rte_free(txq);
	priv_unlock(priv);
}

/* RX queues handling. */

/**
 * Allocate RX queue elements with scattered packets support.
 *
 * @param rxq
 *   Pointer to RX queue structure.
 * @param elts_n
 *   Number of elements to allocate.
 * @param[in] pool
 *   If not NULL, fetch buffers from this array instead of allocating them
 *   with rte_pktmbuf_alloc().
 *
 * @return
 *   0 on success, errno value on failure.
 */
static int
rxq_alloc_elts_sp(struct rxq *rxq, unsigned int elts_n,
		  struct rte_mbuf **pool)
{
	unsigned int i;
	struct rxq_elt_sp (*elts)[elts_n] =
		rte_calloc_socket("RXQ elements", 1, sizeof(*elts), 0,
				  rxq->socket);
	int ret = 0;

	if (elts == NULL) {
		ERROR("%p: can't allocate packets array", (void *)rxq);
		ret = ENOMEM;
		goto error;
	}
	/* For each WR (packet). */
	for (i = 0; (i != elts_n); ++i) {
		unsigned int j;
		struct rxq_elt_sp *elt = &(*elts)[i];
		struct ibv_sge (*sges)[(elemof(elt->sges))] = &elt->sges;

		/* These two arrays must have the same size. */
		assert(elemof(elt->sges) == elemof(elt->bufs));
		/* For each SGE (segment). */
		for (j = 0; (j != elemof(elt->bufs)); ++j) {
			struct ibv_sge *sge = &(*sges)[j];
			struct rte_mbuf *buf;

			if (pool != NULL) {
				buf = *(pool++);
				assert(buf != NULL);
				rte_pktmbuf_reset(buf);
			} else
				buf = rte_pktmbuf_alloc(rxq->mp);
			if (buf == NULL) {
				assert(pool == NULL);
				ERROR("%p: empty mbuf pool", (void *)rxq);
				ret = ENOMEM;
				goto error;
			}
			elt->bufs[j] = buf;
			/* Headroom is reserved by rte_pktmbuf_alloc(). */
			assert(DATA_OFF(buf) == RTE_PKTMBUF_HEADROOM);
			/* Buffer is supposed to be empty. */
			assert(rte_pktmbuf_data_len(buf) == 0);
			assert(rte_pktmbuf_pkt_len(buf) == 0);
			/* sge->addr must be able to store a pointer. */
			assert(sizeof(sge->addr) >= sizeof(uintptr_t));
			if (j == 0) {
				/* The first SGE keeps its headroom. */
				sge->addr = rte_pktmbuf_mtod(buf, uintptr_t);
				sge->length = (buf->buf_len -
					       RTE_PKTMBUF_HEADROOM);
			} else {
				/* Subsequent SGEs lose theirs. */
				assert(DATA_OFF(buf) == RTE_PKTMBUF_HEADROOM);
				SET_DATA_OFF(buf, 0);
				sge->addr = (uintptr_t)buf->buf_addr;
				sge->length = buf->buf_len;
			}
			sge->lkey = rxq->mr->lkey;
			/* Redundant check for tailroom. */
			assert(sge->length == rte_pktmbuf_tailroom(buf));
		}
	}
	DEBUG("%p: allocated and configured %u WRs (%zu segments)",
	      (void *)rxq, elts_n, (elts_n * elemof((*elts)[0].sges)));
	rxq->elts_n = elts_n;
	rxq->elts_head = 0;
	rxq->elts.sp = elts;
	assert(ret == 0);
	return 0;
error:
	if (elts != NULL) {
		assert(pool == NULL);
		for (i = 0; (i != elemof(*elts)); ++i) {
			unsigned int j;
			struct rxq_elt_sp *elt = &(*elts)[i];

			for (j = 0; (j != elemof(elt->bufs)); ++j) {
				struct rte_mbuf *buf = elt->bufs[j];

				if (buf != NULL)
					rte_pktmbuf_free_seg(buf);
			}
		}
		rte_free(elts);
	}
	DEBUG("%p: failed, freed everything", (void *)rxq);
	assert(ret > 0);
	return ret;
}

/**
 * Free RX queue elements with scattered packets support.
 *
 * @param rxq
 *   Pointer to RX queue structure.
 */
static void
rxq_free_elts_sp(struct rxq *rxq)
{
	unsigned int i;
	unsigned int elts_n = rxq->elts_n;
	struct rxq_elt_sp (*elts)[elts_n] = rxq->elts.sp;

	DEBUG("%p: freeing WRs", (void *)rxq);
	rxq->elts_n = 0;
	rxq->elts.sp = NULL;
	if (elts == NULL)
		return;
	for (i = 0; (i != elemof(*elts)); ++i) {
		unsigned int j;
		struct rxq_elt_sp *elt = &(*elts)[i];

		for (j = 0; (j != elemof(elt->bufs)); ++j) {
			struct rte_mbuf *buf = elt->bufs[j];

			if (buf != NULL)
				rte_pktmbuf_free_seg(buf);
		}
	}
	rte_free(elts);
}

/**
 * Allocate RX queue elements.
 *
 * @param rxq
 *   Pointer to RX queue structure.
 * @param elts_n
 *   Number of elements to allocate.
 * @param[in] pool
 *   If not NULL, fetch buffers from this array instead of allocating them
 *   with rte_pktmbuf_alloc().
 *
 * @return
 *   0 on success, errno value on failure.
 */
static int
rxq_alloc_elts(struct rxq *rxq, unsigned int elts_n, struct rte_mbuf **pool)
{
	unsigned int i;
	struct rxq_elt (*elts)[elts_n] =
		rte_calloc_socket("RXQ elements", 1, sizeof(*elts), 0,
				  rxq->socket);
	int ret = 0;

	if (elts == NULL) {
		ERROR("%p: can't allocate packets array", (void *)rxq);
		ret = ENOMEM;
		goto error;
	}
	/* For each WR (packet). */
	for (i = 0; (i != elts_n); ++i) {
		struct rxq_elt *elt = &(*elts)[i];
		struct ibv_sge *sge = &(*elts)[i].sge;
		struct rte_mbuf *buf;

		if (pool != NULL) {
			buf = *(pool++);
			assert(buf != NULL);
			rte_pktmbuf_reset(buf);
		} else
			buf = rte_pktmbuf_alloc(rxq->mp);
		if (buf == NULL) {
			assert(pool == NULL);
			ERROR("%p: empty mbuf pool", (void *)rxq);
			ret = ENOMEM;
			goto error;
		}
		elt->buf = buf;
		/* Headroom is reserved by rte_pktmbuf_alloc(). */
		assert(DATA_OFF(buf) == RTE_PKTMBUF_HEADROOM);
		/* Buffer is supposed to be empty. */
		assert(rte_pktmbuf_data_len(buf) == 0);
		assert(rte_pktmbuf_pkt_len(buf) == 0);
		/* sge->addr must be able to store a pointer. */
		assert(sizeof(sge->addr) >= sizeof(uintptr_t));
		/* SGE keeps its headroom. */
		sge->addr = (uintptr_t)
			((uint8_t *)buf->buf_addr + RTE_PKTMBUF_HEADROOM);
		sge->length = (buf->buf_len - RTE_PKTMBUF_HEADROOM);
		sge->lkey = rxq->mr->lkey;
		/* Redundant check for tailroom. */
		assert(sge->length == rte_pktmbuf_tailroom(buf));
	}
	DEBUG("%p: allocated and configured %u single-segment WRs",
	      (void *)rxq, elts_n);
	rxq->elts_n = elts_n;
	rxq->elts_head = 0;
	rxq->elts.no_sp = elts;
	assert(ret == 0);
	return 0;
error:
	if (elts != NULL) {
		assert(pool == NULL);
		for (i = 0; (i != elemof(*elts)); ++i) {
			struct rxq_elt *elt = &(*elts)[i];
			struct rte_mbuf *buf = elt->buf;

			if (buf != NULL)
				rte_pktmbuf_free_seg(buf);
		}
		rte_free(elts);
	}
	DEBUG("%p: failed, freed everything", (void *)rxq);
	assert(ret > 0);
	return ret;
}

/**
 * Free RX queue elements.
 *
 * @param rxq
 *   Pointer to RX queue structure.
 */
static void
rxq_free_elts(struct rxq *rxq)
{
	unsigned int i;
	unsigned int elts_n = rxq->elts_n;
	struct rxq_elt (*elts)[elts_n] = rxq->elts.no_sp;

	DEBUG("%p: freeing WRs", (void *)rxq);
	rxq->elts_n = 0;
	rxq->elts.no_sp = NULL;
	if (elts == NULL)
		return;
	for (i = 0; (i != elemof(*elts)); ++i) {
		struct rxq_elt *elt = &(*elts)[i];
		struct rte_mbuf *buf = elt->buf;

		if (buf != NULL)
			rte_pktmbuf_free_seg(buf);
	}
	rte_free(elts);
}

/**
 * Delete flow steering rule.
 *
 * @param hash_rxq
 *   Pointer to RX hash queue structure.
 * @param mac_index
 *   MAC address index.
 * @param vlan_index
 *   VLAN index.
 */
static void
hash_rxq_del_flow(struct hash_rxq *hash_rxq, unsigned int mac_index,
		  unsigned int vlan_index)
{
#ifndef NDEBUG
	struct priv *priv = hash_rxq->priv;
	const uint8_t (*mac)[ETHER_ADDR_LEN] =
		(const uint8_t (*)[ETHER_ADDR_LEN])
		priv->mac[mac_index].addr_bytes;
#endif
	assert(hash_rxq->mac_flow[mac_index][vlan_index] != NULL);
	DEBUG("%p: removing MAC address %02x:%02x:%02x:%02x:%02x:%02x index %u"
	      " (VLAN ID %" PRIu16 ")",
	      (void *)hash_rxq,
	      (*mac)[0], (*mac)[1], (*mac)[2], (*mac)[3], (*mac)[4], (*mac)[5],
	      mac_index, priv->vlan_filter[vlan_index].id);
	claim_zero(ibv_exp_destroy_flow(hash_rxq->mac_flow
					[mac_index][vlan_index]));
	hash_rxq->mac_flow[mac_index][vlan_index] = NULL;
}

/**
 * Unregister a MAC address from a RX hash queue.
 *
 * @param hash_rxq
 *   Pointer to RX hash queue structure.
 * @param mac_index
 *   MAC address index.
 */
static void
hash_rxq_mac_addr_del(struct hash_rxq *hash_rxq, unsigned int mac_index)
{
	struct priv *priv = hash_rxq->priv;
	unsigned int i;
	unsigned int vlans = 0;

	assert(mac_index < elemof(priv->mac));
	if (!BITFIELD_ISSET(hash_rxq->mac_configured, mac_index))
		return;
	for (i = 0; (i != elemof(priv->vlan_filter)); ++i) {
		if (!priv->vlan_filter[i].enabled)
			continue;
		hash_rxq_del_flow(hash_rxq, mac_index, i);
		vlans++;
	}
	if (!vlans) {
		hash_rxq_del_flow(hash_rxq, mac_index, 0);
	}
	BITFIELD_RESET(hash_rxq->mac_configured, mac_index);
}

/**
 * Unregister all MAC addresses from a RX hash queue.
 *
 * @param hash_rxq
 *   Pointer to RX hash queue structure.
 */
static void
hash_rxq_mac_addrs_del(struct hash_rxq *hash_rxq)
{
	struct priv *priv = hash_rxq->priv;
	unsigned int i;

	for (i = 0; (i != elemof(priv->mac)); ++i)
		hash_rxq_mac_addr_del(hash_rxq, i);
}

/**
 * Unregister all MAC addresses from all RX hash queues.
 *
 * @param priv
 *   Pointer to private structure.
 */
static void
priv_mac_addrs_disable(struct priv *priv)
{
	unsigned int i;

	for (i = 0; (i != priv->hash_rxqs_n); ++i)
		hash_rxq_mac_addrs_del(&(*priv->hash_rxqs)[i]);
}

/**
 * Populate flow steering rule for a given hash RX queue type using
 * information from hash_rxq_init[]. Nothing is written to flow_attr when
 * flow_attr_size is not large enough, but the required size is still returned.
 *
 * @param[in] priv
 *   Pointer to private structure.
 * @param[out] flow_attr
 *   Pointer to flow attribute structure to fill. Note that the allocated
 *   area must be larger and large enough to hold all flow specifications.
 * @param flow_attr_size
 *   Entire size of flow_attr and trailing room for flow specifications.
 * @param type
 *   Requested Hash RX queue type.
 *
 * @return
 *   Total size of the flow attribute buffer. No errors are defined.
 */
static size_t
priv_populate_flow_attr(const struct priv *priv,
			struct ibv_exp_flow_attr *flow_attr,
			size_t flow_attr_size,
			enum hash_rxq_type type)
{
	size_t offset = sizeof(*flow_attr);
	const struct hash_rxq_init *init = &hash_rxq_init[type];

	assert((size_t)type < elemof(hash_rxq_init));
	do {
		offset += init->flow_spec.hdr.size;
		init = init->underlayer;
	} while (init != NULL);
	if (offset > flow_attr_size)
		return offset;
	flow_attr_size = offset;
	init = &hash_rxq_init[type];
	*flow_attr = (struct ibv_exp_flow_attr){
		.type = IBV_EXP_FLOW_ATTR_NORMAL,
		.priority = init->flow_priority,
		.num_of_specs = 0,
		.port = priv->port,
		.flags = 0,
	};
	do {
		offset -= init->flow_spec.hdr.size;
		memcpy((void *)((uintptr_t)flow_attr + offset),
		       &init->flow_spec,
		       init->flow_spec.hdr.size);
		++flow_attr->num_of_specs;
		init = init->underlayer;
	} while (init != NULL);
	return flow_attr_size;
}

/**
 * Add single flow steering rule.
 *
 * @param hash_rxq
 *   Pointer to RX hash queue structure.
 * @param mac_index
 *   MAC address index to register.
 * @param vlan_index
 *   VLAN index. Use -1 for a flow without VLAN.
 *
 * @return
 *   0 on success, errno value on failure.
 */
static int
hash_rxq_add_flow(struct hash_rxq *hash_rxq, unsigned int mac_index,
		  unsigned int vlan_index)
{
	struct ibv_exp_flow *flow;
	struct priv *priv = hash_rxq->priv;
	const uint8_t (*mac)[ETHER_ADDR_LEN] =
			(const uint8_t (*)[ETHER_ADDR_LEN])
			priv->mac[mac_index].addr_bytes;
	FLOW_ATTR_SPEC_ETH(data, priv_populate_flow_attr(priv, NULL, 0,
							 hash_rxq->type));
	struct ibv_exp_flow_attr *attr = &data->attr;
	struct ibv_exp_flow_spec_eth *spec = &data->spec;

	assert(mac_index < elemof(priv->mac));
	assert((vlan_index < elemof(priv->vlan_filter)) || (vlan_index == -1u));
	/*
	 * No padding must be inserted by the compiler between attr and spec.
	 * This layout is expected by libibverbs.
	 */
	assert(((uint8_t *)attr + sizeof(*attr)) == (uint8_t *)spec);
	priv_populate_flow_attr(priv, attr, sizeof(data), hash_rxq->type);
	/* The first specification must be Ethernet. */
	assert(spec->type == IBV_EXP_FLOW_SPEC_ETH);
	assert(spec->size == sizeof(*spec));
	*spec = (struct ibv_exp_flow_spec_eth){
		.type = IBV_EXP_FLOW_SPEC_ETH,
		.size = sizeof(*spec),
		.val = {
			.dst_mac = {
				(*mac)[0], (*mac)[1], (*mac)[2],
				(*mac)[3], (*mac)[4], (*mac)[5]
			},
			.vlan_tag = ((vlan_index != -1u) ?
				     htons(priv->vlan_filter[vlan_index].id) :
				     0),
		},
		.mask = {
			.dst_mac = "\xff\xff\xff\xff\xff\xff",
			.vlan_tag = ((vlan_index != -1u) ? htons(0xfff) : 0),
		}
	};
	DEBUG("%p: adding MAC address %02x:%02x:%02x:%02x:%02x:%02x index %u"
	      " (VLAN %s %" PRIu16 ")",
	      (void *)hash_rxq,
	      (*mac)[0], (*mac)[1], (*mac)[2], (*mac)[3], (*mac)[4], (*mac)[5],
	      mac_index,
	      ((vlan_index != -1u) ? "ID" : "index"),
	      ((vlan_index != -1u) ? priv->vlan_filter[vlan_index].id : -1u));
	/* Create related flow. */
	errno = 0;
	flow = ibv_exp_create_flow(hash_rxq->qp, attr);
	if (flow == NULL) {
		/* It's not clear whether errno is always set in this case. */
		ERROR("%p: flow configuration failed, errno=%d: %s",
		      (void *)hash_rxq, errno,
		      (errno ? strerror(errno) : "Unknown error"));
		if (errno)
			return errno;
		return EINVAL;
	}
	if (vlan_index == -1u)
		vlan_index = 0;
	assert(hash_rxq->mac_flow[mac_index][vlan_index] == NULL);
	hash_rxq->mac_flow[mac_index][vlan_index] = flow;
	return 0;
}

/**
 * Register a MAC address in a RX hash queue.
 *
 * @param hash_rxq
 *   Pointer to RX hash queue structure.
 * @param mac_index
 *   MAC address index to register.
 *
 * @return
 *   0 on success, errno value on failure.
 */
static int
hash_rxq_mac_addr_add(struct hash_rxq *hash_rxq, unsigned int mac_index)
{
	struct priv *priv = hash_rxq->priv;
	unsigned int i;
	unsigned int vlans = 0;
	int ret;

	assert(mac_index < elemof(priv->mac));
	if (BITFIELD_ISSET(hash_rxq->mac_configured, mac_index))
		hash_rxq_mac_addr_del(hash_rxq, mac_index);
	/* Fill VLAN specifications. */
	for (i = 0; (i != elemof(priv->vlan_filter)); ++i) {
		if (!priv->vlan_filter[i].enabled)
			continue;
		/* Create related flow. */
		ret = hash_rxq_add_flow(hash_rxq, mac_index, i);
		if (!ret) {
			vlans++;
			continue;
		}
		/* Failure, rollback. */
		while (i != 0)
			if (priv->vlan_filter[--i].enabled)
				hash_rxq_del_flow(hash_rxq, mac_index, i);
		assert(ret > 0);
		return ret;
	}
	/* In case there is no VLAN filter. */
	if (!vlans) {
		ret = hash_rxq_add_flow(hash_rxq, mac_index, -1);
		if (ret)
			return ret;
	}
	BITFIELD_SET(hash_rxq->mac_configured, mac_index);
	return 0;
}

/**
 * Register all MAC addresses in a RX hash queue.
 *
 * @param hash_rxq
 *   Pointer to RX queue structure.
 *
 * @return
 *   0 on success, errno value on failure.
 */
static int
hash_rxq_mac_addrs_add(struct hash_rxq *hash_rxq)
{
	struct priv *priv = hash_rxq->priv;
	unsigned int i;
	int ret;

	for (i = 0; (i != elemof(priv->mac)); ++i) {
		if (!BITFIELD_ISSET(priv->mac_configured, i))
			continue;
		ret = hash_rxq_mac_addr_add(hash_rxq, i);
		if (!ret)
			continue;
		/* Failure, rollback. */
		while (i != 0)
			hash_rxq_mac_addr_del(hash_rxq, --i);
		assert(ret > 0);
		return ret;
	}
	return 0;
}

/**
 * Register all MAC addresses in all RX hash queues.
 *
 * @param priv
 *   Pointer to private structure.
 *
 * @return
 *   0 on success, errno value on failure.
 */
static int
priv_mac_addrs_enable(struct priv *priv)
{
	unsigned int i;
	int ret;

	for (i = 0; (i != priv->hash_rxqs_n); ++i) {
		ret = hash_rxq_mac_addrs_add(&(*priv->hash_rxqs)[i]);
		if (!ret)
			continue;
		/* Failure, rollback. */
		while (i != 0)
			hash_rxq_mac_addrs_del(&(*priv->hash_rxqs)[--i]);
		assert(ret > 0);
		return ret;
	}
	return 0;
}

/**
 * Unregister a MAC address.
 *
 * This is done for each RX hash queue.
 *
 * @param priv
 *   Pointer to private structure.
 * @param mac_index
 *   MAC address index.
 */
static void
priv_mac_addr_del(struct priv *priv, unsigned int mac_index)
{
	unsigned int i;

	assert(mac_index < elemof(priv->mac));
	if (!BITFIELD_ISSET(priv->mac_configured, mac_index))
		return;
	for (i = 0; (i != priv->hash_rxqs_n); ++i)
		hash_rxq_mac_addr_del(&(*priv->hash_rxqs)[i], mac_index);
	BITFIELD_RESET(priv->mac_configured, mac_index);
}

/**
 * Register a MAC address.
 *
 * This is done for each RX hash queue.
 *
 * @param priv
 *   Pointer to private structure.
 * @param mac_index
 *   MAC address index to use.
 * @param mac
 *   MAC address to register.
 *
 * @return
 *   0 on success, errno value on failure.
 */
static int
priv_mac_addr_add(struct priv *priv, unsigned int mac_index,
		  const uint8_t (*mac)[ETHER_ADDR_LEN])
{
	unsigned int i;
	int ret;

	assert(mac_index < elemof(priv->mac));
	/* First, make sure this address isn't already configured. */
	for (i = 0; (i != elemof(priv->mac)); ++i) {
		/* Skip this index, it's going to be reconfigured. */
		if (i == mac_index)
			continue;
		if (!BITFIELD_ISSET(priv->mac_configured, i))
			continue;
		if (memcmp(priv->mac[i].addr_bytes, *mac, sizeof(*mac)))
			continue;
		/* Address already configured elsewhere, return with error. */
		return EADDRINUSE;
	}
	if (BITFIELD_ISSET(priv->mac_configured, mac_index))
		priv_mac_addr_del(priv, mac_index);
	priv->mac[mac_index] = (struct ether_addr){
		{
			(*mac)[0], (*mac)[1], (*mac)[2],
			(*mac)[3], (*mac)[4], (*mac)[5]
		}
	};
	/* If device isn't started, this is all we need to do. */
	if (!priv->started) {
#ifndef NDEBUG
		/* Verify that all RX hash queues have this index disabled. */
		for (i = 0; (i != priv->hash_rxqs_n); ++i) {
			assert(!BITFIELD_ISSET
			       ((*priv->hash_rxqs)[i].mac_configured,
				mac_index));
		}
#endif
		goto end;
	}
	for (i = 0; (i != priv->hash_rxqs_n); ++i) {
		ret = hash_rxq_mac_addr_add(&(*priv->hash_rxqs)[i], mac_index);
		if (!ret)
			continue;
		/* Failure, rollback. */
		while (i != 0)
			hash_rxq_mac_addr_del(&(*priv->hash_rxqs)[--i],
					      mac_index);
		return ret;
	}
end:
	BITFIELD_SET(priv->mac_configured, mac_index);
	return 0;
}

/**
 * Enable allmulti mode in a RX hash queue.
 *
 * @param hash_rxq
 *   Pointer to RX hash queue structure.
 *
 * @return
 *   0 on success, errno value on failure.
 */
static int
hash_rxq_allmulticast_enable(struct hash_rxq *hash_rxq)
{
	struct ibv_exp_flow *flow;
	struct ibv_exp_flow_attr attr = {
		.type = IBV_EXP_FLOW_ATTR_MC_DEFAULT,
		.num_of_specs = 0,
		.port = hash_rxq->priv->port,
		.flags = 0
	};

	DEBUG("%p: enabling allmulticast mode", (void *)hash_rxq);
	if (hash_rxq->allmulti_flow != NULL)
		return EBUSY;
	errno = 0;
	flow = ibv_exp_create_flow(hash_rxq->qp, &attr);
	if (flow == NULL) {
		/* It's not clear whether errno is always set in this case. */
		ERROR("%p: flow configuration failed, errno=%d: %s",
		      (void *)hash_rxq, errno,
		      (errno ? strerror(errno) : "Unknown error"));
		if (errno)
			return errno;
		return EINVAL;
	}
	hash_rxq->allmulti_flow = flow;
	DEBUG("%p: allmulticast mode enabled", (void *)hash_rxq);
	return 0;
}

static void hash_rxq_allmulticast_disable(struct hash_rxq *);

/**
 * Enable allmulti mode in all RX hash queues.
 *
 * @param priv
 *   Private structure.
 *
 * @return
 *   0 on success, errno value on failure.
 */
static int
priv_allmulticast_enable(struct priv *priv)
{
	unsigned int i;

	if (priv->allmulti)
		return 0;
	for (i = 0; (i != priv->hash_rxqs_n); ++i) {
		struct hash_rxq *hash_rxq = &(*priv->hash_rxqs)[i];
		int ret;

		ret = hash_rxq_allmulticast_enable(hash_rxq);
		if (!ret)
			continue;
		/* Failure, rollback. */
		while (i != 0) {
			hash_rxq = &(*priv->hash_rxqs)[--i];
			hash_rxq_allmulticast_disable(hash_rxq);
		}
		return ret;
	}
	priv->allmulti = 1;
	return 0;
}

/**
 * Disable allmulti mode in a RX hash queue.
 *
 * @param hash_rxq
 *   Pointer to RX hash queue structure.
 */
static void
hash_rxq_allmulticast_disable(struct hash_rxq *hash_rxq)
{
	DEBUG("%p: disabling allmulticast mode", (void *)hash_rxq);
	if (hash_rxq->allmulti_flow == NULL)
		return;
	claim_zero(ibv_exp_destroy_flow(hash_rxq->allmulti_flow));
	hash_rxq->allmulti_flow = NULL;
	DEBUG("%p: allmulticast mode disabled", (void *)hash_rxq);
}

/**
 * Disable allmulti mode in all RX hash queues.
 *
 * @param priv
 *   Private structure.
 */
static void
priv_allmulticast_disable(struct priv *priv)
{
	unsigned int i;

	if (!priv->allmulti)
		return;
	for (i = 0; (i != priv->hash_rxqs_n); ++i)
		hash_rxq_allmulticast_disable(&(*priv->hash_rxqs)[i]);
	priv->allmulti = 0;
}

/**
 * Enable promiscuous mode in a RX hash queue.
 *
 * @param hash_rxq
 *   Pointer to RX hash queue structure.
 *
 * @return
 *   0 on success, errno value on failure.
 */
static int
hash_rxq_promiscuous_enable(struct hash_rxq *hash_rxq)
{
	struct ibv_exp_flow *flow;
	struct priv *priv = hash_rxq->priv;
	FLOW_ATTR_SPEC_ETH(data, priv_populate_flow_attr(priv, NULL, 0,
							 hash_rxq->type));
	struct ibv_exp_flow_attr *attr = &data->attr;

	if (hash_rxq->priv->vf)
		return 0;
	DEBUG("%p: enabling promiscuous mode", (void *)hash_rxq);
	if (hash_rxq->promisc_flow != NULL)
		return EBUSY;
	/* Promiscuous flows only differ from normal flows by not filtering
	 * on specific MAC addresses. */
	priv_populate_flow_attr(priv, attr, sizeof(data), hash_rxq->type);
	errno = 0;
	flow = ibv_exp_create_flow(hash_rxq->qp, attr);
	if (flow == NULL) {
		/* It's not clear whether errno is always set in this case. */
		ERROR("%p: flow configuration failed, errno=%d: %s",
		      (void *)hash_rxq, errno,
		      (errno ? strerror(errno) : "Unknown error"));
		if (errno)
			return errno;
		return EINVAL;
	}
	hash_rxq->promisc_flow = flow;
	DEBUG("%p: promiscuous mode enabled", (void *)hash_rxq);
	return 0;
}

static void hash_rxq_promiscuous_disable(struct hash_rxq *);

/**
 * Enable promiscuous mode in all RX hash queues.
 *
 * @param priv
 *   Private structure.
 *
 * @return
 *   0 on success, errno value on failure.
 */
static int
priv_promiscuous_enable(struct priv *priv)
{
	unsigned int i;

	if (priv->promisc)
		return 0;
	for (i = 0; (i != priv->hash_rxqs_n); ++i) {
		struct hash_rxq *hash_rxq = &(*priv->hash_rxqs)[i];
		int ret;

		/* Remove normal MAC flows first. */
		hash_rxq_mac_addrs_del(hash_rxq);
		ret = hash_rxq_promiscuous_enable(hash_rxq);
		if (!ret)
			continue;
		/* Failure, rollback. */
		while (i != 0) {
			hash_rxq = &(*priv->hash_rxqs)[--i];
			hash_rxq_promiscuous_disable(hash_rxq);
			/* Restore MAC flows. */
			if (priv->started)
				hash_rxq_mac_addrs_add(hash_rxq);
		}
		return ret;
	}
	priv->promisc = 1;
	return 0;
}

/**
 * Disable promiscuous mode in a RX hash queue.
 *
 * @param hash_rxq
 *   Pointer to RX hash queue structure.
 */
static void
hash_rxq_promiscuous_disable(struct hash_rxq *hash_rxq)
{
	if (hash_rxq->priv->vf)
		return;
	DEBUG("%p: disabling promiscuous mode", (void *)hash_rxq);
	if (hash_rxq->promisc_flow == NULL)
		return;
	claim_zero(ibv_exp_destroy_flow(hash_rxq->promisc_flow));
	hash_rxq->promisc_flow = NULL;
	DEBUG("%p: promiscuous mode disabled", (void *)hash_rxq);
}

/**
 * Disable promiscuous mode in all RX hash queues.
 *
 * @param priv
 *   Private structure.
 */
static void
priv_promiscuous_disable(struct priv *priv)
{
	unsigned int i;

	if (!priv->promisc)
		return;
	for (i = 0; (i != priv->hash_rxqs_n); ++i) {
		struct hash_rxq *hash_rxq = &(*priv->hash_rxqs)[i];

		hash_rxq_promiscuous_disable(hash_rxq);
		/* Restore MAC flows. */
		if (priv->started)
			hash_rxq_mac_addrs_add(hash_rxq);
	}
	priv->promisc = 0;
}

/**
 * Clean up a RX queue.
 *
 * Destroy objects, free allocated memory and reset the structure for reuse.
 *
 * @param rxq
 *   Pointer to RX queue structure.
 */
static void
rxq_cleanup(struct rxq *rxq)
{
	struct ibv_exp_release_intf_params params;

	DEBUG("cleaning up %p", (void *)rxq);
	if (rxq->sp)
		rxq_free_elts_sp(rxq);
	else
		rxq_free_elts(rxq);
	if (rxq->if_wq != NULL) {
		assert(rxq->priv != NULL);
		assert(rxq->priv->ctx != NULL);
		assert(rxq->wq != NULL);
		params = (struct ibv_exp_release_intf_params){
			.comp_mask = 0,
		};
		claim_zero(ibv_exp_release_intf(rxq->priv->ctx,
						rxq->if_wq,
						&params));
	}
	if (rxq->if_cq != NULL) {
		assert(rxq->priv != NULL);
		assert(rxq->priv->ctx != NULL);
		assert(rxq->cq != NULL);
		params = (struct ibv_exp_release_intf_params){
			.comp_mask = 0,
		};
		claim_zero(ibv_exp_release_intf(rxq->priv->ctx,
						rxq->if_cq,
						&params));
	}
	if (rxq->wq != NULL)
		claim_zero(ibv_exp_destroy_wq(rxq->wq));
	if (rxq->cq != NULL)
		claim_zero(ibv_destroy_cq(rxq->cq));
	if (rxq->rd != NULL) {
		struct ibv_exp_destroy_res_domain_attr attr = {
			.comp_mask = 0,
		};

		assert(rxq->priv != NULL);
		assert(rxq->priv->ctx != NULL);
		claim_zero(ibv_exp_destroy_res_domain(rxq->priv->ctx,
						      rxq->rd,
						      &attr));
	}
	if (rxq->mr != NULL)
		claim_zero(ibv_dereg_mr(rxq->mr));
	memset(rxq, 0, sizeof(*rxq));
}

#ifdef RTE_NEXT_ABI
/**
 * Translate RX completion flags to packet type.
 *
 * @param flags
 *   RX completion flags returned by poll_length_flags().
 *
 * @return
 *   Packet type for struct rte_mbuf.
 */
static inline uint32_t
rxq_cq_to_pkt_type(uint32_t flags)
{
	uint32_t pkt_type;

	if (flags & IBV_EXP_CQ_RX_TUNNEL_PACKET)
		pkt_type =
			TRANSPOSE(flags,
			          IBV_EXP_CQ_RX_OUTER_IPV4_PACKET, RTE_PTYPE_L3_IPV4) |
			TRANSPOSE(flags,
			          IBV_EXP_CQ_RX_OUTER_IPV6_PACKET, RTE_PTYPE_L3_IPV6) |
			TRANSPOSE(flags,
			          IBV_EXP_CQ_RX_IPV4_PACKET, RTE_PTYPE_INNER_L3_IPV4) |
			TRANSPOSE(flags,
			          IBV_EXP_CQ_RX_IPV6_PACKET, RTE_PTYPE_INNER_L3_IPV6);
	else
		pkt_type =
			TRANSPOSE(flags,
			          IBV_EXP_CQ_RX_IPV4_PACKET, RTE_PTYPE_L3_IPV4) |
			TRANSPOSE(flags,
			          IBV_EXP_CQ_RX_IPV6_PACKET, RTE_PTYPE_L3_IPV6);
	return pkt_type;
}
#endif /* RTE_NEXT_ABI */

/**
 * Translate RX completion flags to offload flags.
 *
 * @param[in] rxq
 *   Pointer to RX queue structure.
 * @param flags
 *   RX completion flags returned by poll_length_flags().
 *
 * @return
 *   Offload flags (ol_flags) for struct rte_mbuf.
 */
static inline uint32_t
rxq_cq_to_ol_flags(const struct rxq *rxq, uint32_t flags)
{
	uint32_t ol_flags = 0;

#ifndef RTE_NEXT_ABI
	ol_flags =
		TRANSPOSE(flags, IBV_EXP_CQ_RX_IPV4_PACKET, PKT_RX_IPV4_HDR) |
		TRANSPOSE(flags, IBV_EXP_CQ_RX_IPV6_PACKET, PKT_RX_IPV6_HDR);
#endif
	if (rxq->csum)
		ol_flags |=
			TRANSPOSE(~flags,
				  IBV_EXP_CQ_RX_IP_CSUM_OK,
				  PKT_RX_IP_CKSUM_BAD) |
			TRANSPOSE(~flags,
				  IBV_EXP_CQ_RX_TCP_UDP_CSUM_OK,
				  PKT_RX_L4_CKSUM_BAD);
	/*
	 * PKT_RX_IP_CKSUM_BAD and PKT_RX_L4_CKSUM_BAD are used in place
	 * of PKT_RX_EIP_CKSUM_BAD because the latter is not functional
	 * (its value is 0).
	 */
	if ((flags & IBV_EXP_CQ_RX_TUNNEL_PACKET) && (rxq->csum_l2tun))
		ol_flags |=
#ifndef RTE_NEXT_ABI
			TRANSPOSE(flags,
				  IBV_EXP_CQ_RX_OUTER_IPV4_PACKET,
				  PKT_RX_TUNNEL_IPV4_HDR) |
			TRANSPOSE(flags,
				  IBV_EXP_CQ_RX_OUTER_IPV6_PACKET,
				  PKT_RX_TUNNEL_IPV6_HDR) |
#endif
			TRANSPOSE(~flags,
				  IBV_EXP_CQ_RX_OUTER_IP_CSUM_OK,
				  PKT_RX_IP_CKSUM_BAD) |
			TRANSPOSE(~flags,
				  IBV_EXP_CQ_RX_OUTER_TCP_UDP_CSUM_OK,
				  PKT_RX_L4_CKSUM_BAD);
	return ol_flags;
}

static uint16_t
mlx5_rx_burst(void *dpdk_rxq, struct rte_mbuf **pkts, uint16_t pkts_n);

/**
 * DPDK callback for RX with scattered packets support.
 *
 * @param dpdk_rxq
 *   Generic pointer to RX queue structure.
 * @param[out] pkts
 *   Array to store received packets.
 * @param pkts_n
 *   Maximum number of packets in array.
 *
 * @return
 *   Number of packets successfully received (<= pkts_n).
 */
static uint16_t
mlx5_rx_burst_sp(void *dpdk_rxq, struct rte_mbuf **pkts, uint16_t pkts_n)
{
	struct rxq *rxq = (struct rxq *)dpdk_rxq;
	struct rxq_elt_sp (*elts)[rxq->elts_n] = rxq->elts.sp;
	const unsigned int elts_n = rxq->elts_n;
	unsigned int elts_head = rxq->elts_head;
	unsigned int i;
	unsigned int pkts_ret = 0;
	int ret;

	if (unlikely(!rxq->sp))
		return mlx5_rx_burst(dpdk_rxq, pkts, pkts_n);
	if (unlikely(elts == NULL)) /* See RTE_DEV_CMD_SET_MTU. */
		return 0;
	for (i = 0; (i != pkts_n); ++i) {
		struct rxq_elt_sp *elt = &(*elts)[elts_head];
		unsigned int len;
		unsigned int pkt_buf_len;
		struct rte_mbuf *pkt_buf = NULL; /* Buffer returned in pkts. */
		struct rte_mbuf **pkt_buf_next = &pkt_buf;
		unsigned int seg_headroom = RTE_PKTMBUF_HEADROOM;
		unsigned int j = 0;
		uint32_t flags;

		/* Sanity checks. */
		assert(elts_head < rxq->elts_n);
		assert(rxq->elts_head < rxq->elts_n);
		ret = rxq->if_cq->poll_length_flags(rxq->cq, NULL, NULL,
						    &flags);
		if (unlikely(ret < 0)) {
			struct ibv_wc wc;
			int wcs_n;

			DEBUG("rxq=%p, poll_length() failed (ret=%d)",
			      (void *)rxq, ret);
			/* ibv_poll_cq() must be used in case of failure. */
			wcs_n = ibv_poll_cq(rxq->cq, 1, &wc);
			if (unlikely(wcs_n == 0))
				break;
			if (unlikely(wcs_n < 0)) {
				DEBUG("rxq=%p, ibv_poll_cq() failed (wcs_n=%d)",
				      (void *)rxq, wcs_n);
				break;
			}
			assert(wcs_n == 1);
			if (unlikely(wc.status != IBV_WC_SUCCESS)) {
				/* Whatever, just repost the offending WR. */
				DEBUG("rxq=%p, wr_id=%" PRIu64 ": bad work"
				      " completion status (%d): %s",
				      (void *)rxq, wc.wr_id, wc.status,
				      ibv_wc_status_str(wc.status));
#ifdef MLX5_PMD_SOFT_COUNTERS
				/* Increment dropped packets counter. */
				++rxq->stats.idropped;
#endif
				goto repost;
			}
			ret = wc.byte_len;
		}
		if (ret == 0)
			break;
		len = ret;
		pkt_buf_len = len;
		/*
		 * Replace spent segments with new ones, concatenate and
		 * return them as pkt_buf.
		 */
		while (1) {
			struct ibv_sge *sge = &elt->sges[j];
			struct rte_mbuf *seg = elt->bufs[j];
			struct rte_mbuf *rep;
			unsigned int seg_tailroom;

			assert(seg != NULL);
			/*
			 * Fetch initial bytes of packet descriptor into a
			 * cacheline while allocating rep.
			 */
			rte_prefetch0(seg);
			rep = __rte_mbuf_raw_alloc(rxq->mp);
			if (unlikely(rep == NULL)) {
				/*
				 * Unable to allocate a replacement mbuf,
				 * repost WR.
				 */
				DEBUG("rxq=%p: can't allocate a new mbuf",
				      (void *)rxq);
				if (pkt_buf != NULL) {
					*pkt_buf_next = NULL;
					rte_pktmbuf_free(pkt_buf);
				}
				/* Increase out of memory counters. */
				++rxq->stats.rx_nombuf;
				++rxq->priv->dev->data->rx_mbuf_alloc_failed;
				goto repost;
			}
#ifndef NDEBUG
			/* Poison user-modifiable fields in rep. */
			NEXT(rep) = (void *)((uintptr_t)-1);
			SET_DATA_OFF(rep, 0xdead);
			DATA_LEN(rep) = 0xd00d;
			PKT_LEN(rep) = 0xdeadd00d;
			NB_SEGS(rep) = 0x2a;
			PORT(rep) = 0x2a;
			rep->ol_flags = -1;
#endif
			assert(rep->buf_len == seg->buf_len);
			assert(rep->buf_len == rxq->mb_len);
			/* Reconfigure sge to use rep instead of seg. */
			assert(sge->lkey == rxq->mr->lkey);
			sge->addr = ((uintptr_t)rep->buf_addr + seg_headroom);
			elt->bufs[j] = rep;
			++j;
			/* Update pkt_buf if it's the first segment, or link
			 * seg to the previous one and update pkt_buf_next. */
			*pkt_buf_next = seg;
			pkt_buf_next = &NEXT(seg);
			/* Update seg information. */
			seg_tailroom = (seg->buf_len - seg_headroom);
			assert(sge->length == seg_tailroom);
			SET_DATA_OFF(seg, seg_headroom);
			if (likely(len <= seg_tailroom)) {
				/* Last segment. */
				DATA_LEN(seg) = len;
				PKT_LEN(seg) = len;
				/* Sanity check. */
				assert(rte_pktmbuf_headroom(seg) ==
				       seg_headroom);
				assert(rte_pktmbuf_tailroom(seg) ==
				       (seg_tailroom - len));
				break;
			}
			DATA_LEN(seg) = seg_tailroom;
			PKT_LEN(seg) = seg_tailroom;
			/* Sanity check. */
			assert(rte_pktmbuf_headroom(seg) == seg_headroom);
			assert(rte_pktmbuf_tailroom(seg) == 0);
			/* Fix len and clear headroom for next segments. */
			len -= seg_tailroom;
			seg_headroom = 0;
		}
		/* Update head and tail segments. */
		*pkt_buf_next = NULL;
		assert(pkt_buf != NULL);
		assert(j != 0);
		NB_SEGS(pkt_buf) = j;
		PORT(pkt_buf) = rxq->port_id;
		PKT_LEN(pkt_buf) = pkt_buf_len;
#ifdef RTE_NEXT_ABI
		pkt_buf->packet_type = rxq_cq_to_pkt_type(flags);
#endif
		pkt_buf->ol_flags = rxq_cq_to_ol_flags(rxq, flags);

		/* Return packet. */
		*(pkts++) = pkt_buf;
		++pkts_ret;
#ifdef MLX5_PMD_SOFT_COUNTERS
		/* Increase bytes counter. */
		rxq->stats.ibytes += pkt_buf_len;
#endif
repost:
		ret = rxq->if_wq->recv_sg_list(rxq->wq,
					       elt->sges,
					       elemof(elt->sges));
		if (unlikely(ret)) {
			/* Inability to repost WRs is fatal. */
			DEBUG("%p: recv_sg_list(): failed (ret=%d)",
			      (void *)rxq->priv,
			      ret);
			abort();
		}
		if (++elts_head >= elts_n)
			elts_head = 0;
		continue;
	}
	if (unlikely(i == 0))
		return 0;
	rxq->elts_head = elts_head;
#ifdef MLX5_PMD_SOFT_COUNTERS
	/* Increase packets counter. */
	rxq->stats.ipackets += pkts_ret;
#endif
	return pkts_ret;
}

/**
 * DPDK callback for RX.
 *
 * The following function is the same as mlx5_rx_burst_sp(), except it doesn't
 * manage scattered packets. Improves performance when MRU is lower than the
 * size of the first segment.
 *
 * @param dpdk_rxq
 *   Generic pointer to RX queue structure.
 * @param[out] pkts
 *   Array to store received packets.
 * @param pkts_n
 *   Maximum number of packets in array.
 *
 * @return
 *   Number of packets successfully received (<= pkts_n).
 */
static uint16_t
mlx5_rx_burst(void *dpdk_rxq, struct rte_mbuf **pkts, uint16_t pkts_n)
{
	struct rxq *rxq = (struct rxq *)dpdk_rxq;
	struct rxq_elt (*elts)[rxq->elts_n] = rxq->elts.no_sp;
	const unsigned int elts_n = rxq->elts_n;
	unsigned int elts_head = rxq->elts_head;
	struct ibv_sge sges[pkts_n];
	unsigned int i;
	unsigned int pkts_ret = 0;
	int ret;

	if (unlikely(rxq->sp))
		return mlx5_rx_burst_sp(dpdk_rxq, pkts, pkts_n);
	for (i = 0; (i != pkts_n); ++i) {
		struct rxq_elt *elt = &(*elts)[elts_head];
		unsigned int len;
		struct rte_mbuf *seg = elt->buf;
		struct rte_mbuf *rep;
		uint32_t flags;

		/* Sanity checks. */
		assert(seg != NULL);
		assert(elts_head < rxq->elts_n);
		assert(rxq->elts_head < rxq->elts_n);
		ret = rxq->if_cq->poll_length_flags(rxq->cq, NULL, NULL,
						    &flags);
		if (unlikely(ret < 0)) {
			struct ibv_wc wc;
			int wcs_n;

			DEBUG("rxq=%p, poll_length() failed (ret=%d)",
			      (void *)rxq, ret);
			/* ibv_poll_cq() must be used in case of failure. */
			wcs_n = ibv_poll_cq(rxq->cq, 1, &wc);
			if (unlikely(wcs_n == 0))
				break;
			if (unlikely(wcs_n < 0)) {
				DEBUG("rxq=%p, ibv_poll_cq() failed (wcs_n=%d)",
				      (void *)rxq, wcs_n);
				break;
			}
			assert(wcs_n == 1);
			if (unlikely(wc.status != IBV_WC_SUCCESS)) {
				/* Whatever, just repost the offending WR. */
				DEBUG("rxq=%p, wr_id=%" PRIu64 ": bad work"
				      " completion status (%d): %s",
				      (void *)rxq, wc.wr_id, wc.status,
				      ibv_wc_status_str(wc.status));
#ifdef MLX5_PMD_SOFT_COUNTERS
				/* Increment dropped packets counter. */
				++rxq->stats.idropped;
#endif
				/* Add SGE to array for repost. */
				sges[i] = elt->sge;
				goto repost;
			}
			ret = wc.byte_len;
		}
		if (ret == 0)
			break;
		len = ret;
		/*
		 * Fetch initial bytes of packet descriptor into a
		 * cacheline while allocating rep.
		 */
		rte_prefetch0(seg);
		rep = __rte_mbuf_raw_alloc(rxq->mp);
		if (unlikely(rep == NULL)) {
			/*
			 * Unable to allocate a replacement mbuf,
			 * repost WR.
			 */
			DEBUG("rxq=%p: can't allocate a new mbuf",
			      (void *)rxq);
			/* Increase out of memory counters. */
			++rxq->stats.rx_nombuf;
			++rxq->priv->dev->data->rx_mbuf_alloc_failed;
			goto repost;
		}

		/* Reconfigure sge to use rep instead of seg. */
		elt->sge.addr = (uintptr_t)rep->buf_addr + RTE_PKTMBUF_HEADROOM;
		assert(elt->sge.lkey == rxq->mr->lkey);
		elt->buf = rep;

		/* Add SGE to array for repost. */
		sges[i] = elt->sge;

		/* Update seg information. */
		SET_DATA_OFF(seg, RTE_PKTMBUF_HEADROOM);
		NB_SEGS(seg) = 1;
		PORT(seg) = rxq->port_id;
		NEXT(seg) = NULL;
		PKT_LEN(seg) = len;
		DATA_LEN(seg) = len;
#ifdef RTE_NEXT_ABI
		seg->packet_type = rxq_cq_to_pkt_type(flags);
#endif
		seg->ol_flags = rxq_cq_to_ol_flags(rxq, flags);

		/* Return packet. */
		*(pkts++) = seg;
		++pkts_ret;
#ifdef MLX5_PMD_SOFT_COUNTERS
		/* Increase bytes counter. */
		rxq->stats.ibytes += len;
#endif
repost:
		if (++elts_head >= elts_n)
			elts_head = 0;
		continue;
	}
	if (unlikely(i == 0))
		return 0;
	/* Repost WRs. */
#ifdef DEBUG_RECV
	DEBUG("%p: reposting %u WRs", (void *)rxq, i);
#endif
	ret = rxq->if_wq->recv_burst(rxq->wq, sges, i);
	if (unlikely(ret)) {
		/* Inability to repost WRs is fatal. */
		DEBUG("%p: recv_burst(): failed (ret=%d)",
		      (void *)rxq->priv,
		      ret);
		abort();
	}
	rxq->elts_head = elts_head;
#ifdef MLX5_PMD_SOFT_COUNTERS
	/* Increase packets counter. */
	rxq->stats.ipackets += pkts_ret;
#endif
	return pkts_ret;
}

/**
 * Reconfigure a RX queue with new parameters.
 *
 * rxq_rehash() does not allocate mbufs, which, if not done from the right
 * thread (such as a control thread), may corrupt the pool.
 * In case of failure, the queue is left untouched.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param rxq
 *   RX queue pointer.
 *
 * @return
 *   0 on success, errno value on failure.
 */
static int
rxq_rehash(struct rte_eth_dev *dev, struct rxq *rxq)
{
	struct priv *priv = rxq->priv;
	struct rxq tmpl = *rxq;
	unsigned int mbuf_n;
	unsigned int desc_n;
	struct rte_mbuf **pool;
	unsigned int i, k;
	struct ibv_exp_wq_attr mod;
	int err;

	DEBUG("%p: rehashing queue %p", (void *)dev, (void *)rxq);
	/* Number of descriptors and mbufs currently allocated. */
	desc_n = (tmpl.elts_n * (tmpl.sp ? MLX5_PMD_SGE_WR_N : 1));
	mbuf_n = desc_n;
	/* Toggle RX checksum offload if hardware supports it. */
	if (priv->hw_csum) {
		tmpl.csum = !!dev->data->dev_conf.rxmode.hw_ip_checksum;
		rxq->csum = tmpl.csum;
	}
	if (priv->hw_csum_l2tun) {
		tmpl.csum_l2tun = !!dev->data->dev_conf.rxmode.hw_ip_checksum;
		rxq->csum_l2tun = tmpl.csum_l2tun;
	}
	/* Enable scattered packets support for this queue if necessary. */
	if ((dev->data->dev_conf.rxmode.jumbo_frame) &&
	    (dev->data->dev_conf.rxmode.max_rx_pkt_len >
	     (tmpl.mb_len - RTE_PKTMBUF_HEADROOM))) {
		tmpl.sp = 1;
		desc_n /= MLX5_PMD_SGE_WR_N;
	} else
		tmpl.sp = 0;
	DEBUG("%p: %s scattered packets support (%u WRs)",
	      (void *)dev, (tmpl.sp ? "enabling" : "disabling"), desc_n);
	/* If scatter mode is the same as before, nothing to do. */
	if (tmpl.sp == rxq->sp) {
		DEBUG("%p: nothing to do", (void *)dev);
		return 0;
	}
	/* From now on, any failure will render the queue unusable.
	 * Reinitialize WQ. */
	mod = (struct ibv_exp_wq_attr){
		.attr_mask = IBV_EXP_WQ_ATTR_STATE,
		.wq_state = IBV_EXP_WQS_RESET,
	};
	err = ibv_exp_modify_wq(tmpl.wq, &mod);
	if (err) {
		ERROR("%p: cannot reset WQ: %s", (void *)dev, strerror(err));
		assert(err > 0);
		return err;
	}
	/* Allocate pool. */
	pool = rte_malloc(__func__, (mbuf_n * sizeof(*pool)), 0);
	if (pool == NULL) {
		ERROR("%p: cannot allocate memory", (void *)dev);
		return ENOBUFS;
	}
	/* Snatch mbufs from original queue. */
	k = 0;
	if (rxq->sp) {
		struct rxq_elt_sp (*elts)[rxq->elts_n] = rxq->elts.sp;

		for (i = 0; (i != elemof(*elts)); ++i) {
			struct rxq_elt_sp *elt = &(*elts)[i];
			unsigned int j;

			for (j = 0; (j != elemof(elt->bufs)); ++j) {
				assert(elt->bufs[j] != NULL);
				pool[k++] = elt->bufs[j];
			}
		}
	} else {
		struct rxq_elt (*elts)[rxq->elts_n] = rxq->elts.no_sp;

		for (i = 0; (i != elemof(*elts)); ++i) {
			struct rxq_elt *elt = &(*elts)[i];
			struct rte_mbuf *buf = elt->buf;

			pool[k++] = buf;
		}
	}
	assert(k == mbuf_n);
	tmpl.elts_n = 0;
	tmpl.elts.sp = NULL;
	assert((void *)&tmpl.elts.sp == (void *)&tmpl.elts.no_sp);
	err = ((tmpl.sp) ?
	       rxq_alloc_elts_sp(&tmpl, desc_n, pool) :
	       rxq_alloc_elts(&tmpl, desc_n, pool));
	if (err) {
		ERROR("%p: cannot reallocate WRs, aborting", (void *)dev);
		rte_free(pool);
		assert(err > 0);
		return err;
	}
	assert(tmpl.elts_n == desc_n);
	assert(tmpl.elts.sp != NULL);
	rte_free(pool);
	/* Clean up original data. */
	rxq->elts_n = 0;
	rte_free(rxq->elts.sp);
	rxq->elts.sp = NULL;
	/* Change queue state to ready. */
	mod = (struct ibv_exp_wq_attr){
		.attr_mask = IBV_EXP_WQ_ATTR_STATE,
		.wq_state = IBV_EXP_WQS_RDY,
	};
	err = ibv_exp_modify_wq(tmpl.wq, &mod);
	if (err) {
		ERROR("%p: WQ state to IBV_EXP_WQS_RDY failed: %s",
		      (void *)dev, strerror(err));
		goto error;
	}
	/* Post SGEs. */
	assert(tmpl.if_wq != NULL);
	if (tmpl.sp) {
		struct rxq_elt_sp (*elts)[tmpl.elts_n] = tmpl.elts.sp;

		for (i = 0; (i != elemof(*elts)); ++i) {
			err = tmpl.if_wq->recv_sg_list
				(tmpl.wq,
				 (*elts)[i].sges,
				 elemof((*elts)[i].sges));
			if (err)
				break;
		}
	} else {
		struct rxq_elt (*elts)[tmpl.elts_n] = tmpl.elts.no_sp;

		for (i = 0; (i != elemof(*elts)); ++i) {
			err = tmpl.if_wq->recv_burst(
				tmpl.wq,
				&(*elts)[i].sge,
				1);
			if (err)
				break;
		}
	}
	if (err) {
		ERROR("%p: failed to post SGEs with error %d",
		      (void *)dev, err);
		/* Set err because it does not contain a valid errno value. */
		err = EIO;
		goto error;
	}
error:
	*rxq = tmpl;
	assert(err >= 0);
	return err;
}

/**
 * Configure a RX queue.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param rxq
 *   Pointer to RX queue structure.
 * @param desc
 *   Number of descriptors to configure in queue.
 * @param socket
 *   NUMA socket on which memory must be allocated.
 * @param[in] conf
 *   Thresholds parameters.
 * @param mp
 *   Memory pool for buffer allocations.
 *
 * @return
 *   0 on success, errno value on failure.
 */
static int
rxq_setup(struct rte_eth_dev *dev, struct rxq *rxq, uint16_t desc,
	  unsigned int socket, const struct rte_eth_rxconf *conf,
	  struct rte_mempool *mp)
{
	struct priv *priv = dev->data->dev_private;
	struct rxq tmpl = {
		.priv = priv,
		.mp = mp,
		.socket = socket
	};
	struct ibv_exp_wq_attr mod;
	union {
		struct ibv_exp_query_intf_params params;
		struct ibv_exp_cq_init_attr cq;
		struct ibv_exp_res_domain_init_attr rd;
		struct ibv_exp_wq_init_attr wq;
	} attr;
	enum ibv_exp_query_intf_status status;
	struct rte_mbuf *buf;
	int ret = 0;
	unsigned int i;
	unsigned int cq_size = desc;

	(void)conf; /* Thresholds configuration (ignored). */
	if ((desc == 0) || (desc % MLX5_PMD_SGE_WR_N)) {
		ERROR("%p: invalid number of RX descriptors (must be a"
		      " multiple of %d)", (void *)dev, MLX5_PMD_SGE_WR_N);
		return EINVAL;
	}
	/* Get mbuf length. */
	buf = rte_pktmbuf_alloc(mp);
	if (buf == NULL) {
		ERROR("%p: unable to allocate mbuf", (void *)dev);
		return ENOMEM;
	}
	tmpl.mb_len = buf->buf_len;
	assert((rte_pktmbuf_headroom(buf) +
		rte_pktmbuf_tailroom(buf)) == tmpl.mb_len);
	assert(rte_pktmbuf_headroom(buf) == RTE_PKTMBUF_HEADROOM);
	rte_pktmbuf_free(buf);
	/* Toggle RX checksum offload if hardware supports it. */
	if (priv->hw_csum)
		tmpl.csum = !!dev->data->dev_conf.rxmode.hw_ip_checksum;
	if (priv->hw_csum_l2tun)
		tmpl.csum_l2tun = !!dev->data->dev_conf.rxmode.hw_ip_checksum;
	/* Enable scattered packets support for this queue if necessary. */
	if ((dev->data->dev_conf.rxmode.jumbo_frame) &&
	    (dev->data->dev_conf.rxmode.max_rx_pkt_len >
	     (tmpl.mb_len - RTE_PKTMBUF_HEADROOM))) {
		tmpl.sp = 1;
		desc /= MLX5_PMD_SGE_WR_N;
	}
	DEBUG("%p: %s scattered packets support (%u WRs)",
	      (void *)dev, (tmpl.sp ? "enabling" : "disabling"), desc);
	/* Use the entire RX mempool as the memory region. */
	tmpl.mr = ibv_reg_mr(priv->pd,
			     (void *)mp->elt_va_start,
			     (mp->elt_va_end - mp->elt_va_start),
			     (IBV_ACCESS_LOCAL_WRITE |
			      IBV_ACCESS_REMOTE_WRITE));
	if (tmpl.mr == NULL) {
		ret = EINVAL;
		ERROR("%p: MR creation failure: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	attr.rd = (struct ibv_exp_res_domain_init_attr){
		.comp_mask = (IBV_EXP_RES_DOMAIN_THREAD_MODEL |
			      IBV_EXP_RES_DOMAIN_MSG_MODEL),
		.thread_model = IBV_EXP_THREAD_SINGLE,
		.msg_model = IBV_EXP_MSG_HIGH_BW,
	};
	tmpl.rd = ibv_exp_create_res_domain(priv->ctx, &attr.rd);
	if (tmpl.rd == NULL) {
		ret = ENOMEM;
		ERROR("%p: RD creation failure: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	attr.cq = (struct ibv_exp_cq_init_attr){
		.comp_mask = IBV_EXP_CQ_INIT_ATTR_RES_DOMAIN,
		.res_domain = tmpl.rd,
	};
	tmpl.cq = ibv_exp_create_cq(priv->ctx, cq_size, NULL, NULL, 0,
				    &attr.cq);
	if (tmpl.cq == NULL) {
		ret = ENOMEM;
		ERROR("%p: CQ creation failure: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	DEBUG("priv->device_attr.max_qp_wr is %d",
	      priv->device_attr.max_qp_wr);
	DEBUG("priv->device_attr.max_sge is %d",
	      priv->device_attr.max_sge);
	attr.wq = (struct ibv_exp_wq_init_attr){
		.wq_context = NULL, /* Could be useful in the future. */
		.wq_type = IBV_EXP_WQT_RQ,
		/* Max number of outstanding WRs. */
		.max_recv_wr = ((priv->device_attr.max_qp_wr < (int)cq_size) ?
				priv->device_attr.max_qp_wr :
				(int)cq_size),
		/* Max number of scatter/gather elements in a WR. */
		.max_recv_sge = ((priv->device_attr.max_sge <
				  MLX5_PMD_SGE_WR_N) ?
				 priv->device_attr.max_sge :
				 MLX5_PMD_SGE_WR_N),
		.pd = priv->pd,
		.cq = tmpl.cq,
		.comp_mask = IBV_EXP_CREATE_WQ_RES_DOMAIN,
		.res_domain = tmpl.rd,
	};
	tmpl.wq = ibv_exp_create_wq(priv->ctx, &attr.wq);
	if (tmpl.wq == NULL) {
		ret = (errno ? errno : EINVAL);
		ERROR("%p: WQ creation failure: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	if (tmpl.sp)
		ret = rxq_alloc_elts_sp(&tmpl, desc, NULL);
	else
		ret = rxq_alloc_elts(&tmpl, desc, NULL);
	if (ret) {
		ERROR("%p: RXQ allocation failed: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	/* Save port ID. */
	tmpl.port_id = dev->data->port_id;
	DEBUG("%p: RTE port ID: %u", (void *)rxq, tmpl.port_id);
	attr.params = (struct ibv_exp_query_intf_params){
		.intf_scope = IBV_EXP_INTF_GLOBAL,
		.intf = IBV_EXP_INTF_CQ,
		.obj = tmpl.cq,
	};
	tmpl.if_cq = ibv_exp_query_intf(priv->ctx, &attr.params, &status);
	if (tmpl.if_cq == NULL) {
		ERROR("%p: CQ interface family query failed with status %d",
		      (void *)dev, status);
		goto error;
	}
	attr.params = (struct ibv_exp_query_intf_params){
		.intf_scope = IBV_EXP_INTF_GLOBAL,
		.intf = IBV_EXP_INTF_WQ,
		.obj = tmpl.wq,
	};
	tmpl.if_wq = ibv_exp_query_intf(priv->ctx, &attr.params, &status);
	if (tmpl.if_wq == NULL) {
		ERROR("%p: WQ interface family query failed with status %d",
		      (void *)dev, status);
		goto error;
	}
	/* Change queue state to ready. */
	mod = (struct ibv_exp_wq_attr){
		.attr_mask = IBV_EXP_WQ_ATTR_STATE,
		.wq_state = IBV_EXP_WQS_RDY,
	};
	ret = ibv_exp_modify_wq(tmpl.wq, &mod);
	if (ret) {
		ERROR("%p: WQ state to IBV_EXP_WQS_RDY failed: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	/* Post SGEs. */
	if (tmpl.sp) {
		struct rxq_elt_sp (*elts)[tmpl.elts_n] = tmpl.elts.sp;

		for (i = 0; (i != elemof(*elts)); ++i) {
			ret = tmpl.if_wq->recv_sg_list
				(tmpl.wq,
				 (*elts)[i].sges,
				 elemof((*elts)[i].sges));
			if (ret)
				break;
		}
	} else {
		struct rxq_elt (*elts)[tmpl.elts_n] = tmpl.elts.no_sp;

		for (i = 0; (i != elemof(*elts)); ++i) {
			ret = tmpl.if_wq->recv_burst(
				tmpl.wq,
				&(*elts)[i].sge,
				1);
			if (ret)
				break;
		}
	}
	if (ret) {
		ERROR("%p: failed to post SGEs with error %d",
		      (void *)dev, ret);
		/* Set ret because it does not contain a valid errno value. */
		ret = EIO;
		goto error;
	}
	/* Clean up rxq in case we're reinitializing it. */
	DEBUG("%p: cleaning-up old rxq just in case", (void *)rxq);
	rxq_cleanup(rxq);
	*rxq = tmpl;
	DEBUG("%p: rxq updated with %p", (void *)rxq, (void *)&tmpl);
	assert(ret == 0);
	return 0;
error:
	rxq_cleanup(&tmpl);
	assert(ret > 0);
	return ret;
}

/**
 * DPDK callback to configure a RX queue.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param idx
 *   RX queue index.
 * @param desc
 *   Number of descriptors to configure in queue.
 * @param socket
 *   NUMA socket on which memory must be allocated.
 * @param[in] conf
 *   Thresholds parameters.
 * @param mp
 *   Memory pool for buffer allocations.
 *
 * @return
 *   0 on success, negative errno value on failure.
 */
static int
mlx5_rx_queue_setup(struct rte_eth_dev *dev, uint16_t idx, uint16_t desc,
		    unsigned int socket, const struct rte_eth_rxconf *conf,
		    struct rte_mempool *mp)
{
	struct priv *priv = dev->data->dev_private;
	struct rxq *rxq = (*priv->rxqs)[idx];
	int ret;

	priv_lock(priv);
	DEBUG("%p: configuring queue %u for %u descriptors",
	      (void *)dev, idx, desc);
	if (idx >= priv->rxqs_n) {
		ERROR("%p: queue index out of range (%u >= %u)",
		      (void *)dev, idx, priv->rxqs_n);
		priv_unlock(priv);
		return -EOVERFLOW;
	}
	if (rxq != NULL) {
		DEBUG("%p: reusing already allocated queue index %u (%p)",
		      (void *)dev, idx, (void *)rxq);
		if (priv->started) {
			priv_unlock(priv);
			return -EEXIST;
		}
		(*priv->rxqs)[idx] = NULL;
		rxq_cleanup(rxq);
	} else {
		rxq = rte_calloc_socket("RXQ", 1, sizeof(*rxq), 0, socket);
		if (rxq == NULL) {
			ERROR("%p: unable to allocate queue index %u",
			      (void *)dev, idx);
			priv_unlock(priv);
			return -ENOMEM;
		}
	}
	ret = rxq_setup(dev, rxq, desc, socket, conf, mp);
	if (ret)
		rte_free(rxq);
	else {
		rxq->stats.idx = idx;
		DEBUG("%p: adding RX queue %p to list",
		      (void *)dev, (void *)rxq);
		(*priv->rxqs)[idx] = rxq;
		/* Update receive callback. */
		if (rxq->sp)
			dev->rx_pkt_burst = mlx5_rx_burst_sp;
		else
			dev->rx_pkt_burst = mlx5_rx_burst;
	}
	priv_unlock(priv);
	return -ret;
}

/**
 * DPDK callback to release a RX queue.
 *
 * @param dpdk_rxq
 *   Generic RX queue pointer.
 */
static void
mlx5_rx_queue_release(void *dpdk_rxq)
{
	struct rxq *rxq = (struct rxq *)dpdk_rxq;
	struct priv *priv;
	unsigned int i;

	if (rxq == NULL)
		return;
	priv = rxq->priv;
	priv_lock(priv);
	for (i = 0; (i != priv->rxqs_n); ++i)
		if ((*priv->rxqs)[i] == rxq) {
			DEBUG("%p: removing RX queue %p from list",
			      (void *)priv->dev, (void *)rxq);
			(*priv->rxqs)[i] = NULL;
			break;
		}
	rxq_cleanup(rxq);
	rte_free(rxq);
	priv_unlock(priv);
}

/**
 * DPDK callback to start the device.
 *
 * Simulate device start by attaching all configured flows.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 *
 * @return
 *   0 on success, negative errno value on failure.
 */
static int
mlx5_dev_start(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	int err;

	priv_lock(priv);
	if (priv->started) {
		priv_unlock(priv);
		return 0;
	}
	DEBUG("%p: allocating and configuring RX hash queues", (void *)dev);
	err = priv_create_hash_rxqs(priv);
	if (!err)
		err = priv_mac_addrs_enable(priv);
	if (!err && priv->promisc_req)
		err = priv_promiscuous_enable(priv);
	if (!err && priv->allmulti_req)
		err = priv_allmulticast_enable(priv);
	if (!err)
		priv->started = 1;
	else {
		ERROR("%p: an error occured while configuring RX hash queues:"
		      " %s",
		      (void *)priv, strerror(err));
		/* Rollback. */
		priv_allmulticast_disable(priv);
		priv_promiscuous_disable(priv);
		priv_mac_addrs_disable(priv);
		priv_destroy_hash_rxqs(priv);
	}
	priv_unlock(priv);
	return -err;
}

/**
 * DPDK callback to stop the device.
 *
 * Simulate device stop by detaching all configured flows.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 */
static void
mlx5_dev_stop(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;

	priv_lock(priv);
	if (!priv->started) {
		priv_unlock(priv);
		return;
	}
	DEBUG("%p: cleaning up and destroying RX hash queues", (void *)dev);
	priv_allmulticast_disable(priv);
	priv_promiscuous_disable(priv);
	priv_mac_addrs_disable(priv);
	priv_destroy_hash_rxqs(priv);
	priv->started = 0;
	priv_unlock(priv);
}

/**
 * Dummy DPDK callback for TX.
 *
 * This function is used to temporarily replace the real callback during
 * unsafe control operations on the queue, or in case of error.
 *
 * @param dpdk_txq
 *   Generic pointer to TX queue structure.
 * @param[in] pkts
 *   Packets to transmit.
 * @param pkts_n
 *   Number of packets in array.
 *
 * @return
 *   Number of packets successfully transmitted (<= pkts_n).
 */
static uint16_t
removed_tx_burst(void *dpdk_txq, struct rte_mbuf **pkts, uint16_t pkts_n)
{
	(void)dpdk_txq;
	(void)pkts;
	(void)pkts_n;
	return 0;
}

/**
 * Dummy DPDK callback for RX.
 *
 * This function is used to temporarily replace the real callback during
 * unsafe control operations on the queue, or in case of error.
 *
 * @param dpdk_rxq
 *   Generic pointer to RX queue structure.
 * @param[out] pkts
 *   Array to store received packets.
 * @param pkts_n
 *   Maximum number of packets in array.
 *
 * @return
 *   Number of packets successfully received (<= pkts_n).
 */
static uint16_t
removed_rx_burst(void *dpdk_rxq, struct rte_mbuf **pkts, uint16_t pkts_n)
{
	(void)dpdk_rxq;
	(void)pkts;
	(void)pkts_n;
	return 0;
}

/**
 * DPDK callback to close the device.
 *
 * Destroy all queues and objects, free memory.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 */
static void
mlx5_dev_close(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	void *tmp;
	unsigned int i;

	priv_lock(priv);
	DEBUG("%p: closing device \"%s\"",
	      (void *)dev,
	      ((priv->ctx != NULL) ? priv->ctx->device->name : ""));
	/* In case mlx5_dev_stop() has not been called. */
	if (priv->started) {
		priv_allmulticast_disable(priv);
		priv_promiscuous_disable(priv);
		priv_mac_addrs_disable(priv);
		priv_destroy_hash_rxqs(priv);
	}
	/* Prevent crashes when queues are still in use. This is unfortunately
	 * still required for DPDK 1.3 because some programs (such as testpmd)
	 * never release them before closing the device. */
	dev->rx_pkt_burst = removed_rx_burst;
	dev->tx_pkt_burst = removed_tx_burst;
	if (priv->rxqs != NULL) {
		/* XXX race condition if mlx5_rx_burst() is still running. */
		usleep(1000);
		for (i = 0; (i != priv->rxqs_n); ++i) {
			tmp = (*priv->rxqs)[i];
			if (tmp == NULL)
				continue;
			(*priv->rxqs)[i] = NULL;
			rxq_cleanup(tmp);
			rte_free(tmp);
		}
		priv->rxqs_n = 0;
		priv->rxqs = NULL;
	}
	if (priv->txqs != NULL) {
		/* XXX race condition if mlx5_tx_burst() is still running. */
		usleep(1000);
		for (i = 0; (i != priv->txqs_n); ++i) {
			tmp = (*priv->txqs)[i];
			if (tmp == NULL)
				continue;
			(*priv->txqs)[i] = NULL;
			txq_cleanup(tmp);
			rte_free(tmp);
		}
		priv->txqs_n = 0;
		priv->txqs = NULL;
	}
	if (priv->pd != NULL) {
		assert(priv->ctx != NULL);
		claim_zero(ibv_dealloc_pd(priv->pd));
		claim_zero(ibv_close_device(priv->ctx));
	} else
		assert(priv->ctx == NULL);
	if (priv->rss_conf != NULL) {
		for (i = 0; (i != elemof(hash_rxq_init)); ++i)
			rte_free((*priv->rss_conf)[i]);
		rte_free(priv->rss_conf);
	}
	priv_unlock(priv);
	memset(priv, 0, sizeof(*priv));
}

/**
 * DPDK callback to get information about the device.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param[out] info
 *   Info structure output buffer.
 */
static void
mlx5_dev_infos_get(struct rte_eth_dev *dev, struct rte_eth_dev_info *info)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int max;

	priv_lock(priv);
	/* FIXME: we should ask the device for these values. */
	info->min_rx_bufsize = 32;
	info->max_rx_pktlen = 65536;
	/*
	 * Since we need one CQ per QP, the limit is the minimum number
	 * between the two values.
	 */
	max = ((priv->device_attr.max_cq > priv->device_attr.max_qp) ?
	       priv->device_attr.max_qp : priv->device_attr.max_cq);
	/* If max >= 65535 then max = 0, max_rx_queues is uint16_t. */
	if (max >= 65535)
		max = 65535;
	info->max_rx_queues = max;
	info->max_tx_queues = max;
	info->max_mac_addrs = elemof(priv->mac);
	info->rx_offload_capa =
		(priv->hw_csum ?
		 (DEV_RX_OFFLOAD_IPV4_CKSUM |
		  DEV_RX_OFFLOAD_UDP_CKSUM |
		  DEV_RX_OFFLOAD_TCP_CKSUM) :
		 0);
	info->tx_offload_capa =
		(priv->hw_csum ?
		 (DEV_TX_OFFLOAD_IPV4_CKSUM |
		  DEV_TX_OFFLOAD_UDP_CKSUM |
		  DEV_TX_OFFLOAD_TCP_CKSUM) :
		 0);
	priv_unlock(priv);
}

/**
 * DPDK callback to get device statistics.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param[out] stats
 *   Stats structure output buffer.
 */
static void
mlx5_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *stats)
{
	struct priv *priv = dev->data->dev_private;
	struct rte_eth_stats tmp = {0};
	unsigned int i;
	unsigned int idx;

	priv_lock(priv);
	/* Add software counters. */
	for (i = 0; (i != priv->rxqs_n); ++i) {
		struct rxq *rxq = (*priv->rxqs)[i];

		if (rxq == NULL)
			continue;
		idx = rxq->stats.idx;
		if (idx < RTE_ETHDEV_QUEUE_STAT_CNTRS) {
#ifdef MLX5_PMD_SOFT_COUNTERS
			tmp.q_ipackets[idx] += rxq->stats.ipackets;
			tmp.q_ibytes[idx] += rxq->stats.ibytes;
#endif
			tmp.q_errors[idx] += (rxq->stats.idropped +
					      rxq->stats.rx_nombuf);
		}
#ifdef MLX5_PMD_SOFT_COUNTERS
		tmp.ipackets += rxq->stats.ipackets;
		tmp.ibytes += rxq->stats.ibytes;
#endif
		tmp.ierrors += rxq->stats.idropped;
		tmp.rx_nombuf += rxq->stats.rx_nombuf;
	}
	for (i = 0; (i != priv->txqs_n); ++i) {
		struct txq *txq = (*priv->txqs)[i];

		if (txq == NULL)
			continue;
		idx = txq->stats.idx;
		if (idx < RTE_ETHDEV_QUEUE_STAT_CNTRS) {
#ifdef MLX5_PMD_SOFT_COUNTERS
			tmp.q_opackets[idx] += txq->stats.opackets;
			tmp.q_obytes[idx] += txq->stats.obytes;
#endif
			tmp.q_errors[idx] += txq->stats.odropped;
		}
#ifdef MLX5_PMD_SOFT_COUNTERS
		tmp.opackets += txq->stats.opackets;
		tmp.obytes += txq->stats.obytes;
#endif
		tmp.oerrors += txq->stats.odropped;
	}
#ifndef MLX5_PMD_SOFT_COUNTERS
	/* FIXME: retrieve and add hardware counters. */
#endif
	*stats = tmp;
	priv_unlock(priv);
}

/**
 * DPDK callback to clear device statistics.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 */
static void
mlx5_stats_reset(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int i;
	unsigned int idx;

	priv_lock(priv);
	for (i = 0; (i != priv->rxqs_n); ++i) {
		if ((*priv->rxqs)[i] == NULL)
			continue;
		idx = (*priv->rxqs)[i]->stats.idx;
		(*priv->rxqs)[i]->stats =
			(struct mlx5_rxq_stats){ .idx = idx };
	}
	for (i = 0; (i != priv->txqs_n); ++i) {
		if ((*priv->txqs)[i] == NULL)
			continue;
		idx = (*priv->rxqs)[i]->stats.idx;
		(*priv->txqs)[i]->stats =
			(struct mlx5_txq_stats){ .idx = idx };
	}
#ifndef MLX5_PMD_SOFT_COUNTERS
	/* FIXME: reset hardware counters. */
#endif
	priv_unlock(priv);
}

/**
 * DPDK callback to remove a MAC address.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param index
 *   MAC address index.
 */
static void
mlx5_mac_addr_remove(struct rte_eth_dev *dev, uint32_t index)
{
	struct priv *priv = dev->data->dev_private;

	priv_lock(priv);
	DEBUG("%p: removing MAC address from index %" PRIu32,
	      (void *)dev, index);
	if (index >= MLX5_MAX_MAC_ADDRESSES)
		goto end;
	/* Refuse to remove the broadcast address, this one is special. */
	if (!memcmp(priv->mac[index].addr_bytes, "\xff\xff\xff\xff\xff\xff",
		    ETHER_ADDR_LEN))
		goto end;
	priv_mac_addr_del(priv, index);
end:
	priv_unlock(priv);
}

/**
 * DPDK callback to add a MAC address.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param mac_addr
 *   MAC address to register.
 * @param index
 *   MAC address index.
 * @param vmdq
 *   VMDq pool index to associate address with (ignored).
 */
static void
mlx5_mac_addr_add(struct rte_eth_dev *dev, struct ether_addr *mac_addr,
		  uint32_t index, uint32_t vmdq)
{
	struct priv *priv = dev->data->dev_private;

	(void)vmdq;
	priv_lock(priv);
	DEBUG("%p: adding MAC address at index %" PRIu32,
	      (void *)dev, index);
	if (index >= MLX5_MAX_MAC_ADDRESSES)
		goto end;
	/* Refuse to add the broadcast address, this one is special. */
	if (!memcmp(mac_addr->addr_bytes, "\xff\xff\xff\xff\xff\xff",
		    ETHER_ADDR_LEN))
		goto end;
	priv_mac_addr_add(priv, index,
			  (const uint8_t (*)[ETHER_ADDR_LEN])
			  mac_addr->addr_bytes);
end:
	priv_unlock(priv);
}

/**
 * DPDK callback to enable promiscuous mode.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 */
static void
mlx5_promiscuous_enable(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	int ret;

	priv_lock(priv);
	priv->promisc_req = 1;
	ret = priv_promiscuous_enable(priv);
	if (ret)
		ERROR("cannot enable promiscuous mode: %s", strerror(ret));
	priv_unlock(priv);
}

/**
 * DPDK callback to disable promiscuous mode.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 */
static void
mlx5_promiscuous_disable(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;

	priv_lock(priv);
	priv->promisc_req = 0;
	priv_promiscuous_disable(priv);
	priv_unlock(priv);
}

/**
 * DPDK callback to enable allmulti mode.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 */
static void
mlx5_allmulticast_enable(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	int ret;

	priv_lock(priv);
	priv->allmulti_req = 1;
	ret = priv_allmulticast_enable(priv);
	if (ret)
		ERROR("cannot enable allmulticast mode: %s", strerror(ret));
	priv_unlock(priv);
}

/**
 * DPDK callback to disable allmulti mode.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 */
static void
mlx5_allmulticast_disable(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;

	priv_lock(priv);
	priv->allmulti_req = 0;
	priv_allmulticast_disable(priv);
	priv_unlock(priv);
}

/**
 * DPDK callback to retrieve physical link information (unlocked version).
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param wait_to_complete
 *   Wait for request completion (ignored).
 */
static int
mlx5_link_update_unlocked(struct rte_eth_dev *dev, int wait_to_complete)
{
	struct priv *priv = dev->data->dev_private;
	struct ethtool_cmd edata = {
		.cmd = ETHTOOL_GSET
	};
	struct ifreq ifr;
	struct rte_eth_link dev_link;
	int link_speed = 0;

	(void)wait_to_complete;
	if (priv_ifreq(priv, SIOCGIFFLAGS, &ifr)) {
		WARN("ioctl(SIOCGIFFLAGS) failed: %s", strerror(errno));
		return -1;
	}
	memset(&dev_link, 0, sizeof(dev_link));
	dev_link.link_status = ((ifr.ifr_flags & IFF_UP) &&
				(ifr.ifr_flags & IFF_RUNNING));
	ifr.ifr_data = &edata;
	if (priv_ifreq(priv, SIOCETHTOOL, &ifr)) {
		WARN("ioctl(SIOCETHTOOL, ETHTOOL_GSET) failed: %s",
		     strerror(errno));
		return -1;
	}
	link_speed = ethtool_cmd_speed(&edata);
	if (link_speed == -1)
		dev_link.link_speed = 0;
	else
		dev_link.link_speed = link_speed;
	dev_link.link_duplex = ((edata.duplex == DUPLEX_HALF) ?
				ETH_LINK_HALF_DUPLEX : ETH_LINK_FULL_DUPLEX);
	if (memcmp(&dev_link, &dev->data->dev_link, sizeof(dev_link))) {
		/* Link status changed. */
		dev->data->dev_link = dev_link;
		return 0;
	}
	/* Link status is still the same. */
	return -1;
}

/**
 * DPDK callback to retrieve physical link information.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param wait_to_complete
 *   Wait for request completion (ignored).
 */
static int
mlx5_link_update(struct rte_eth_dev *dev, int wait_to_complete)
{
	struct priv *priv = dev->data->dev_private;
	int ret;

	priv_lock(priv);
	ret = mlx5_link_update_unlocked(dev, wait_to_complete);
	priv_unlock(priv);
	return ret;
}

/**
 * DPDK callback to change the MTU.
 *
 * Setting the MTU affects hardware MRU (packets larger than the MTU cannot be
 * received). Use this as a hint to enable/disable scattered packets support
 * and improve performance when not needed.
 * Since failure is not an option, reconfiguring queues on the fly is not
 * recommended.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param in_mtu
 *   New MTU.
 *
 * @return
 *   0 on success, negative errno value on failure.
 */
static int
mlx5_dev_set_mtu(struct rte_eth_dev *dev, uint16_t mtu)
{
	struct priv *priv = dev->data->dev_private;
	int ret = 0;
	unsigned int i;
	uint16_t (*rx_func)(void *, struct rte_mbuf **, uint16_t) =
		mlx5_rx_burst;

	priv_lock(priv);
	/* Set kernel interface MTU first. */
	if (priv_set_mtu(priv, mtu)) {
		ret = errno;
		WARN("cannot set port %u MTU to %u: %s", priv->port, mtu,
		     strerror(ret));
		goto out;
	} else
		DEBUG("adapter port %u MTU set to %u", priv->port, mtu);
	priv->mtu = mtu;
	/* Temporarily replace RX handler with a fake one, assuming it has not
	 * been copied elsewhere. */
	dev->rx_pkt_burst = removed_rx_burst;
	/* Make sure everyone has left mlx5_rx_burst() and uses
	 * removed_rx_burst() instead. */
	rte_wmb();
	usleep(1000);
	/* Reconfigure each RX queue. */
	for (i = 0; (i != priv->rxqs_n); ++i) {
		struct rxq *rxq = (*priv->rxqs)[i];
		unsigned int max_frame_len;
		int sp;

		if (rxq == NULL)
			continue;
		/* Calculate new maximum frame length according to MTU and
		 * toggle scattered support (sp) if necessary. */
		max_frame_len = (priv->mtu + ETHER_HDR_LEN +
				 (ETHER_MAX_VLAN_FRAME_LEN - ETHER_MAX_LEN));
		sp = (max_frame_len > (rxq->mb_len - RTE_PKTMBUF_HEADROOM));
		/* Provide new values to rxq_setup(). */
		dev->data->dev_conf.rxmode.jumbo_frame = sp;
		dev->data->dev_conf.rxmode.max_rx_pkt_len = max_frame_len;
		ret = rxq_rehash(dev, rxq);
		if (ret) {
			/* Force SP RX if that queue requires it and abort. */
			if (rxq->sp)
				rx_func = mlx5_rx_burst_sp;
			break;
		}
		/* Scattered burst function takes priority. */
		if (rxq->sp)
			rx_func = mlx5_rx_burst_sp;
	}
	/* Burst functions can now be called again. */
	rte_wmb();
	dev->rx_pkt_burst = rx_func;
out:
	priv_unlock(priv);
	assert(ret >= 0);
	return -ret;
}

/**
 * DPDK callback to get flow control status.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param[out] fc_conf
 *   Flow control output buffer.
 *
 * @return
 *   0 on success, negative errno value on failure.
 */
static int
mlx5_dev_get_flow_ctrl(struct rte_eth_dev *dev, struct rte_eth_fc_conf *fc_conf)
{
	struct priv *priv = dev->data->dev_private;
	struct ifreq ifr;
	struct ethtool_pauseparam ethpause = {
		.cmd = ETHTOOL_GPAUSEPARAM
	};
	int ret;

	ifr.ifr_data = &ethpause;
	priv_lock(priv);
	if (priv_ifreq(priv, SIOCETHTOOL, &ifr)) {
		ret = errno;
		WARN("ioctl(SIOCETHTOOL, ETHTOOL_GPAUSEPARAM)"
		     " failed: %s",
		     strerror(ret));
		goto out;
	}

	fc_conf->autoneg = ethpause.autoneg;
	if (ethpause.rx_pause && ethpause.tx_pause)
		fc_conf->mode = RTE_FC_FULL;
	else if (ethpause.rx_pause)
		fc_conf->mode = RTE_FC_RX_PAUSE;
	else if (ethpause.tx_pause)
		fc_conf->mode = RTE_FC_TX_PAUSE;
	else
		fc_conf->mode = RTE_FC_NONE;
	ret = 0;

out:
	priv_unlock(priv);
	assert(ret >= 0);
	return -ret;
}

/**
 * DPDK callback to modify flow control parameters.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param[in] fc_conf
 *   Flow control parameters.
 *
 * @return
 *   0 on success, negative errno value on failure.
 */
static int
mlx5_dev_set_flow_ctrl(struct rte_eth_dev *dev, struct rte_eth_fc_conf *fc_conf)
{
	struct priv *priv = dev->data->dev_private;
	struct ifreq ifr;
	struct ethtool_pauseparam ethpause = {
		.cmd = ETHTOOL_SPAUSEPARAM
	};
	int ret;

	ifr.ifr_data = &ethpause;
	ethpause.autoneg = fc_conf->autoneg;
	if (((fc_conf->mode & RTE_FC_FULL) == RTE_FC_FULL) ||
	    (fc_conf->mode & RTE_FC_RX_PAUSE))
		ethpause.rx_pause = 1;
	else
		ethpause.rx_pause = 0;

	if (((fc_conf->mode & RTE_FC_FULL) == RTE_FC_FULL) ||
	    (fc_conf->mode & RTE_FC_TX_PAUSE))
		ethpause.tx_pause = 1;
	else
		ethpause.tx_pause = 0;

	priv_lock(priv);
	if (priv_ifreq(priv, SIOCETHTOOL, &ifr)) {
		ret = errno;
		WARN("ioctl(SIOCETHTOOL, ETHTOOL_SPAUSEPARAM)"
		     " failed: %s",
		     strerror(ret));
		goto out;
	}
	ret = 0;

out:
	priv_unlock(priv);
	assert(ret >= 0);
	return -ret;
}

/**
 * Configure a VLAN filter.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param vlan_id
 *   VLAN ID to filter.
 * @param on
 *   Toggle filter.
 *
 * @return
 *   0 on success, errno value on failure.
 */
static int
vlan_filter_set(struct rte_eth_dev *dev, uint16_t vlan_id, int on)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int i;
	unsigned int j = -1;

	DEBUG("%p: %s VLAN filter ID %" PRIu16,
	      (void *)dev, (on ? "enable" : "disable"), vlan_id);
	for (i = 0; (i != elemof(priv->vlan_filter)); ++i) {
		if (!priv->vlan_filter[i].enabled) {
			/* Unused index, remember it. */
			j = i;
			continue;
		}
		if (priv->vlan_filter[i].id != vlan_id)
			continue;
		/* This VLAN ID is already known, use its index. */
		j = i;
		break;
	}
	/* Check if there's room for another VLAN filter. */
	if (j == (unsigned int)-1)
		return ENOMEM;
	/*
	 * VLAN filters apply to all configured MAC addresses, flow
	 * specifications must be reconfigured accordingly.
	 */
	priv->vlan_filter[j].id = vlan_id;
	if ((on) && (!priv->vlan_filter[j].enabled)) {
		/*
		 * Filter is disabled, enable it.
		 * Rehashing flows in all RX hash queues is necessary.
		 */
		for (i = 0; (i != priv->hash_rxqs_n); ++i)
			hash_rxq_mac_addrs_del(&(*priv->hash_rxqs)[i]);
		priv->vlan_filter[j].enabled = 1;
		if (priv->started)
			for (i = 0; (i != priv->hash_rxqs_n); ++i)
				hash_rxq_mac_addrs_add(&(*priv->hash_rxqs)[i]);
	} else if ((!on) && (priv->vlan_filter[j].enabled)) {
		/*
		 * Filter is enabled, disable it.
		 * Rehashing flows in all RX queues is necessary.
		 */
		for (i = 0; (i != priv->hash_rxqs_n); ++i)
			hash_rxq_mac_addrs_del(&(*priv->hash_rxqs)[i]);
		priv->vlan_filter[j].enabled = 0;
		if (priv->started)
			for (i = 0; (i != priv->hash_rxqs_n); ++i)
				hash_rxq_mac_addrs_add(&(*priv->hash_rxqs)[i]);
	}
	return 0;
}

/**
 * DPDK callback to configure a VLAN filter.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param vlan_id
 *   VLAN ID to filter.
 * @param on
 *   Toggle filter.
 *
 * @return
 *   0 on success, negative errno value on failure.
 */
static int
mlx5_vlan_filter_set(struct rte_eth_dev *dev, uint16_t vlan_id, int on)
{
	struct priv *priv = dev->data->dev_private;
	int ret;

	priv_lock(priv);
	ret = vlan_filter_set(dev, vlan_id, on);
	priv_unlock(priv);
	assert(ret >= 0);
	return -ret;
}

/**
 * Get a RSS configuration hash key.
 *
 * @param priv
 *   Pointer to private structure.
 * @param rss_hf
 *   RSS hash functions configuration must be retrieved for.
 *
 * @return
 *   Pointer to a RSS configuration structure or NULL if rss_hf cannot
 *   be matched.
 */
static struct rte_eth_rss_conf *
rss_hash_get(struct priv *priv, uint64_t rss_hf)
{
	unsigned int i;

	for (i = 0; (i != elemof(hash_rxq_init)); ++i) {
		uint64_t dpdk_rss_hf = hash_rxq_init[i].dpdk_rss_hf;

		if (!(dpdk_rss_hf & rss_hf))
			continue;
		return (*priv->rss_conf)[i];
	}
	return NULL;
}

/**
 * Register a RSS key.
 *
 * @param priv
 *   Pointer to private structure.
 * @param key
 *   Hash key to register.
 * @param key_len
 *   Hash key length in bytes.
 * @param rss_hf
 *   RSS hash functions the provided key applies to.
 *
 * @return
 *   0 on success, errno value on failure.
 */
static int
rss_hash_rss_conf_new_key(struct priv *priv, const uint8_t *key,
			  unsigned int key_len, uint64_t rss_hf)
{
	unsigned int i;

	for (i = 0; (i != elemof(hash_rxq_init)); ++i) {
		struct rte_eth_rss_conf *rss_conf;
		uint64_t dpdk_rss_hf = hash_rxq_init[i].dpdk_rss_hf;

		if (!(dpdk_rss_hf & rss_hf))
			continue;
		rss_conf = rte_realloc((*priv->rss_conf)[i],
				       (sizeof(*rss_conf) + key_len),
				       0);
		if (!rss_conf)
			return ENOMEM;
		rss_conf->rss_key = (void *)(rss_conf + 1);
		rss_conf->rss_key_len = key_len;
		rss_conf->rss_hf = dpdk_rss_hf;
		memcpy(rss_conf->rss_key, key, key_len);
		(*priv->rss_conf)[i] = rss_conf;
	}
	return 0;
}

/**
 * DPDK callback to update the RSS hash configuration.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param[in] rss_conf
 *   RSS configuration data.
 *
 * @return
 *   0 on success, negative errno value on failure.
 */
static int
mlx5_rss_hash_update(struct rte_eth_dev *dev,
		     struct rte_eth_rss_conf *rss_conf)
{
	struct priv *priv = dev->data->dev_private;
	int err = 0;

	priv_lock(priv);

	assert(priv->rss_conf != NULL);

	/* Apply configuration. */
	if (rss_conf->rss_key)
		err = rss_hash_rss_conf_new_key(priv,
						rss_conf->rss_key,
						rss_conf->rss_key_len,
						rss_conf->rss_hf);
	else
		err = rss_hash_rss_conf_new_key(priv,
						rss_hash_default_key,
						sizeof(rss_hash_default_key),
						ETH_RSS_PROTO_MASK);

	priv_unlock(priv);
	assert(err >= 0);
	return -err;
}

/**
 * DPDK callback to get the RSS hash configuration.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param[in, out] rss_conf
 *   RSS configuration data.
 *
 * @return
 *   0 on success, negative errno value on failure.
 */
static int
mlx5_rss_hash_conf_get(struct rte_eth_dev *dev,
		       struct rte_eth_rss_conf *rss_conf)
{
	struct priv *priv = dev->data->dev_private;
	struct rte_eth_rss_conf *priv_rss_conf;

	priv_lock(priv);

	assert(priv->rss_conf != NULL);

	priv_rss_conf = rss_hash_get(priv, rss_conf->rss_hf);
	if (!priv_rss_conf) {
		rss_conf->rss_hf = 0;
		priv_unlock(priv);
		return -EINVAL;
	}
	if (rss_conf->rss_key &&
	    rss_conf->rss_key_len >= priv_rss_conf->rss_key_len)
		memcpy(rss_conf->rss_key,
		       priv_rss_conf->rss_key,
		       priv_rss_conf->rss_key_len);
	rss_conf->rss_key_len = priv_rss_conf->rss_key_len;
	rss_conf->rss_hf = priv_rss_conf->rss_hf;

	priv_unlock(priv);
	return 0;
}

static const struct eth_dev_ops mlx5_dev_ops = {
	.dev_configure = mlx5_dev_configure,
	.dev_start = mlx5_dev_start,
	.dev_stop = mlx5_dev_stop,
	.dev_close = mlx5_dev_close,
	.promiscuous_enable = mlx5_promiscuous_enable,
	.promiscuous_disable = mlx5_promiscuous_disable,
	.allmulticast_enable = mlx5_allmulticast_enable,
	.allmulticast_disable = mlx5_allmulticast_disable,
	.link_update = mlx5_link_update,
	.stats_get = mlx5_stats_get,
	.stats_reset = mlx5_stats_reset,
	.queue_stats_mapping_set = NULL,
	.dev_infos_get = mlx5_dev_infos_get,
	.vlan_filter_set = mlx5_vlan_filter_set,
	.vlan_tpid_set = NULL,
	.vlan_strip_queue_set = NULL,
	.vlan_offload_set = NULL,
	.rx_queue_setup = mlx5_rx_queue_setup,
	.tx_queue_setup = mlx5_tx_queue_setup,
	.rx_queue_release = mlx5_rx_queue_release,
	.tx_queue_release = mlx5_tx_queue_release,
	.dev_led_on = NULL,
	.dev_led_off = NULL,
	.flow_ctrl_get = mlx5_dev_get_flow_ctrl,
	.flow_ctrl_set = mlx5_dev_set_flow_ctrl,
	.priority_flow_ctrl_set = NULL,
	.mac_addr_remove = mlx5_mac_addr_remove,
	.mac_addr_add = mlx5_mac_addr_add,
	.mtu_set = mlx5_dev_set_mtu,
	.udp_tunnel_add = NULL,
	.udp_tunnel_del = NULL,
	.fdir_add_signature_filter = NULL,
	.fdir_update_signature_filter = NULL,
	.fdir_remove_signature_filter = NULL,
	.fdir_add_perfect_filter = NULL,
	.fdir_update_perfect_filter = NULL,
	.fdir_remove_perfect_filter = NULL,
	.fdir_set_masks = NULL,
	.rss_hash_update = mlx5_rss_hash_update,
	.rss_hash_conf_get = mlx5_rss_hash_conf_get,
};

/**
 * Get PCI information from struct ibv_device.
 *
 * @param device
 *   Pointer to Ethernet device structure.
 * @param[out] pci_addr
 *   PCI bus address output buffer.
 *
 * @return
 *   0 on success, -1 on failure and errno is set.
 */
static int
mlx5_ibv_device_to_pci_addr(const struct ibv_device *device,
			    struct rte_pci_addr *pci_addr)
{
	FILE *file;
	char line[32];
	MKSTR(path, "%s/device/uevent", device->ibdev_path);

	file = fopen(path, "rb");
	if (file == NULL)
		return -1;
	while (fgets(line, sizeof(line), file) == line) {
		size_t len = strlen(line);
		int ret;

		/* Truncate long lines. */
		if (len == (sizeof(line) - 1))
			while (line[(len - 1)] != '\n') {
				ret = fgetc(file);
				if (ret == EOF)
					break;
				line[(len - 1)] = ret;
			}
		/* Extract information. */
		if (sscanf(line,
			   "PCI_SLOT_NAME="
			   "%" SCNx16 ":%" SCNx8 ":%" SCNx8 ".%" SCNx8 "\n",
			   &pci_addr->domain,
			   &pci_addr->bus,
			   &pci_addr->devid,
			   &pci_addr->function) == 4) {
			ret = 0;
			break;
		}
	}
	fclose(file);
	return 0;
}

/**
 * Get MAC address by querying netdevice.
 *
 * @param[in] priv
 *   struct priv for the requested device.
 * @param[out] mac
 *   MAC address output buffer.
 *
 * @return
 *   0 on success, -1 on failure and errno is set.
 */
static int
priv_get_mac(struct priv *priv, uint8_t (*mac)[ETHER_ADDR_LEN])
{
	struct ifreq request;

	if (priv_ifreq(priv, SIOCGIFHWADDR, &request))
		return -1;
	memcpy(mac, request.ifr_hwaddr.sa_data, ETHER_ADDR_LEN);
	return 0;
}

/* Support up to 32 adapters. */
static struct {
	struct rte_pci_addr pci_addr; /* associated PCI address */
	uint32_t ports; /* physical ports bitfield. */
} mlx5_dev[32];

/**
 * Get device index in mlx5_dev[] from PCI bus address.
 *
 * @param[in] pci_addr
 *   PCI bus address to look for.
 *
 * @return
 *   mlx5_dev[] index on success, -1 on failure.
 */
static int
mlx5_dev_idx(struct rte_pci_addr *pci_addr)
{
	unsigned int i;
	int ret = -1;

	assert(pci_addr != NULL);
	for (i = 0; (i != elemof(mlx5_dev)); ++i) {
		if ((mlx5_dev[i].pci_addr.domain == pci_addr->domain) &&
		    (mlx5_dev[i].pci_addr.bus == pci_addr->bus) &&
		    (mlx5_dev[i].pci_addr.devid == pci_addr->devid) &&
		    (mlx5_dev[i].pci_addr.function == pci_addr->function))
			return i;
		if ((mlx5_dev[i].ports == 0) && (ret == -1))
			ret = i;
	}
	return ret;
}

/**
 * Retrieve integer value from environment variable.
 *
 * @param[in] name
 *   Environment variable name.
 *
 * @return
 *   Integer value, 0 if the variable is not set.
 */
static int
mlx5_getenv_int(const char *name)
{
	const char *val = getenv(name);

	if (val == NULL)
		return 0;
	return atoi(val);
}

static struct eth_driver mlx5_driver;

/**
 * DPDK callback to register a PCI device.
 *
 * This function creates an Ethernet device for each port of a given
 * PCI device.
 *
 * @param[in] pci_drv
 *   PCI driver structure (mlx5_driver).
 * @param[in] pci_dev
 *   PCI device information.
 *
 * @return
 *   0 on success, negative errno value on failure.
 */
static int
mlx5_pci_devinit(struct rte_pci_driver *pci_drv, struct rte_pci_device *pci_dev)
{
	struct ibv_device **list;
	struct ibv_device *ibv_dev;
	int err = 0;
	struct ibv_context *attr_ctx = NULL;
	struct ibv_device_attr device_attr;
	unsigned int vf;
	int idx;
	int i;

	(void)pci_drv;
	assert(pci_drv == &mlx5_driver.pci_drv);
	/* Get mlx5_dev[] index. */
	idx = mlx5_dev_idx(&pci_dev->addr);
	if (idx == -1) {
		ERROR("this driver cannot support any more adapters");
		return -ENOMEM;
	}
	DEBUG("using driver device index %d", idx);

	/* Save PCI address. */
	mlx5_dev[idx].pci_addr = pci_dev->addr;
	list = ibv_get_device_list(&i);
	if (list == NULL) {
		assert(errno);
		if (errno == ENOSYS) {
			WARN("cannot list devices, is ib_uverbs loaded?");
			return 0;
		}
		return -errno;
	}
	assert(i >= 0);
	/*
	 * For each listed device, check related sysfs entry against
	 * the provided PCI ID.
	 */
	while (i != 0) {
		struct rte_pci_addr pci_addr;

		--i;
		DEBUG("checking device \"%s\"", list[i]->name);
		if (mlx5_ibv_device_to_pci_addr(list[i], &pci_addr))
			continue;
		if ((pci_dev->addr.domain != pci_addr.domain) ||
		    (pci_dev->addr.bus != pci_addr.bus) ||
		    (pci_dev->addr.devid != pci_addr.devid) ||
		    (pci_dev->addr.function != pci_addr.function))
			continue;
		vf = ((pci_dev->id.device_id ==
		       PCI_DEVICE_ID_MELLANOX_CONNECTX4VF) ||
		      (pci_dev->id.device_id ==
		       PCI_DEVICE_ID_MELLANOX_CONNECTX4LXVF));
		INFO("PCI information matches, using device \"%s\" (VF: %s)",
		     list[i]->name, (vf ? "true" : "false"));
		attr_ctx = ibv_open_device(list[i]);
		err = errno;
		break;
	}
	if (attr_ctx == NULL) {
		ibv_free_device_list(list);
		switch (err) {
		case 0:
			WARN("cannot access device, is mlx5_ib loaded?");
			return 0;
		case EINVAL:
			WARN("cannot use device, are drivers up to date?");
			return 0;
		}
		assert(err > 0);
		return -err;
	}
	ibv_dev = list[i];

	DEBUG("device opened");
	if (ibv_query_device(attr_ctx, &device_attr))
		goto error;
	INFO("%u port(s) detected", device_attr.phys_port_cnt);

	for (i = 0; i < device_attr.phys_port_cnt; i++) {
		uint32_t port = i + 1; /* ports are indexed from one */
		uint32_t test = (1 << i);
		struct ibv_context *ctx = NULL;
		struct ibv_port_attr port_attr;
		struct ibv_pd *pd = NULL;
		struct priv *priv = NULL;
		struct rte_eth_dev *eth_dev;
#ifdef HAVE_EXP_QUERY_DEVICE
		struct ibv_exp_device_attr exp_device_attr;
#endif /* HAVE_EXP_QUERY_DEVICE */
		struct ether_addr mac;

#ifdef HAVE_EXP_QUERY_DEVICE
		exp_device_attr.comp_mask =
			IBV_EXP_DEVICE_ATTR_EXP_CAP_FLAGS |
			IBV_EXP_DEVICE_ATTR_RX_HASH;
#endif /* HAVE_EXP_QUERY_DEVICE */

		DEBUG("using port %u (%08" PRIx32 ")", port, test);

		ctx = ibv_open_device(ibv_dev);
		if (ctx == NULL)
			goto port_error;

		/* Check port status. */
		err = ibv_query_port(ctx, port, &port_attr);
		if (err) {
			ERROR("port query failed: %s", strerror(err));
			goto port_error;
		}
		if (port_attr.state != IBV_PORT_ACTIVE)
			WARN("bad state for port %d: \"%s\" (%d)",
			     port, ibv_port_state_str(port_attr.state),
			     port_attr.state);

		/* Allocate protection domain. */
		pd = ibv_alloc_pd(ctx);
		if (pd == NULL) {
			ERROR("PD allocation failure");
			err = ENOMEM;
			goto port_error;
		}

		mlx5_dev[idx].ports |= test;

		/* from rte_ethdev.c */
		priv = rte_zmalloc("ethdev private structure",
				   sizeof(*priv),
				   RTE_CACHE_LINE_SIZE);
		if (priv == NULL) {
			ERROR("priv allocation failure");
			err = ENOMEM;
			goto port_error;
		}

		priv->ctx = ctx;
		priv->device_attr = device_attr;
		priv->port = port;
		priv->pd = pd;
		priv->mtu = ETHER_MTU;
#ifdef HAVE_EXP_QUERY_DEVICE
		if (ibv_exp_query_device(ctx, &exp_device_attr)) {
			ERROR("ibv_exp_query_device() failed");
			goto port_error;
		}

		priv->hw_csum =
			((exp_device_attr.exp_device_cap_flags &
			  IBV_EXP_DEVICE_RX_CSUM_TCP_UDP_PKT) &&
			 (exp_device_attr.exp_device_cap_flags &
			  IBV_EXP_DEVICE_RX_CSUM_IP_PKT));
		DEBUG("checksum offloading is %ssupported",
		      (priv->hw_csum ? "" : "not "));

		priv->hw_csum_l2tun = !!(exp_device_attr.exp_device_cap_flags &
					 IBV_EXP_DEVICE_VXLAN_SUPPORT);
		DEBUG("L2 tunnel checksum offloads are %ssupported",
		      (priv->hw_csum_l2tun ? "" : "not "));

		priv->ind_table_max_size = exp_device_attr.rx_hash_caps.max_rwq_indirection_table_size;
		DEBUG("maximum RX indirection table size is %u",
		      priv->ind_table_max_size);

#else /* HAVE_EXP_QUERY_DEVICE */
		priv->ind_table_max_size = RSS_INDIRECTION_TABLE_SIZE;
#endif /* HAVE_EXP_QUERY_DEVICE */

		(void)mlx5_getenv_int;
		priv->vf = vf;
		/* Allocate and register default RSS hash keys. */
		priv->rss_conf = rte_calloc(__func__, elemof(hash_rxq_init),
					    sizeof((*priv->rss_conf)[0]), 0);
		if (priv->rss_conf == NULL) {
			err = ENOMEM;
			goto port_error;
		}
		err = rss_hash_rss_conf_new_key(priv,
						rss_hash_default_key,
						sizeof(rss_hash_default_key),
						ETH_RSS_PROTO_MASK);
		if (err)
			goto port_error;
		/* Configure the first MAC address by default. */
		if (priv_get_mac(priv, &mac.addr_bytes)) {
			ERROR("cannot get MAC address, is mlx5_en loaded?"
			      " (errno: %s)", strerror(errno));
			goto port_error;
		}
		INFO("port %u MAC address is %02x:%02x:%02x:%02x:%02x:%02x",
		     priv->port,
		     mac.addr_bytes[0], mac.addr_bytes[1],
		     mac.addr_bytes[2], mac.addr_bytes[3],
		     mac.addr_bytes[4], mac.addr_bytes[5]);
		/* Register MAC and broadcast addresses. */
		claim_zero(priv_mac_addr_add(priv, 0,
					     (const uint8_t (*)[ETHER_ADDR_LEN])
					     mac.addr_bytes));
		claim_zero(priv_mac_addr_add(priv, 1,
					     &(const uint8_t [ETHER_ADDR_LEN])
					     { "\xff\xff\xff\xff\xff\xff" }));
#ifndef NDEBUG
		{
			char ifname[IF_NAMESIZE];

			if (priv_get_ifname(priv, &ifname) == 0)
				DEBUG("port %u ifname is \"%s\"",
				      priv->port, ifname);
			else
				DEBUG("port %u ifname is unknown", priv->port);
		}
#endif
		/* Get actual MTU if possible. */
		priv_get_mtu(priv, &priv->mtu);
		DEBUG("port %u MTU is %u", priv->port, priv->mtu);

		/* from rte_ethdev.c */
		{
			char name[RTE_ETH_NAME_MAX_LEN];

			snprintf(name, sizeof(name), "%s port %u",
				 ibv_get_device_name(ibv_dev), port);
			eth_dev = rte_eth_dev_allocate(name, RTE_ETH_DEV_PCI);
		}
		if (eth_dev == NULL) {
			ERROR("can not allocate rte ethdev");
			err = ENOMEM;
			goto port_error;
		}

		eth_dev->data->dev_private = priv;
		eth_dev->pci_dev = pci_dev;
		eth_dev->driver = &mlx5_driver;
		eth_dev->data->rx_mbuf_alloc_failed = 0;
		eth_dev->data->mtu = ETHER_MTU;

		priv->dev = eth_dev;
		eth_dev->dev_ops = &mlx5_dev_ops;
		eth_dev->data->mac_addrs = priv->mac;

		/* Bring Ethernet device up. */
		DEBUG("forcing Ethernet interface up");
		priv_set_flags(priv, ~IFF_UP, IFF_UP);
		continue;

port_error:
		rte_free(priv->rss_conf);
		rte_free(priv);
		if (pd)
			claim_zero(ibv_dealloc_pd(pd));
		if (ctx)
			claim_zero(ibv_close_device(ctx));
		break;
	}

	/*
	 * XXX if something went wrong in the loop above, there is a resource
	 * leak (ctx, pd, priv, dpdk ethdev) but we can do nothing about it as
	 * long as the dpdk does not provide a way to deallocate a ethdev and a
	 * way to enumerate the registered ethdevs to free the previous ones.
	 */

	/* no port found, complain */
	if (!mlx5_dev[idx].ports) {
		err = ENODEV;
		goto error;
	}

error:
	if (attr_ctx)
		claim_zero(ibv_close_device(attr_ctx));
	if (list)
		ibv_free_device_list(list);
	assert(err >= 0);
	return -err;
}

static const struct rte_pci_id mlx5_pci_id_map[] = {
	{
		.vendor_id = PCI_VENDOR_ID_MELLANOX,
		.device_id = PCI_DEVICE_ID_MELLANOX_CONNECTX4,
		.subsystem_vendor_id = PCI_ANY_ID,
		.subsystem_device_id = PCI_ANY_ID
	},
	{
		.vendor_id = PCI_VENDOR_ID_MELLANOX,
		.device_id = PCI_DEVICE_ID_MELLANOX_CONNECTX4VF,
		.subsystem_vendor_id = PCI_ANY_ID,
		.subsystem_device_id = PCI_ANY_ID
	},
	{
		.vendor_id = PCI_VENDOR_ID_MELLANOX,
		.device_id = PCI_DEVICE_ID_MELLANOX_CONNECTX4LX,
		.subsystem_vendor_id = PCI_ANY_ID,
		.subsystem_device_id = PCI_ANY_ID
	},
	{
		.vendor_id = PCI_VENDOR_ID_MELLANOX,
		.device_id = PCI_DEVICE_ID_MELLANOX_CONNECTX4LXVF,
		.subsystem_vendor_id = PCI_ANY_ID,
		.subsystem_device_id = PCI_ANY_ID
	},
	{
		.vendor_id = 0
	}
};

static struct eth_driver mlx5_driver = {
	.pci_drv = {
		.name = MLX5_DRIVER_NAME,
		.id_table = mlx5_pci_id_map,
		.devinit = mlx5_pci_devinit,
	},
	.dev_private_size = sizeof(struct priv)
};

/**
 * Driver initialization routine.
 */
static int
rte_mlx5_pmd_init(const char *name, const char *args)
{
	(void)name;
	(void)args;
	/*
	 * RDMAV_HUGEPAGES_SAFE tells ibv_fork_init() we intend to use
	 * huge pages. Calling ibv_fork_init() during init allows
	 * applications to use fork() safely for purposes other than
	 * using this PMD, which is not supported in forked processes.
	 */
	setenv("RDMAV_HUGEPAGES_SAFE", "1", 1);
	ibv_fork_init();
	rte_eal_pci_register(&mlx5_driver.pci_drv);
	return 0;
}

static struct rte_driver rte_mlx5_driver = {
	.type = PMD_PDEV,
	.name = MLX5_DRIVER_NAME,
	.init = rte_mlx5_pmd_init,
};

PMD_REGISTER_DRIVER(rte_mlx5_driver)
