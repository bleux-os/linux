/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_TOPOLOGY_H
#define _LINUX_SCHED_TOPOLOGY_H

#include <linux/topology.h>

#include <linux/sched/idle.h>

/*
 * sched-domains (multiprocessor balancing) declarations:
 */

/* Generate SD flag indexes */
#define SD_FLAG(name, mflags) __##name,
enum {
	#include <linux/sched/sd_flags.h>
	__SD_FLAG_CNT,
};
#undef SD_FLAG
/* Generate SD flag bits */
#define SD_FLAG(name, mflags) name = 1 << __##name,
enum {
	#include <linux/sched/sd_flags.h>
};
#undef SD_FLAG

struct sd_flag_debug {
	unsigned int meta_flags;
	char *name;
};
extern const struct sd_flag_debug sd_flag_debug[];

#ifdef CONFIG_SCHED_SMT
static inline int cpu_smt_flags(void)
{
	return SD_SHARE_CPUCAPACITY | SD_SHARE_LLC;
}
#endif

#ifdef CONFIG_SCHED_CLUSTER
static inline int cpu_cluster_flags(void)
{
	return SD_CLUSTER | SD_SHARE_LLC;
}
#endif

#ifdef CONFIG_SCHED_MC
static inline int cpu_core_flags(void)
{
	return SD_SHARE_LLC;
}
#endif

#ifdef CONFIG_NUMA
static inline int cpu_numa_flags(void)
{
	return SD_NUMA;
}
#endif

extern int arch_asym_cpu_priority(int cpu);

struct sched_domain_attr {
	int relax_domain_level;
};

#define SD_ATTR_INIT	(struct sched_domain_attr) {	\
	.relax_domain_level = -1,			\
}

extern int sched_domain_level_max;

struct sched_group;

struct sched_domain_shared {
	atomic_t	ref;
	atomic_t	nr_busy_cpus;
	int		has_idle_cores;
	int		nr_idle_scan;
};

struct sched_domain {
	/* These fields must be setup */
	struct sched_domain __rcu *parent;	/* top domain must be null terminated */
	struct sched_domain __rcu *child;	/* bottom domain must be null terminated */
	struct sched_group *groups;	/* the balancing groups of the domain */
	unsigned long min_interval;	/* Minimum balance interval ms */
	unsigned long max_interval;	/* Maximum balance interval ms */
	unsigned int busy_factor;	/* less balancing by factor if busy */
	unsigned int imbalance_pct;	/* No balance until over watermark */
	unsigned int cache_nice_tries;	/* Leave cache hot tasks for # tries */
	unsigned int imb_numa_nr;	/* Nr running tasks that allows a NUMA imbalance */

	int nohz_idle;			/* NOHZ IDLE status */
	int flags;			/* See SD_* */
	int level;

	/* Runtime fields. */
	unsigned long last_balance;	/* init to jiffies. units in jiffies */
	unsigned int balance_interval;	/* initialise to 1. units in ms. */
	unsigned int nr_balance_failed; /* initialise to 0 */

	/* idle_balance() stats */
	u64 max_newidle_lb_cost;
	unsigned long last_decay_max_lb_cost;

#ifdef CONFIG_SCHEDSTATS
	/* sched_balance_rq() stats */
	unsigned int lb_count[CPU_MAX_IDLE_TYPES];
	unsigned int lb_failed[CPU_MAX_IDLE_TYPES];
	unsigned int lb_balanced[CPU_MAX_IDLE_TYPES];
	unsigned int lb_imbalance_load[CPU_MAX_IDLE_TYPES];
	unsigned int lb_imbalance_util[CPU_MAX_IDLE_TYPES];
	unsigned int lb_imbalance_task[CPU_MAX_IDLE_TYPES];
	unsigned int lb_imbalance_misfit[CPU_MAX_IDLE_TYPES];
	unsigned int lb_gained[CPU_MAX_IDLE_TYPES];
	unsigned int lb_hot_gained[CPU_MAX_IDLE_TYPES];
	unsigned int lb_nobusyg[CPU_MAX_IDLE_TYPES];
	unsigned int lb_nobusyq[CPU_MAX_IDLE_TYPES];

	/* Active load balancing */
	unsigned int alb_count;
	unsigned int alb_failed;
	unsigned int alb_pushed;

	/* SD_BALANCE_EXEC stats */
	unsigned int sbe_count;
	unsigned int sbe_balanced;
	unsigned int sbe_pushed;

	/* SD_BALANCE_FORK stats */
	unsigned int sbf_count;
	unsigned int sbf_balanced;
	unsigned int sbf_pushed;

	/* try_to_wake_up() stats */
	unsigned int ttwu_wake_remote;
	unsigned int ttwu_move_affine;
	unsigned int ttwu_move_balance;
#endif
	char *name;
	union {
		void *private;		/* used during construction */
		struct rcu_head rcu;	/* used during destruction */
	};
	struct sched_domain_shared *shared;

	unsigned int span_weight;
	/*
	 * Span of all CPUs in this domain.
	 *
	 * NOTE: this field is variable length. (Allocated dynamically
	 * by attaching extra space to the end of the structure,
	 * depending on how many CPUs the kernel has booted up with)
	 */
	unsigned long span[];
};

static inline struct cpumask *sched_domain_span(struct sched_domain *sd)
{
	return to_cpumask(sd->span);
}

extern void partition_sched_domains(int ndoms_new, cpumask_var_t doms_new[],
				    struct sched_domain_attr *dattr_new);

/* Allocate an array of sched domains, for partition_sched_domains(). */
cpumask_var_t *alloc_sched_domains(unsigned int ndoms);
void free_sched_domains(cpumask_var_t doms[], unsigned int ndoms);

bool cpus_equal_capacity(int this_cpu, int that_cpu);
bool cpus_share_cache(int this_cpu, int that_cpu);
bool cpus_share_resources(int this_cpu, int that_cpu);

typedef const struct cpumask *(*sched_domain_mask_f)(int cpu);
typedef int (*sched_domain_flags_f)(void);

struct sd_data {
	struct sched_domain *__percpu *sd;
	struct sched_domain_shared *__percpu *sds;
	struct sched_group *__percpu *sg;
	struct sched_group_capacity *__percpu *sgc;
};

struct sched_domain_topology_level {
	sched_domain_mask_f mask;
	sched_domain_flags_f sd_flags;
	int		    numa_level;
	struct sd_data      data;
	char                *name;
};

extern void __init set_sched_topology(struct sched_domain_topology_level *tl);
extern void sched_update_asym_prefer_cpu(int cpu, int old_prio, int new_prio);

#define SDTL_INIT(maskfn, flagsfn, dname) ((struct sched_domain_topology_level) \
	    { .mask = maskfn, .sd_flags = flagsfn, .name = #dname })

#if defined(CONFIG_ENERGY_MODEL) && defined(CONFIG_CPU_FREQ_GOV_SCHEDUTIL)
extern void rebuild_sched_domains_energy(void);
#else
static inline void rebuild_sched_domains_energy(void)
{
}
#endif

#ifndef arch_scale_cpu_capacity
/**
 * arch_scale_cpu_capacity - get the capacity scale factor of a given CPU.
 * @cpu: the CPU in question.
 *
 * Return: the CPU scale factor normalized against SCHED_CAPACITY_SCALE, i.e.
 *
 *             max_perf(cpu)
 *      ----------------------------- * SCHED_CAPACITY_SCALE
 *      max(max_perf(c) : c \in CPUs)
 */
static __always_inline
unsigned long arch_scale_cpu_capacity(int cpu)
{
	return SCHED_CAPACITY_SCALE;
}
#endif

#ifndef arch_scale_hw_pressure
static __always_inline
unsigned long arch_scale_hw_pressure(int cpu)
{
	return 0;
}
#endif

#ifndef arch_update_hw_pressure
static __always_inline
void arch_update_hw_pressure(const struct cpumask *cpus,
				  unsigned long capped_frequency)
{ }
#endif

#ifndef arch_scale_freq_ref
static __always_inline
unsigned int arch_scale_freq_ref(int cpu)
{
	return 0;
}
#endif

static inline int task_node(const struct task_struct *p)
{
	return cpu_to_node(task_cpu(p));
}

#endif /* _LINUX_SCHED_TOPOLOGY_H */
