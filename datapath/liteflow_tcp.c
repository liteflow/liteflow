// LiteFlow TCP Kernel will register to both LiteFlow kernek and Kernel TCP Congestion Control
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/net.h>
#include <net/tcp.h>
#include <linux/inet_diag.h>
#include <net/genetlink.h>

#include "linux/liteflow.h"
#include "liteflow_tcp.h"

#define S_TO_US 1000000

struct lf_tcp_metric {
    s64 values[NUM_OF_INPUT_METRICS];
};

struct lf_tcp_internal {
    u64 src_ip;
    u64 dest_ip;
    u64 src_port;
    u64 dest_port;
    u64 last_snd_una;
    u32 current_pointer;
    s64 global_stats[NUM_OF_GLOBAL_STATS];
    struct lf_tcp_metric* metrics; // ring buffer
};

static inline void lf_increase_pointer(struct lf_tcp_internal *lf_tcp) {
    u32 current_pointer;

    current_pointer = lf_tcp->current_pointer;
    lf_tcp->current_pointer = (++current_pointer) % HISTORY_LEN;
}

// Set rate towards connection
static inline void lf_set_rate (struct sock *sk, u32 rate) {
    sk->sk_pacing_rate = rate;
}

static int rate_sample_valid(const struct rate_sample *rs) {
    int ret = 0;
    if (rs->delivered <= 0)
    ret |= 1;
    if (rs->interval_us <= 0)
    ret |= 1 << 1;
    if (rs->rtt_us <= 0)
    ret |= 1 << 2;
    return ret;
}

static struct genl_multicast_group lf_tcp_mcgrps[] = {
    [LF_TCP_NL_MC_DEFAULT] = { .name = LF_TCP_NL_MC_DEFAULT_NAME, },
};

static struct genl_family lf_tcp_gnl_family = {
    .name = LF_TCP_NL_NAME,     
    .version = LF_TCP_NL_VERSION,
    .netnsok = false,
    .maxattr = LF_TCP_NL_ATTR_MAX,
    .mcgrps = lf_tcp_mcgrps,
    .n_mcgrps = ARRAY_SIZE(lf_tcp_mcgrps),
    .module = THIS_MODULE,
};

static inline int report_to_user(s64 *nn_input, u32 input_size) {
    struct sk_buff *skb;
    void *msg_head;
    int ret;

    skb = genlmsg_new(NLMSG_GOODSIZE, GFP_ATOMIC);

    if (skb == NULL) {
        printk (KERN_ERR "Cannot allocate skb for LiteFlow TCP netlink ...\n");
        return LF_ERROR;
    }

    msg_head = genlmsg_put(skb, 0, 0, &lf_tcp_gnl_family, GFP_ATOMIC, LF_TCP_NL_C_REPORT);
    if (msg_head == NULL) {
        printk (KERN_ERR "Cannot allocate msg_head for LiteFlow TCP netlink ...\n");
        return LF_ERROR;
    }

    ret = nla_put(skb, LF_TCP_NL_ATTR_NN_INPUT, input_size * sizeof(s64), nn_input);
    if (ret != 0) {
        printk (KERN_ERR "Cannot put data for LiteFlow TCP netlink, error code: %d ...\n", ret);
        return LF_ERROR;
    }

    genlmsg_end(skb, msg_head);

    genlmsg_multicast(&lf_tcp_gnl_family, skb, 0, LF_TCP_NL_MC_DEFAULT, GFP_ATOMIC);

    return LF_SUCCS;
}

// kernel version == 4.14.0
static inline int load_metric(struct lf_tcp_internal *ca, struct tcp_sock *tp, const struct rate_sample *rs) {
    
    struct lf_tcp_metric* metric;
    u64 rin = 0, rout = 0;
    u64 ack_us = 0, snd_us = 0;
    int i;
    
    int measured_valid_rate = rate_sample_valid(rs);
    if (measured_valid_rate != 0) {
        return LF_ERROR;
    }
        
    if (rs->rtt_us > 0
        && (ca->global_stats[GLOBAL_STATS_POS_MIN_RTT_US] == -1 
        || ca->global_stats[GLOBAL_STATS_POS_MIN_RTT_US] > rs->rtt_us))
    {
        ca->global_stats[GLOBAL_STATS_POS_MIN_RTT_US] = rs->rtt_us;
    }
    
    metric = &(ca->metrics[ca->current_pointer]);

    metric->values[INPUT_METRICS_POS_SENT_LAT_INFLACTION] = 0;
    if (ca->global_stats[GLOBAL_STATS_POS_MIN_RTT_US] > 0) {
        metric->values[INPUT_METRICS_POS_LAT_RATIO] = INPUT_SCALE * rs->rtt_us; 
        do_div(metric->values[INPUT_METRICS_POS_LAT_RATIO], ca->global_stats[GLOBAL_STATS_POS_MIN_RTT_US]);
    }
    else {
        metric->values[INPUT_METRICS_POS_LAT_RATIO] = INPUT_SCALE * 1;
    }
    

    ack_us = tcp_stamp_us_delta(tp->tcp_mstamp, rs->prior_mstamp);
    for (i = 0; i < MAX_SKB_STORED; ++i) {
        if (ca->skb_array[i].first_tx_mstamp == tp->first_tx_mstamp) {
            snd_us = ca->skb_array[i].interval_us;
            break;
        }
    }

    if (ack_us != 0 && snd_us != 0) {
        rin = rout = (s64) rs->delivered * tp->mss_cache * S_TO_US;
        do_div(rin, snd_us);
        do_div(rout, ack_us);
        metric->values[INPUT_METRICS_POS_SNED_RATIO] = INPUT_SCALE * rin;
        do_div(metric->values[INPUT_METRICS_POS_SNED_RATIO], rout);
    }
    else {
        metric->values[INPUT_METRICS_POS_SNED_RATIO] = INPUT_SCALE * 1;
    }

    return LF_SUCCS;
}

static void lf_tcp_conn_init(struct sock *sk) {
    struct lf_tcp_internal *ca;
    struct tcp_sock *tp;

    printk(KERN_INFO "New flow handled by liteflow tcp kernel inits...\n");
    ca = inet_csk_ca(sk);
    tp = tcp_sk(sk);

    ca->src_ip = tp->inet_conn.icsk_inet.inet_saddr;
    ca->src_port = tp->inet_conn.icsk_inet.inet_sport;
    ca->dest_ip = tp->inet_conn.icsk_inet.inet_daddr;
    ca->dest_port = tp->inet_conn.icsk_inet.inet_dport;
    ca->last_snd_una = tp->snd_una;
    ca->global_stats[GLOBAL_STATS_POS_MIN_RTT_US] = -1;

    ca->current_pointer = 0;
    ca->metrics = kmalloc(sizeof(struct lf_tcp_metric) * HISTORY_LEN, GFP_KERNEL);
    memset(ca->metrics, 0, sizeof(struct lf_tcp_metric) * HISTORY_LEN);

    if (!(tp->ecn_flags & TCP_ECN_OK)) {
        INET_ECN_dontxmit(sk);
    }
    // Turn on pacing, so we can use pacing to control the speed
    // Learn from BBR and CCP :)
    cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
}

static void lf_tcp_conn_release(struct sock *sk)
{
    struct lf_tcp_internal *ca;

    printk(KERN_INFO "Free flow handled by liteflow tcp kernel...\n");  

    ca = inet_csk_ca(sk);
    if (ca->metrics != NULL) {
        kfree(ca->metrics);
    }
}

// Learn from CCP
static u32 lf_tcp_conn_ssthresh(struct sock *sk) 
{
    const struct tcp_sock *tp = tcp_sk(sk);
    return max(tp->snd_cwnd >> 1U, 2U);
}

// Learn from CCP
static u32 lf_tcp_conn_undo_cwnd(struct sock *sk) 
{
    const struct tcp_sock *tp = tcp_sk(sk);
    return max(tp->snd_cwnd, tp->snd_ssthresh << 1);
}

// Send all metrics to liteflow hosted NN
// and set sending rate based on returned rate
static void lf_tcp_conn_nn_control(struct sock *sk, const struct rate_sample *rs) 
{
    int ret;
    int global_stats_pos, metric_pos, value_pos, pos = 0;
    u32 output_rate;
    struct lf_tcp_internal *ca;
    struct tcp_sock *tp;
    s64 nn_input[INPUT_SIZE];
    s64 nn_output[NUM_OF_OUTPUT_VALUE];
    
    ca = inet_csk_ca(sk);
    if(ca->metrics == NULL) {
        printk(KERN_ERR "Current flow is not managed by liteflow tcp kernel!\n");
        return;
    }

    tp = tcp_sk(sk);
    ret = load_metric(ca, tp, rs);
    if(ret == LF_ERROR) {
        return;
    }
    
    // prepare input vector
    // TODO performance is not good
    // for (global_stats_pos = 0; global_stats_pos < NUM_OF_GLOBAL_STATS; ++global_stats_pos) {
    //     nn_input[pos] = ca->global_stats[global_stats_pos];
    //     pos++;
    // }

    for (metric_pos = 0; metric_pos < HISTORY_LEN; ++metric_pos) {
        for (value_pos = 0; value_pos < NUM_OF_INPUT_METRICS; ++value_pos) {
            nn_input[pos] = ca->metrics[(ca->current_pointer + HISTORY_LEN - metric_pos) % HISTORY_LEN].values[value_pos];
            pos++;
        }
    }

    // TODO do not frequently report to user space
    // ret = report_to_user(nn_input, INPUT_SIZE);
    // if (ret == LF_ERROR) {
    //     printk(KERN_ERR "Report to user space failed!\n");
    // }
    
    ret = lf_query_model(LF_TCP_APP_ID, nn_input, nn_output);
    if (ret == LF_ERROR) {
        printk(KERN_ERR "Query NN model failed!\n");
    } else {
        output_rate = nn_output[OUTPUT_RATE];
        lf_set_rate(sk, output_rate);
    }

    lf_increase_pointer(ca);
}

static void lf_tcp_conn_in_ack_event(struct sock *sk, u32 flags) 
{
    // Obtain information
    // More can be added
    const struct tcp_sock *tp;
    struct lf_tcp_internal *ca;
    u32 acked_bytes, acked_packets;
    
    tp = tcp_sk(sk);
    ca = inet_csk_ca(sk);

    if(ca->metrics == NULL) {
        printk(KERN_ERR "Current flow is not managed by liteflow tcp kernel!\n");
        return;
    }

    acked_bytes = tp->snd_una - ca->last_snd_una;
    acked_packets = (u64)acked_bytes / tp->mss_cache;
    if((u64)acked_bytes % tp->mss_cache != 0) {
        acked_packets += 1;
    }

    ca->last_snd_una = tp->snd_una;

    // ca->metrics[ca->current_pointer].values[INPUT_METRICS_POS_BYTES_ACKED] = acked_bytes;
    // ca->metrics[ca->current_pointer].values[INPUT_METRICS_POS_PACKETS_ACKED] = acked_packets;
    // if (flags & CA_ACK_ECE) {
    //     ca->metrics[ca->current_pointer].values[INPUT_METRICS_POS_ECN_BYTES] = acked_bytes;
    //     ca->metrics[ca->current_pointer].values[INPUT_METRICS_POS_ECN_PACKETS] = acked_packets;
    // } else {
    //     ca->metrics[ca->current_pointer].values[INPUT_METRICS_POS_ECN_BYTES] = 0;
    //     ca->metrics[ca->current_pointer].values[INPUT_METRICS_POS_ECN_PACKETS] = 0;
    // }
}


struct app lf_tcp_app = {
    .appid = LF_TCP_APP_ID,
    .input_size = INPUT_SIZE,
    .output_size = NUM_OF_OUTPUT_VALUE,
};

struct tcp_congestion_ops lf_tcp_congestion_ops = {
    .init = lf_tcp_conn_init,
    .release = lf_tcp_conn_release,

    .ssthresh = lf_tcp_conn_ssthresh,
    .undo_cwnd = lf_tcp_conn_undo_cwnd,
    // .cong_avoid = lf_tcp_conn_cong_avoid,
    .cong_control = lf_tcp_conn_nn_control,
    // .set_state = ,
    // .pkts_acked = ，
    .in_ack_event = lf_tcp_conn_in_ack_event,
    .name = "lf_tcp_kernel",
    .owner = THIS_MODULE,
};

static int
__init liteflow_tcp_kernel_init(void)
{
    int ret;

    BUILD_BUG_ON(sizeof(struct lf_tcp_internal) > ICSK_CA_PRIV_SIZE);
    ret = tcp_register_congestion_control(&lf_tcp_congestion_ops);
    if (ret != 0) {
        printk(KERN_ERR "Cannot register liteflow tcp kernel with kernel CC!\n");
        return ret;
    }
    ret = lf_register_app(&lf_tcp_app);
    if (ret != LF_SUCCS) {
        printk(KERN_ERR "Cannot register liteflow tcp kernel with liteflow kernel!\n");
        return ret;
    }

    ret = genl_register_family(&lf_tcp_gnl_family);
    if (ret != 0) {
        printk(KERN_ERR "Cannot register liteflow tcp generic netlink!\n");
        return ret;
    }
    printk(KERN_INFO "Successfully register liteflow tcp kernel with liteflow kernel, kernel CC and netlink...\n");
    return ret;
}

static void
__exit liteflow_tcp_kernel_exit(void)
{
    int ret; 
    tcp_unregister_congestion_control(&lf_tcp_congestion_ops);
    lf_unregister_app(LF_TCP_APP_ID);
    ret = genl_unregister_family(&lf_tcp_gnl_family);
    if (ret != 0) {
        printk(KERN_ERR "Cannot unregister liteflow tcp generic netlink!\n");
    }
}

module_init(liteflow_tcp_kernel_init);
module_exit(liteflow_tcp_kernel_exit);

MODULE_DESCRIPTION("liteflow tcp kernel");
MODULE_AUTHOR("Junxue ZHANG");
MODULE_AUTHOR("Chaoliang ZENG");
MODULE_LICENSE("GPL v2");