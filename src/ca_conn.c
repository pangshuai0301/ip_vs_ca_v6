/*
 * ca_conn.c
 * Copyright (C) 2016 yubo@yubo.org
 * 2016-02-14
 */
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/proc_fs.h>	/* for proc_net_* */
#include <linux/seq_file.h>
#include <linux/jhash.h>
#include <linux/random.h>
#include <net/net_namespace.h>
#include "ca.h"

static struct list_head *ip_vs_ca_conn_tab_s;
static struct list_head *ip_vs_ca_conn_tab_c;

/*  SLAB cache for IP_VS_CA connections */
static struct kmem_cache *ip_vs_ca_conn_cachep __read_mostly;

/*  counter for current IP_VS_CA connections */
static atomic_t ip_vs_ca_conn_count = ATOMIC_INIT(0);

/* random value for IP_VS_CA connection hash */
static unsigned int ip_vs_ca_conn_rnd;

static int ip_vs_ca_conn_tab_bits = IP_VS_CA_CONN_TAB_BITS;
static int ip_vs_ca_conn_tab_size = 1 << IP_VS_CA_CONN_TAB_BITS;
static int ip_vs_ca_conn_tab_mask = IP_VS_CA_CONN_TAB_SIZE - 1;

module_param(ip_vs_ca_conn_tab_bits, int, 0660);

/*
 *  Fine locking granularity for big connection hash table
 */
#define CT_LOCKARRAY_BITS  16
#define CT_LOCKARRAY_SIZE  (1<<CT_LOCKARRAY_BITS)
#define CT_LOCKARRAY_MASK  (CT_LOCKARRAY_SIZE-1)

static int ct_lockarray_bits = CT_LOCKARRAY_BITS;
static int ct_lockarray_size = 1 << CT_LOCKARRAY_BITS;
static int ct_lockarray_mask = CT_LOCKARRAY_SIZE - 1;

struct ip_vs_ca_aligned_lock {
    rwlock_t l;
} __attribute__ ((__aligned__(SMP_CACHE_BYTES)));

/* lock array for conn table */
static struct ip_vs_ca_aligned_lock *__ip_vs_ca_conntbl_lock_array_s;

static struct ip_vs_ca_aligned_lock *__ip_vs_ca_conntbl_lock_array_c;

static inline void ct_read_lock_bh_s(unsigned key)
{
        read_lock_bh(&__ip_vs_ca_conntbl_lock_array_s[key & ct_lockarray_mask].l);
}

static inline void ct_read_unlock_bh_s(unsigned key)
{
        read_unlock_bh(&__ip_vs_ca_conntbl_lock_array_s[key & ct_lockarray_mask].l);
}

static inline void ct_write_lock_bh_s(unsigned key)
{
        write_lock_bh(&__ip_vs_ca_conntbl_lock_array_s[key & ct_lockarray_mask].l);
}

static inline void ct_write_unlock_bh_s(unsigned key)
{
        write_unlock_bh(&__ip_vs_ca_conntbl_lock_array_s[key & ct_lockarray_mask].l);
}

static inline void ct_read_lock_bh_c(unsigned key)
{
        read_lock_bh(&__ip_vs_ca_conntbl_lock_array_c[key & ct_lockarray_mask].l);
}

static inline void ct_read_unlock_bh_c(unsigned key)
{
        read_unlock_bh(&__ip_vs_ca_conntbl_lock_array_c[key & ct_lockarray_mask].l);
}

static inline void ct_write_lock_bh_c(unsigned key)
{
        write_lock_bh(&__ip_vs_ca_conntbl_lock_array_c[key & ct_lockarray_mask].l);
}

static inline void ct_write_unlock_bh_c(unsigned key)
{
        write_unlock_bh(&__ip_vs_ca_conntbl_lock_array_c[key & ct_lockarray_mask].l);
}

/*
 *  Returns hash value for IPVS connection entry
 */
static unsigned int ip_vs_ca_conn_hashkey(int af, unsigned proto,
        const union nf_inet_addr *addr, __be16 port) {
#ifdef CONFIG_IP_VS_CA_IPV6
    if (af == AF_INET6)
        return jhash_3words(
                    jhash(addr, 16, ip_vs_ca_conn_rnd),
                    (__force u32) port, proto,
                    ip_vs_ca_conn_rnd) & ip_vs_ca_conn_tab_mask;
#endif
        return jhash_3words((__force u32) addr->ip, (__force u32) port, proto,
                    ip_vs_ca_conn_rnd) & ip_vs_ca_conn_tab_mask;
}

/*
 * Lock two buckets of ip_vs_ca_conn_tab
 */
static inline void ct_lock2(unsigned shash, unsigned chash) {
    unsigned slock, clock;

    slock = shash & ct_lockarray_mask;
    clock = chash & ct_lockarray_mask;

    /* lock the conntab bucket */
    if (slock < clock) {
        ct_write_lock_bh_s(shash);
        ct_write_lock_bh_c(chash);
    } else if (slock > clock) {
        ct_write_lock_bh_c(chash);
        ct_write_lock_bh_s(shash);
    } else {
        ct_write_lock_bh_s(shash);
    }
}

/*
 * Unlock two buckets of ip_vs_ca_conn_tab
 */
static inline void ct_unlock2(unsigned shash, unsigned chash) {
    unsigned slock, clock;

    slock = shash & ct_lockarray_mask;
    clock = chash & ct_lockarray_mask;

    /* lock the conntab bucket */
    if (slock < clock) {
        ct_write_unlock_bh_c(chash);
        ct_write_unlock_bh_s(shash);
    } else if (slock > clock) {
        ct_write_unlock_bh_s(shash);
        ct_write_unlock_bh_c(chash);
    } else {
        ct_write_unlock_bh_c(shash);
    }
}

/*
 *  Hashed ip_vs_ca_conn into ip_vs_ca_conn_tab
 *  returns bool success.
 */
static inline int __ip_vs_ca_conn_hash(struct ip_vs_ca_conn *cp, unsigned shash,
        unsigned chash) {
    int ret;

    if (!(cp->flags & IP_VS_CA_CONN_F_HASHED)) {
        list_add(&cp->s_list, &ip_vs_ca_conn_tab_s[shash]);
        list_add(&cp->c_list, &ip_vs_ca_conn_tab_c[chash]);
        cp->flags |= IP_VS_CA_CONN_F_HASHED;
        atomic_inc(&cp->refcnt);
        ret = 1;
    } else {
        IP_VS_CA_ERR("request for already hashed, called from %pF\n",
               __builtin_return_address(0));
        ret = 0;
    }

    return ret;
}

/*
 *  Hashed ip_vs_ca_conn in two buckets of ip_vs_ca_conn_tab
 *  by caddr/cport/vaddr/vport and raddr/rport/laddr/lport,
 *  returns bool success.
 */
static int
ip_vs_ca_conn_hash(struct ip_vs_ca_conn *cp) {
    unsigned shash, chash;
    int ret;

    shash = ip_vs_ca_conn_hashkey(cp->af, cp->protocol,
            &cp->s_addr, cp->s_port);
    chash = ip_vs_ca_conn_hashkey(cp->af, cp->protocol,
            &cp->c_addr, cp->c_port);

    ct_lock2(shash, chash);

    ret = __ip_vs_ca_conn_hash(cp, shash, chash);

    ct_unlock2(shash, chash);

    return ret;
}

/*
 *  UNhashes ip_vs_ca_conn from ip_vs_ca_conn_tab.
 *  cp->refcnt must be equal 2,
 *  returns bool success.
 */
static int
ip_vs_ca_conn_unhash(struct ip_vs_ca_conn *cp) {
    unsigned shash, chash;
    int ret;

    shash = ip_vs_ca_conn_hashkey(cp->af, cp->protocol,
            &cp->s_addr, cp->s_port);
    chash = ip_vs_ca_conn_hashkey(cp->af, cp->protocol,
            &cp->c_addr, cp->c_port);

    /* locked */
    ct_lock2(shash, chash);

    /* unhashed */
    if ((cp->flags & IP_VS_CA_CONN_F_HASHED) && (atomic_read(&cp->refcnt) == 2)) {
        list_del(&cp->s_list);
        list_del(&cp->c_list);
        cp->flags &= ~IP_VS_CA_CONN_F_HASHED;
        atomic_dec(&cp->refcnt);
        ret = 1;
    } else {
        ret = 0;
    }

    ct_unlock2(shash, chash);

    return ret;
}

static void ip_vs_ca_conn_expire(unsigned long data) {
    struct ip_vs_ca_conn *cp = (struct ip_vs_ca_conn *)data;

    /*
     * Set proper timeout.
     */
    cp->timeout = 60 * HZ;

    /*
     *      hey, I'm using it
     */
    atomic_inc(&cp->refcnt);

    /*
     *      unhash it if it is hashed in the conn table
     */
    if (!ip_vs_ca_conn_unhash(cp))
        goto expire_later;

    /*
     *      refcnt==1 implies I'm the only one referrer
     */
    if (likely(atomic_read(&cp->refcnt) == 1)) {
        /* delete the timer if it is activated by other users */
        if (timer_pending(&cp->timer))
            del_timer(&cp->timer);

        atomic_dec(&ip_vs_ca_conn_count);
        IP_VS_CA_INC_STATS(ext_stats, CONN_DEL_CNT);

        if (cp->af == AF_INET) {
            IP_VS_CA_DBG("conn expire: %pI4:%d(%pI4:%d) -> %pI4:%d timer:%p\n",
                    &cp->s_addr.ip, ntohs(cp->s_port),
                    &cp->c_addr.ip, ntohs(cp->c_port),
                    &cp->d_addr.ip, ntohs(cp->d_port),
                    &cp->timer);
            kmem_cache_free(ip_vs_ca_conn_cachep, cp);
            return;
        }

#ifdef CONFIG_IP_VS_CA_IPV6
        if (cp->af == AF_INET6) {
            IP_VS_CA_DBG("conn expire: %pI6:%d(%pI6:%d) -> %pI6:%d timer:%p\n",
                    &cp->s_addr.in6, ntohs(cp->s_port),
                    &cp->c_addr.in6, ntohs(cp->c_port),
                    &cp->d_addr.in6, ntohs(cp->d_port),
                    &cp->timer);
            kmem_cache_free(ip_vs_ca_conn_cachep, cp);
            return;
        }
#endif
    }

    /* hash it back to the table */
    ip_vs_ca_conn_hash(cp);

expire_later:
    IP_VS_CA_DBG("delayed: conn->refcnt-1=%d\n", atomic_read(&cp->refcnt) - 1);

    ip_vs_ca_conn_put(cp);
}

struct ip_vs_ca_conn *
ip_vs_ca_conn_new(int af, struct ip_vs_ca_protocol *pp,
                  const union nf_inet_addr *saddr, __be16 sport,
                  const union nf_inet_addr *daddr, __be16 dport,
                  const union nf_inet_addr *caddr, __be16 cport,
                  struct sk_buff *skb) {
    struct ip_vs_ca_conn *cp;

    //EnterFunction();

    cp = kmem_cache_zalloc(ip_vs_ca_conn_cachep, GFP_ATOMIC);
    if (cp == NULL) {
        IP_VS_CA_ERR("%s(): no memory\n", __func__);
        return NULL;
    }

    /* now init connection */
    IP_VS_CA_DBG("setup_timer, %p\n", &cp->timer);
    setup_timer(&cp->timer, ip_vs_ca_conn_expire, (unsigned long)cp);
    cp->af = af;
    cp->protocol = pp->protocol;

    ip_vs_ca_addr_copy(af, &cp->s_addr, saddr);
    cp->s_port = sport;

    ip_vs_ca_addr_copy(af, &cp->c_addr, caddr);
    cp->c_port = cport;

    ip_vs_ca_addr_copy(af, &cp->d_addr, daddr);
    cp->d_port = dport;

    cp->flags = 0;

    spin_lock_init(&cp->lock);
    atomic_set(&cp->refcnt, 1);
    atomic_inc(&ip_vs_ca_conn_count);
    IP_VS_CA_INC_STATS(ext_stats, CONN_NEW_CNT);

    cp->state = 0;
    cp->timeout = *pp->timeout;

    ip_vs_ca_conn_hash(cp);

#ifdef CONFIG_IP_VS_CA_IPV6
    if (af == AF_INET6)
        IP_VS_CA_DBG("conn new: proto:%u, %pI6:%d(%pI6:%d) -> %pI6:%d\n",
                cp->protocol,
                &cp->s_addr.ip, ntohs(cp->s_port),
                &cp->c_addr.ip, ntohs(cp->c_port),
                &cp->d_addr.ip, ntohs(cp->d_port));
    else
#endif
        IP_VS_CA_DBG("conn new: proto:%u, %pI4:%d(%pI4:%d) -> %pI4:%d\n",
        cp->protocol,
        &cp->s_addr.ip, ntohs(cp->s_port),
        &cp->c_addr.ip, ntohs(cp->c_port),
        &cp->d_addr.ip, ntohs(cp->d_port));
    //LeaveFunction();
    return cp;
}

/*
 * support ipv4 and ipv6
 */
struct ip_vs_ca_conn *
ip_vs_ca_conn_get(int af, __u8 protocol, const union nf_inet_addr *addr,
                  __be16 port, int dir) {
    unsigned hash;
    struct ip_vs_ca_conn *cp;

    hash = ip_vs_ca_conn_hashkey(af, protocol, addr, port);

    if (dir == IP_VS_CA_IN) {
	ct_read_lock_bh_s(hash);
        list_for_each_entry(cp, &ip_vs_ca_conn_tab_s[hash], s_list) {
            if (cp->af == af &&
                ip_vs_ca_addr_equal(af, addr, &cp->s_addr) &&
                port == cp->s_port &&
                protocol == cp->protocol) {
                    /* HIT */
                    atomic_inc(&cp->refcnt);
		    ct_read_unlock_bh_s(hash);
                    return cp;
    	    }
    	}
	ct_read_unlock_bh_s(hash);
    } else {
	ct_read_lock_bh_c(hash);
        list_for_each_entry(cp, &ip_vs_ca_conn_tab_c[hash], c_list) {
            if (cp->af == af &&
                ip_vs_ca_addr_equal(af, addr, &cp->c_addr) &&
                port == cp->c_port &&
                protocol == cp->protocol) {
                    /* HIT */
                    atomic_inc(&cp->refcnt);
		    ct_read_unlock_bh_c(hash);
                    return cp;
            }
        }
	ct_read_unlock_bh_c(hash);
    }
    return NULL;
}

void ip_vs_ca_conn_put(struct ip_vs_ca_conn *cp) {
    /* reset it expire in its timeout */
    /* IP_VS_CA_DBG("mod_timer %lu\n", cp->timeout / HZ); */
    mod_timer(&cp->timer, jiffies + cp->timeout);
    __ip_vs_ca_conn_put(cp);
}

static void ip_vs_ca_conn_expire_now(struct ip_vs_ca_conn *cp) {
    IP_VS_CA_DBG("expire_now: timer(%p)\n", &cp->timer);
    if (del_timer(&cp->timer))
        mod_timer(&cp->timer, jiffies);
}

/*
 *      Flush all the connection entries in the ip_vs_ca_conn_tab
 */
static void ip_vs_ca_conn_flush(void) {
    int idx;
    struct ip_vs_ca_conn *cp;

flush_again:
    for (idx = 0; idx < ip_vs_ca_conn_tab_size; idx++) {
        /*
         *  Lock is actually needed in this loop.
         */
        ct_write_lock_bh_s(idx);

        list_for_each_entry(cp, &ip_vs_ca_conn_tab_s[idx], s_list) {
            IP_VS_CA_DBG("del connection\n");
            ip_vs_ca_conn_expire_now(cp);
        }
        ct_write_unlock_bh_s(idx);
    }

    /* the counter may be not NULL, because maybe some conn entries
       are run by slow timer handler or unhashed but still referred */
    if (atomic_read(&ip_vs_ca_conn_count) != 0) {
        schedule();
        goto flush_again;
    }
}

static void conn_tab_size_init(void) {
        ip_vs_ca_conn_tab_size = 1 << ip_vs_ca_conn_tab_bits;
        ip_vs_ca_conn_tab_mask = ip_vs_ca_conn_tab_size - 1;

        ct_lockarray_bits = ip_vs_ca_conn_tab_bits;
        ct_lockarray_size = 1 << ct_lockarray_bits;
        ct_lockarray_mask = ct_lockarray_size - 1;
}

int __init ip_vs_ca_conn_init(void) {
    int idx;

    conn_tab_size_init();

    ip_vs_ca_conn_tab_s = vmalloc(ip_vs_ca_conn_tab_size *
            (sizeof(struct list_head)));
    if (!ip_vs_ca_conn_tab_s)
        return -ENOMEM;

    ip_vs_ca_conn_tab_c = vmalloc(ip_vs_ca_conn_tab_size *
            (sizeof(struct list_head)));
    if (!ip_vs_ca_conn_tab_c)
        return -ENOMEM;

    __ip_vs_ca_conntbl_lock_array_s = vmalloc(ct_lockarray_size *
            (sizeof(struct ip_vs_ca_aligned_lock)));
    if (!__ip_vs_ca_conntbl_lock_array_s)
        return -ENOMEM;
    __ip_vs_ca_conntbl_lock_array_c = vmalloc(ct_lockarray_size *
            (sizeof(struct ip_vs_ca_aligned_lock)));
    if (!__ip_vs_ca_conntbl_lock_array_c)
        return -ENOMEM;

    /* Allocate ip_vs_ca_conn slab cache */
    ip_vs_ca_conn_cachep = kmem_cache_create("ip_vs_ca_conn",
                            sizeof(struct ip_vs_ca_conn),
                            0, SLAB_HWCACHE_ALIGN, NULL);
    if (!ip_vs_ca_conn_cachep) {
        vfree(ip_vs_ca_conn_tab_s);
        vfree(ip_vs_ca_conn_tab_c);
        return -ENOMEM;
    }

    IP_VS_CA_INFO("Connection hash table configured "
        "(size=%d, memory=%ldKbytes)\n",
        ip_vs_ca_conn_tab_size,
        (long)(ip_vs_ca_conn_tab_size * sizeof(struct list_head)) / 1024);

    IP_VS_CA_DBG("Each connection entry needs %Zd bytes at least\n",
          sizeof(struct ip_vs_ca_conn));

    for (idx = 0; idx < ip_vs_ca_conn_tab_size; idx++) {
        INIT_LIST_HEAD(&ip_vs_ca_conn_tab_s[idx]);
        INIT_LIST_HEAD(&ip_vs_ca_conn_tab_c[idx]);
    }

    for (idx = 0; idx < ct_lockarray_size; idx++) {
        rwlock_init(&__ip_vs_ca_conntbl_lock_array_s[idx].l);
        rwlock_init(&__ip_vs_ca_conntbl_lock_array_c[idx].l);
    }
    IP_VS_CA_INFO("Connection table lock array configured (size=%d)\n", ct_lockarray_size);

    /* calculate the random value for connection hash */
    get_random_bytes(&ip_vs_ca_conn_rnd, sizeof(ip_vs_ca_conn_rnd));

    return 0;
}

void ip_vs_ca_conn_cleanup(void) {
    ip_vs_ca_conn_flush();
    kmem_cache_destroy(ip_vs_ca_conn_cachep);
    vfree(ip_vs_ca_conn_tab_s);
    vfree(ip_vs_ca_conn_tab_c);
    vfree(__ip_vs_ca_conntbl_lock_array_s);
    vfree(__ip_vs_ca_conntbl_lock_array_c);
}
