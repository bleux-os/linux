/*
 * Copyright (C) 2017 Netronome Systems, Inc.
 *
 * This software is licensed under the GNU General License Version 2,
 * June 1991 as shown in the file COPYING in the top-level directory of this
 * source tree.
 *
 * THE COPYRIGHT HOLDERS AND/OR OTHER PARTIES PROVIDE THE PROGRAM "AS IS"
 * WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE. THE ENTIRE RISK AS TO THE QUALITY AND PERFORMANCE
 * OF THE PROGRAM IS WITH YOU. SHOULD THE PROGRAM PROVE DEFECTIVE, YOU ASSUME
 * THE COST OF ALL NECESSARY SERVICING, REPAIR OR CORRECTION.
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/ethtool.h>
#include <linux/ethtool_netlink.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/netdevice.h>
#include <linux/ptp_mock.h>
#include <linux/u64_stats_sync.h>
#include <net/devlink.h>
#include <net/udp_tunnel.h>
#include <net/xdp.h>
#include <net/macsec.h>

#define DRV_NAME	"netdevsim"

#define NSIM_XDP_MAX_MTU	4000

#define NSIM_EA(extack, msg)	NL_SET_ERR_MSG_MOD((extack), msg)

#define NSIM_IPSEC_MAX_SA_COUNT		33
#define NSIM_IPSEC_VALID		BIT(31)
#define NSIM_UDP_TUNNEL_N_PORTS		4

#define NSIM_HDS_THRESHOLD_MAX		1024

struct nsim_sa {
	struct xfrm_state *xs;
	__be32 ipaddr[4];
	u32 key[4];
	u32 salt;
	bool used;
	bool crypt;
	bool rx;
};

struct nsim_ipsec {
	struct nsim_sa sa[NSIM_IPSEC_MAX_SA_COUNT];
	struct dentry *pfile;
	u32 count;
	u32 tx;
};

#define NSIM_MACSEC_MAX_SECY_COUNT 3
#define NSIM_MACSEC_MAX_RXSC_COUNT 1
struct nsim_rxsc {
	sci_t sci;
	bool used;
};

struct nsim_secy {
	sci_t sci;
	struct nsim_rxsc nsim_rxsc[NSIM_MACSEC_MAX_RXSC_COUNT];
	u8 nsim_rxsc_count;
	bool used;
};

struct nsim_macsec {
	struct nsim_secy nsim_secy[NSIM_MACSEC_MAX_SECY_COUNT];
	u8 nsim_secy_count;
};

struct nsim_ethtool_pauseparam {
	bool rx;
	bool tx;
	bool report_stats_rx;
	bool report_stats_tx;
};

struct nsim_ethtool {
	u32 get_err;
	u32 set_err;
	u32 channels;
	struct nsim_ethtool_pauseparam pauseparam;
	struct ethtool_coalesce coalesce;
	struct ethtool_ringparam ring;
	struct ethtool_fecparam fec;
};

struct nsim_rq {
	struct napi_struct napi;
	struct sk_buff_head skb_queue;
	struct page_pool *page_pool;
	struct hrtimer napi_timer;
};

struct netdevsim {
	struct net_device *netdev;
	struct nsim_dev *nsim_dev;
	struct nsim_dev_port *nsim_dev_port;
	struct mock_phc *phc;
	struct nsim_rq **rq;

	int rq_reset_mode;

	struct nsim_bus_dev *nsim_bus_dev;

	struct bpf_prog	*bpf_offloaded;
	u32 bpf_offloaded_id;

	struct xdp_attachment_info xdp;
	struct xdp_attachment_info xdp_hw;

	bool bpf_tc_accept;
	bool bpf_tc_non_bound_accept;
	bool bpf_xdpdrv_accept;
	bool bpf_xdpoffload_accept;

	bool bpf_map_accept;
	struct nsim_ipsec ipsec;
	struct nsim_macsec macsec;
	struct {
		u32 inject_error;
		u32 __ports[2][NSIM_UDP_TUNNEL_N_PORTS];
		u32 (*ports)[NSIM_UDP_TUNNEL_N_PORTS];
		struct dentry *ddir;
		struct debugfs_u32_array dfs_ports[2];
	} udp_ports;

	struct page *page;
	struct dentry *pp_dfs;
	struct dentry *qr_dfs;

	struct nsim_ethtool ethtool;
	struct netdevsim __rcu *peer;

	struct notifier_block nb;
	struct netdev_net_notifier nn;
};

struct netdevsim *nsim_create(struct nsim_dev *nsim_dev,
			      struct nsim_dev_port *nsim_dev_port,
			      u8 perm_addr[ETH_ALEN]);
void nsim_destroy(struct netdevsim *ns);
bool netdev_is_nsim(struct net_device *dev);

void nsim_ethtool_init(struct netdevsim *ns);

void nsim_udp_tunnels_debugfs_create(struct nsim_dev *nsim_dev);
int nsim_udp_tunnels_info_create(struct nsim_dev *nsim_dev,
				 struct net_device *dev);
void nsim_udp_tunnels_info_destroy(struct net_device *dev);

#ifdef CONFIG_BPF_SYSCALL
int nsim_bpf_dev_init(struct nsim_dev *nsim_dev);
void nsim_bpf_dev_exit(struct nsim_dev *nsim_dev);
int nsim_bpf_init(struct netdevsim *ns);
void nsim_bpf_uninit(struct netdevsim *ns);
int nsim_bpf(struct net_device *dev, struct netdev_bpf *bpf);
int nsim_bpf_disable_tc(struct netdevsim *ns);
int nsim_bpf_setup_tc_block_cb(enum tc_setup_type type,
			       void *type_data, void *cb_priv);
#else

static inline int nsim_bpf_dev_init(struct nsim_dev *nsim_dev)
{
	return 0;
}

static inline void nsim_bpf_dev_exit(struct nsim_dev *nsim_dev)
{
}
static inline int nsim_bpf_init(struct netdevsim *ns)
{
	return 0;
}

static inline void nsim_bpf_uninit(struct netdevsim *ns)
{
}

static inline int nsim_bpf(struct net_device *dev, struct netdev_bpf *bpf)
{
	return -EOPNOTSUPP;
}

static inline int nsim_bpf_disable_tc(struct netdevsim *ns)
{
	return 0;
}

static inline int
nsim_bpf_setup_tc_block_cb(enum tc_setup_type type, void *type_data,
			   void *cb_priv)
{
	return -EOPNOTSUPP;
}
#endif

enum nsim_resource_id {
	NSIM_RESOURCE_NONE,   /* DEVLINK_RESOURCE_ID_PARENT_TOP */
	NSIM_RESOURCE_IPV4,
	NSIM_RESOURCE_IPV4_FIB,
	NSIM_RESOURCE_IPV4_FIB_RULES,
	NSIM_RESOURCE_IPV6,
	NSIM_RESOURCE_IPV6_FIB,
	NSIM_RESOURCE_IPV6_FIB_RULES,
	NSIM_RESOURCE_NEXTHOPS,
};

struct nsim_dev_health {
	struct devlink_health_reporter *empty_reporter;
	struct devlink_health_reporter *dummy_reporter;
	struct dentry *ddir;
	char *recovered_break_msg;
	u32 binary_len;
	bool fail_recover;
};

int nsim_dev_health_init(struct nsim_dev *nsim_dev, struct devlink *devlink);
void nsim_dev_health_exit(struct nsim_dev *nsim_dev);

struct nsim_dev_hwstats_netdev {
	struct list_head list;
	struct net_device *netdev;
	struct rtnl_hw_stats64 stats;
	bool enabled;
	bool fail_enable;
};

struct nsim_dev_hwstats {
	struct dentry *ddir;
	struct dentry *l3_ddir;

	struct mutex hwsdev_list_lock; /* protects hwsdev list(s) */
	struct list_head l3_list;

	struct notifier_block netdevice_nb;
	struct delayed_work traffic_dw;
};

int nsim_dev_hwstats_init(struct nsim_dev *nsim_dev);
void nsim_dev_hwstats_exit(struct nsim_dev *nsim_dev);

#if IS_ENABLED(CONFIG_PSAMPLE)
int nsim_dev_psample_init(struct nsim_dev *nsim_dev);
void nsim_dev_psample_exit(struct nsim_dev *nsim_dev);
#else
static inline int nsim_dev_psample_init(struct nsim_dev *nsim_dev)
{
	return 0;
}

static inline void nsim_dev_psample_exit(struct nsim_dev *nsim_dev)
{
}
#endif

enum nsim_dev_port_type {
	NSIM_DEV_PORT_TYPE_PF,
	NSIM_DEV_PORT_TYPE_VF,
};

#define NSIM_DEV_VF_PORT_INDEX_BASE 128
#define NSIM_DEV_VF_PORT_INDEX_MAX UINT_MAX

struct nsim_dev_port {
	struct list_head list;
	struct devlink_port devlink_port;
	unsigned int port_index;
	enum nsim_dev_port_type port_type;
	struct dentry *ddir;
	struct dentry *rate_parent;
	char *parent_name;
	u32 tc_bw[DEVLINK_RATE_TCS_MAX];
	struct netdevsim *ns;
};

struct nsim_vf_config {
	int link_state;
	u16 min_tx_rate;
	u16 max_tx_rate;
	u16 vlan;
	__be16 vlan_proto;
	u16 qos;
	u8 vf_mac[ETH_ALEN];
	bool spoofchk_enabled;
	bool trusted;
	bool rss_query_enabled;
};

struct nsim_dev {
	struct nsim_bus_dev *nsim_bus_dev;
	struct nsim_fib_data *fib_data;
	struct nsim_trap_data *trap_data;
	struct dentry *ddir;
	struct dentry *ports_ddir;
	struct dentry *take_snapshot;
	struct dentry *nodes_ddir;

	struct nsim_vf_config *vfconfigs;

	struct bpf_offload_dev *bpf_dev;
	bool bpf_bind_accept;
	bool bpf_bind_verifier_accept;
	u32 bpf_bind_verifier_delay;
	struct dentry *ddir_bpf_bound_progs;
	u32 prog_id_gen;
	struct list_head bpf_bound_progs;
	struct list_head bpf_bound_maps;
	struct netdev_phys_item_id switch_id;
	struct list_head port_list;
	bool fw_update_status;
	u32 fw_update_overwrite_mask;
	u32 fw_update_flash_chunk_time_ms;
	u32 max_macs;
	bool test1;
	bool dont_allow_reload;
	bool fail_reload;
	struct devlink_region *dummy_region;
	struct nsim_dev_health health;
	struct nsim_dev_hwstats hwstats;
	struct flow_action_cookie *fa_cookie;
	spinlock_t fa_cookie_lock; /* protects fa_cookie */
	bool fail_trap_group_set;
	bool fail_trap_policer_set;
	bool fail_trap_policer_counter_get;
	bool fail_trap_drop_counter_get;
	struct {
		struct udp_tunnel_nic_shared utn_shared;
		u32 __ports[2][NSIM_UDP_TUNNEL_N_PORTS];
		bool sync_all;
		bool open_only;
		bool ipv4_only;
		bool shared;
		bool static_iana_vxlan;
	} udp_ports;
	struct nsim_dev_psample *psample;
	u16 esw_mode;
};

static inline bool nsim_esw_mode_is_legacy(struct nsim_dev *nsim_dev)
{
	return nsim_dev->esw_mode == DEVLINK_ESWITCH_MODE_LEGACY;
}

static inline bool nsim_esw_mode_is_switchdev(struct nsim_dev *nsim_dev)
{
	return nsim_dev->esw_mode == DEVLINK_ESWITCH_MODE_SWITCHDEV;
}

static inline struct net *nsim_dev_net(struct nsim_dev *nsim_dev)
{
	return devlink_net(priv_to_devlink(nsim_dev));
}

int nsim_dev_init(void);
void nsim_dev_exit(void);
int nsim_drv_probe(struct nsim_bus_dev *nsim_bus_dev);
void nsim_drv_remove(struct nsim_bus_dev *nsim_bus_dev);
int nsim_drv_port_add(struct nsim_bus_dev *nsim_bus_dev,
		      enum nsim_dev_port_type type, unsigned int port_index,
		      u8 perm_addr[ETH_ALEN]);
int nsim_drv_port_del(struct nsim_bus_dev *nsim_bus_dev,
		      enum nsim_dev_port_type type,
		      unsigned int port_index);
int nsim_drv_configure_vfs(struct nsim_bus_dev *nsim_bus_dev,
			   unsigned int num_vfs);

unsigned int nsim_dev_get_vfs(struct nsim_dev *nsim_dev);

struct nsim_fib_data *nsim_fib_create(struct devlink *devlink,
				      struct netlink_ext_ack *extack);
void nsim_fib_destroy(struct devlink *devlink, struct nsim_fib_data *fib_data);
u64 nsim_fib_get_val(struct nsim_fib_data *fib_data,
		     enum nsim_resource_id res_id, bool max);

static inline bool nsim_dev_port_is_pf(struct nsim_dev_port *nsim_dev_port)
{
	return nsim_dev_port->port_type == NSIM_DEV_PORT_TYPE_PF;
}

static inline bool nsim_dev_port_is_vf(struct nsim_dev_port *nsim_dev_port)
{
	return nsim_dev_port->port_type == NSIM_DEV_PORT_TYPE_VF;
}
#if IS_ENABLED(CONFIG_XFRM_OFFLOAD)
void nsim_ipsec_init(struct netdevsim *ns);
void nsim_ipsec_teardown(struct netdevsim *ns);
bool nsim_ipsec_tx(struct netdevsim *ns, struct sk_buff *skb);
#else
static inline void nsim_ipsec_init(struct netdevsim *ns)
{
}

static inline void nsim_ipsec_teardown(struct netdevsim *ns)
{
}

static inline bool nsim_ipsec_tx(struct netdevsim *ns, struct sk_buff *skb)
{
	return true;
}
#endif

#if IS_ENABLED(CONFIG_MACSEC)
void nsim_macsec_init(struct netdevsim *ns);
void nsim_macsec_teardown(struct netdevsim *ns);
#else
static inline void nsim_macsec_init(struct netdevsim *ns)
{
}

static inline void nsim_macsec_teardown(struct netdevsim *ns)
{
}
#endif

struct nsim_bus_dev {
	struct device dev;
	struct list_head list;
	unsigned int port_count;
	unsigned int num_queues; /* Number of queues for each port on this bus */
	struct net *initial_net; /* Purpose of this is to carry net pointer
				  * during the probe time only.
				  */
	unsigned int max_vfs;
	unsigned int num_vfs;
	bool init;
};

int nsim_bus_init(void);
void nsim_bus_exit(void);
