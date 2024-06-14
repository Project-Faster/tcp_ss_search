// SPDX-License-Identifier: GPL-2.0-only
/*
 * TCP CUBIC: Binary Increase Congestion control for TCP v2.3
 * Home page:
 *      http://netsrv.csc.ncsu.edu/twiki/bin/view/Main/BIC
 * This is from the implementation of CUBIC TCP in
 * Sangtae Ha, Injong Rhee and Lisong Xu,
 *  "CUBIC: A New TCP-Friendly High-Speed TCP Variant"
 *  in ACM SIGOPS Operating System Review, July 2008.
 * Available from:
 *  http://netsrv.csc.ncsu.edu/export/cubic_a_new_tcp_2008.pdf
 *
 * CUBIC integrates a new slow start algorithm, called HyStart.
 * The details of HyStart are presented in
 *  Sangtae Ha and Injong Rhee,
 *  "Taming the Elephants: New TCP Slow Start", NCSU TechReport 2008.
 * Available from:
 *  http://netsrv.csc.ncsu.edu/export/hystart_techreport_2008.pdf
 *
 * All testing results are available from:
 * http://netsrv.csc.ncsu.edu/wiki/index.php/TCP_Testing
 *
 * Unless CUBIC is enabled and congestion window is large
 * this behaves the same as the original Reno.
 */

#include <linux/mm.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>
#include <linux/module.h>
#include <linux/math64.h>
#include <net/tcp.h>

#define BICTCP_BETA_SCALE    1024	/* Scale factor beta calculation
					 * max_cwnd = snd_cwnd * beta
					 */
#define	BICTCP_HZ		10	/* BIC HZ 2^10 = 1024 */

/* Two methods of hybrid slow start */
#define HYSTART_ACK_TRAIN	0x1
#define HYSTART_DELAY		0x2

/* Number of delay samples for detecting the increase of delay */
#define HYSTART_MIN_SAMPLES	8
#define HYSTART_DELAY_MIN	(4000U)	/* 4 ms */
#define HYSTART_DELAY_MAX	(16000U)	/* 16 ms */
#define HYSTART_DELAY_THRESH(x)	clamp(x, HYSTART_DELAY_MIN, HYSTART_DELAY_MAX)

static int fast_convergence __read_mostly = 1;
static int beta __read_mostly = 717;	/* = 717/1024 (BICTCP_BETA_SCALE) */
static int initial_ssthresh __read_mostly = TCP_INFINITE_SSTHRESH;
static int bic_scale __read_mostly = 41;
static int tcp_friendliness __read_mostly = 1;

static int hystart __read_mostly = 0;
static int hystart_detect __read_mostly = HYSTART_ACK_TRAIN | HYSTART_DELAY;
static int hystart_low_window __read_mostly = 16;
static int hystart_ack_delta_us __read_mostly = 2000;

static u32 cube_rtt_scale __read_mostly;
static u32 beta_scale __read_mostly;
static u64 cube_factor __read_mostly;

/* Note parameters that are used for precomputing scale factors are read-only */
module_param(fast_convergence, int, 0644);
MODULE_PARM_DESC(fast_convergence, "turn on/off fast convergence");
module_param(beta, int, 0644);
MODULE_PARM_DESC(beta, "beta for multiplicative increase");
module_param(initial_ssthresh, int, 0644);
MODULE_PARM_DESC(initial_ssthresh, "initial value of slow start threshold");
module_param(bic_scale, int, 0444);
MODULE_PARM_DESC(bic_scale, "scale (scaled by 1024) value for bic function (bic_scale/1024)");
module_param(tcp_friendliness, int, 0644);
MODULE_PARM_DESC(tcp_friendliness, "turn on/off tcp friendliness");
module_param(hystart, int, 0644);
MODULE_PARM_DESC(hystart, "turn on/off hybrid slow start algorithm");
module_param(hystart_detect, int, 0644);
MODULE_PARM_DESC(hystart_detect, "hybrid slow start detection mechanisms"
		 " 1: packet-train 2: delay 3: both packet-train and delay");
module_param(hystart_low_window, int, 0644);
MODULE_PARM_DESC(hystart_low_window, "lower bound cwnd for hybrid slow start");
module_param(hystart_ack_delta_us, int, 0644);
MODULE_PARM_DESC(hystart_ack_delta_us, "spacing between ack's indicating train (usecs)");

#define TOTAL_NUM_BINS		30
#define LOOK_BACK_WINDOW	10
#define DEBUG_BUF_SIZE		256
#define SCALE_FACTOR_100	100

#define FOUND_SSTHRESH_HYSTART	1
#define FOUND_SSTHRESH_SEARCH 	2
#define FOUND_SNDCLAMP_SEARCH	3
#define FOUND_SSTHRESH_LOSS	4

static int search_enable_mode __read_mostly = 2;	/* 0: disable, 1: set ssthresh when exit, 2: set snd_clamp and ssthresh when exit */
static int max_rtt_factor __read_mostly = 350;
static int search_exit_thresh __read_mostly = 25;
static int search_double_cross_exit __read_mostly = 0;
static short debug_port __read_mostly = 5201;

//with our initial design, the look_back_window size and total number of bins are configurable
//static int look_back_window __read_mostly = LOOK_BACK_WINDOW;
//static int total_bins __read_mostly = TOTAL_NUM_BINS;

module_param(search_enable_mode, int, 0644);
MODULE_PARM_DESC(search_enable_mode, "search  mode. 0: disable, 1: set ssthresh exit, 2: set snd_clamp and ssthresh when exit");
module_param(max_rtt_factor, int, 0644);
MODULE_PARM_DESC(max_rtt_factor, "number of RTTs required for search");
module_param(search_exit_thresh, int, 0644);
MODULE_PARM_DESC(search_exit_thresh, "threshold to exit from search");
module_param(search_double_cross_exit, int, 0644);
MODULE_PARM_DESC(search_double_cross_exit, "exit only two successive normalized diff samples no less than exit threshold");
module_param(debug_port, short, 0644);
MODULE_PARM_DESC(debug_port, "for debug usage (set to 0 to disable debug)");


/* BIC TCP Parameters */
struct bictcp {
	u32	cnt;		/* increase cwnd by 1 after ACKs */
	u32	last_max_cwnd;	/* last maximum snd_cwnd */
	u32	loss_cwnd;	/* congestion window at last loss */
	u32	last_cwnd;	/* the last snd_cwnd */
	u32	last_time;	/* time when updated last_cwnd */
	u32	bic_origin_point;/* origin point of bic function */
	u32	bic_K;		/* time to origin point
				   from the beginning of the current epoch */
	u32	delay_min;	/* min delay (usec) */
	u32	epoch_start;	/* beginning of an epoch */
	u32	ack_cnt;	/* number of acks */
	u32	tcp_cwnd;	/* estimated tcp cwnd */
	u16	unused;
	u8	sample_cnt;	/* number of samples to decide curr_rtt */
	u8	found;		/* the exit point is found? */
	u32	round_start;	/* beginning of each round */
	u32	end_seq;	/* end_seq of the round */
	u32	last_ack;	/* last time when the ACK spacing is close */
	u32	curr_rtt;	/* the minimum rtt of current round */

	u32	epoch_dur_us;
	u32	epoch_expires_us;
	u32	start_tm_us;
        u32     index;
        u32     *bins;
        u64	bytes_acked_prev;
#ifdef LARGE_SOCK_PRIV
	u32	latest_rtt_us;
	u32	sum_delivered_curr;
	u32	sum_delivered_prev;
#endif
	s32	factor;
};

static struct tcp_congestion_ops cubictcp;
static inline void bictcp_search_reset(struct sock *sk);
static inline u32 bictcp_clock_us(const struct sock *sk);

static u64 tcp_compute_delivery_rate(const struct tcp_sock *tp)
{
        u32 rate = READ_ONCE(tp->rate_delivered);
        u32 intv = READ_ONCE(tp->rate_interval_us);
        u64 rate64 = 0;

        if (rate && intv) {
                rate64 = (u64)rate * tp->mss_cache * USEC_PER_SEC;
                do_div(rate64, intv);
        }
	// convert rate Bps to Kbps
        return (rate64>>7);
}

static inline void __debug_get_conn_str(struct sock *sk, char *str, size_t len)
{
        const struct inet_sock *inet = inet_sk(sk);
#if 0
        if (sk->sk_family == AF_INET) {
#endif
                snprintf(str, len, "%pI4:%u -> %pI4:%u",
                                &(inet->inet_saddr),
                                ntohs(inet->inet_sport),
                                &(inet->inet_daddr),
                                ntohs(inet->inet_dport));
#if 0
        } else {
                snprintf(str, len, "%pI6:%u -> %pI6:%u",
                                &(inet->pinet6->saddr),
                                ntohs(inet->inet_sport),
                                &(sk->sk_v6_daddr),
                                ntohs(inet->inet_dport));
        }
#endif
        return;
}

static inline void __debug_epoch_info(struct sock *sk, const char *func, const u32 line)
{
        const struct tcp_sock *tp = tcp_sk(sk);
        const struct bictcp *ca = inet_csk_ca(sk);
        char str_conn[DEBUG_BUF_SIZE] = {0};

        if (ntohs(inet_sk(sk)->inet_dport) != debug_port &&
                ntohs(inet_sk(sk)->inet_sport) != debug_port)
                return;
        __debug_get_conn_str(sk, str_conn, sizeof(str_conn));

	pr_info("time_ms: %lu, ", (bictcp_clock_us(sk) - ca->start_tm_us) / USEC_PER_MSEC);
        pr_cont("module: %s, ", cubictcp.name);
	pr_cont("start_tm_ms: %lu, ", ca->start_tm_us / USEC_PER_MSEC);
        pr_cont("%s, ", str_conn);
        pr_cont("func: %s, ", func);
	pr_cont("line: %d, ", line);
        pr_cont("srtt_ms: %lu, ", (u32) (tp->srtt_us >> 3) / USEC_PER_MSEC);
	pr_cont("snd_cwnd: %u, ", tp->snd_cwnd);
	pr_cont("snd_cwnd_clamp: %u, ", tp->snd_cwnd_clamp);
        pr_cont("ssthresh: %u, ", tp->snd_ssthresh);
	pr_cont("snd_wnd: %u, ", tp->snd_wnd / (tp->mss_cache ? tp->mss_cache : TCP_MSS_DEFAULT));
	pr_cont("max_window_kb: %u, ", tp->max_window >> 10);
	pr_cont("bytes_sent: %llu, ", tp->bytes_sent);
        pr_cont("bytes_acked: %llu, ", tp->bytes_acked);
        pr_cont("total_retrans: %u, ", tp->total_retrans);
        pr_cont("packets_out: %u, ", tp->packets_out);
        pr_cont("sacked_out: %u, ", tp->sacked_out);
	pr_cont("lost_out: %u, ", tp->lost_out);
	pr_cont("retrans_out: %u, ", tp->retrans_out);
	pr_cont("delivery_rate: %llu, ", tcp_compute_delivery_rate(tp));
        pr_cont("ca_state: %u, ", inet_csk(sk)->icsk_ca_state);
	pr_cont("icsk_rto_ms: %u, ", jiffies_to_msecs(inet_csk(sk)->icsk_rto));
	pr_cont("epoch_dur_ms: %lu, ", ca->epoch_dur_us / USEC_PER_MSEC);
	pr_cont("epoch_expires_ms: %lu, ", ca->epoch_expires_us / USEC_PER_MSEC);
	pr_cont("index: %u, ", ca->index);
#ifdef LARGE_SOCK_PRIV
	pr_cont("latest_rtt_ms: %lu, ", ca->latest_rtt_us / USEC_PER_MSEC);
	pr_cont("sum_delivered_prev: %u, ", ca->sum_delivered_prev);
	pr_cont("sum_delivered_curr: %u, ", ca->sum_delivered_curr);
#endif
	pr_cont("bytes_acked_prev: %llu, ", ca->bytes_acked_prev);
	pr_cont("factor: %d, ", ca->factor);
	pr_cont("found: %u, ", ca->found);

	pr_cont("\n");

}

static inline u32 bictcp_clock(void)
{
#if HZ < 1000
        return ktime_to_ms(ktime_get_real());
#else
        return jiffies_to_msecs(jiffies);
#endif
}

static inline void bictcp_reset(struct bictcp *ca)
{
	memset(ca, 0, offsetof(struct bictcp, unused));
	ca->found = 0;
}

static inline u32 bictcp_clock_us(const struct sock *sk)
{
	return tcp_sk(sk)->tcp_mstamp;
}

static inline void bictcp_hystart_reset(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);

	ca->round_start = ca->last_ack = bictcp_clock_us(sk);
	ca->end_seq = tp->snd_nxt;
	ca->curr_rtt = ~0U;
	ca->sample_cnt = 0;
}

static inline void bictcp_search_reset(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);
	u32 rtt_min_us = tcp_min_rtt(tp);
	ca->bytes_acked_prev = tp->bytes_acked;

	if (rtt_min_us == ~0)
		ca->epoch_dur_us = 0;
	else
		ca->epoch_dur_us =  rtt_min_us * max_rtt_factor / SCALE_FACTOR_100 / LOOK_BACK_WINDOW;

	ca->epoch_expires_us = bictcp_clock_us(sk) + ca->epoch_dur_us;
	ca->bytes_acked_prev = tp->bytes_acked;
	ca->index = 0;

	if (!ca->bins)
		ca->bins = (u32 *) kcalloc(TOTAL_NUM_BINS, sizeof(u32), GFP_NOWAIT | __GFP_NOWARN);

	if (ca->bins)
		memset(ca->bins, 0, sizeof(u32) * TOTAL_NUM_BINS);
	return;
}

__bpf_kfunc static void cubictcp_init(struct sock *sk)
{
	struct bictcp *ca = inet_csk_ca(sk);

	bictcp_reset(ca);
	ca->loss_cwnd = 0;
	ca->start_tm_us = bictcp_clock_us(sk);

	if (hystart)
		bictcp_hystart_reset(sk);

	if (!hystart && initial_ssthresh)
		tcp_sk(sk)->snd_ssthresh = initial_ssthresh;

	if (search_enable_mode) {
		bictcp_search_reset(sk);
	}
}

__bpf_kfunc static void cubictcp_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
	if (event == CA_EVENT_TX_START) {
		struct bictcp *ca = inet_csk_ca(sk);
		u32 now = tcp_jiffies32;
		s32 delta;

		delta = now - tcp_sk(sk)->lsndtime;

		/* We were application limited (idle) for a while.
		 * Shift epoch_start to keep cwnd growth to cubic curve.
		 */
		if (ca->epoch_start && delta > 0) {
			ca->epoch_start += delta;
			if (after(ca->epoch_start, now))
				ca->epoch_start = now;
		}
		return;
	}
}

/* calculate the cubic root of x using a table lookup followed by one
 * Newton-Raphson iteration.
 * Avg err ~= 0.195%
 */
static u32 cubic_root(u64 a)
{
	u32 x, b, shift;
	/*
	 * cbrt(x) MSB values for x MSB values in [0..63].
	 * Precomputed then refined by hand - Willy Tarreau
	 *
	 * For x in [0..63],
	 *   v = cbrt(x << 18) - 1
	 *   cbrt(x) = (v[x] + 10) >> 6
	 */
	static const u8 v[] = {
		/* 0x00 */    0,   54,   54,   54,  118,  118,  118,  118,
		/* 0x08 */  123,  129,  134,  138,  143,  147,  151,  156,
		/* 0x10 */  157,  161,  164,  168,  170,  173,  176,  179,
		/* 0x18 */  181,  185,  187,  190,  192,  194,  197,  199,
		/* 0x20 */  200,  202,  204,  206,  209,  211,  213,  215,
		/* 0x28 */  217,  219,  221,  222,  224,  225,  227,  229,
		/* 0x30 */  231,  232,  234,  236,  237,  239,  240,  242,
		/* 0x38 */  244,  245,  246,  248,  250,  251,  252,  254,
	};

	b = fls64(a);
	if (b < 7) {
		/* a in [0..63] */
		return ((u32)v[(u32)a] + 35) >> 6;
	}

	b = ((b * 84) >> 8) - 1;
	shift = (a >> (b * 3));

	x = ((u32)(((u32)v[shift] + 10) << b)) >> 6;

	/*
	 * Newton-Raphson iteration
	 *                         2
	 * x    = ( 2 * x  +  a / x  ) / 3
	 *  k+1          k         k
	 */
	x = (2 * x + (u32)div64_u64(a, (u64)x * (u64)(x - 1)));
	x = ((x * 341) >> 10);
	return x;
}

/*
 * Compute congestion window to use.
 */
static inline void bictcp_update(struct bictcp *ca, u32 cwnd, u32 acked)
{
	u32 delta, bic_target, max_cnt;
	u64 offs, t;

	ca->ack_cnt += acked;	/* count the number of ACKed packets */

	if (ca->last_cwnd == cwnd &&
	    (s32)(tcp_jiffies32 - ca->last_time) <= HZ / 32)
		return;

	/* The CUBIC function can update ca->cnt at most once per jiffy.
	 * On all cwnd reduction events, ca->epoch_start is set to 0,
	 * which will force a recalculation of ca->cnt.
	 */
	if (ca->epoch_start && tcp_jiffies32 == ca->last_time)
		goto tcp_friendliness;

	ca->last_cwnd = cwnd;
	ca->last_time = tcp_jiffies32;

	if (ca->epoch_start == 0) {
		ca->epoch_start = tcp_jiffies32;	/* record beginning */
		ca->ack_cnt = acked;			/* start counting */
		ca->tcp_cwnd = cwnd;			/* syn with cubic */

		if (ca->last_max_cwnd <= cwnd) {
			ca->bic_K = 0;
			ca->bic_origin_point = cwnd;
		} else {
			/* Compute new K based on
			 * (wmax-cwnd) * (srtt>>3 / HZ) / c * 2^(3*bictcp_HZ)
			 */
			ca->bic_K = cubic_root(cube_factor
					       * (ca->last_max_cwnd - cwnd));
			ca->bic_origin_point = ca->last_max_cwnd;
		}
	}

	/* cubic function - calc*/
	/* calculate c * time^3 / rtt,
	 *  while considering overflow in calculation of time^3
	 * (so time^3 is done by using 64 bit)
	 * and without the support of division of 64bit numbers
	 * (so all divisions are done by using 32 bit)
	 *  also NOTE the unit of those veriables
	 *	  time  = (t - K) / 2^bictcp_HZ
	 *	  c = bic_scale >> 10
	 * rtt  = (srtt >> 3) / HZ
	 * !!! The following code does not have overflow problems,
	 * if the cwnd < 1 million packets !!!
	 */

	t = (s32)(tcp_jiffies32 - ca->epoch_start);
	t += usecs_to_jiffies(ca->delay_min);
	/* change the unit from HZ to bictcp_HZ */
	t <<= BICTCP_HZ;
	do_div(t, HZ);

	if (t < ca->bic_K)		/* t - K */
		offs = ca->bic_K - t;
	else
		offs = t - ca->bic_K;

	/* c/rtt * (t-K)^3 */
	delta = (cube_rtt_scale * offs * offs * offs) >> (10+3*BICTCP_HZ);
	if (t < ca->bic_K)                            /* below origin*/
		bic_target = ca->bic_origin_point - delta;
	else                                          /* above origin*/
		bic_target = ca->bic_origin_point + delta;

	/* cubic function - calc bictcp_cnt*/
	if (bic_target > cwnd) {
		ca->cnt = cwnd / (bic_target - cwnd);
	} else {
		ca->cnt = 100 * cwnd;              /* very small increment*/
	}

	/*
	 * The initial growth of cubic function may be too conservative
	 * when the available bandwidth is still unknown.
	 */
	if (ca->last_max_cwnd == 0 && ca->cnt > 20)
		ca->cnt = 20;	/* increase cwnd 5% per RTT */

tcp_friendliness:
	/* TCP Friendly */
	if (tcp_friendliness) {
		u32 scale = beta_scale;

		delta = (cwnd * scale) >> 3;
		while (ca->ack_cnt > delta) {		/* update tcp cwnd */
			ca->ack_cnt -= delta;
			ca->tcp_cwnd++;
		}

		if (ca->tcp_cwnd > cwnd) {	/* if bic is slower than tcp */
			delta = ca->tcp_cwnd - cwnd;
			max_cnt = cwnd / delta;
			if (ca->cnt > max_cnt)
				ca->cnt = max_cnt;
		}
	}

	/* The maximum rate of cwnd increase CUBIC allows is 1 packet per
	 * 2 packets ACKed, meaning cwnd grows at 1.5x per RTT.
	 */
	ca->cnt = max(ca->cnt, 2U);
}

__bpf_kfunc static void cubictcp_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);

	if (!tcp_is_cwnd_limited(sk))
		return;

	if (tcp_in_slow_start(tp)) {
		acked = tcp_slow_start(tp, acked);
		if (!acked)
			return;
	}
	bictcp_update(ca, tcp_snd_cwnd(tp), acked);
	tcp_cong_avoid_ai(tp, ca->cnt, acked);
}

__bpf_kfunc static u32 cubictcp_recalc_ssthresh(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);
	u32 ssthresh = tp->snd_ssthresh;

	ca->epoch_start = 0;	/* end of epoch */

	/* Wmax and fast convergence */
	if (tcp_snd_cwnd(tp) < ca->last_max_cwnd && fast_convergence)
		ca->last_max_cwnd = (tcp_snd_cwnd(tp) * (BICTCP_BETA_SCALE + beta))
			/ (2 * BICTCP_BETA_SCALE);
	else
		ca->last_max_cwnd = tcp_snd_cwnd(tp);

	return max((tcp_snd_cwnd(tp) * beta) / BICTCP_BETA_SCALE, 2U);
}

__bpf_kfunc static void cubictcp_state(struct sock *sk, u8 new_state)
{
	if (new_state == TCP_CA_Loss) {
		bictcp_reset(inet_csk_ca(sk));
		bictcp_hystart_reset(sk);
	}
}

/* Account for TSO/GRO delays.
 * Otherwise short RTT flows could get too small ssthresh, since during
 * slow start we begin with small TSO packets and ca->delay_min would
 * not account for long aggregation delay when TSO packets get bigger.
 * Ideally even with a very small RTT we would like to have at least one
 * TSO packet being sent and received by GRO, and another one in qdisc layer.
 * We apply another 100% factor because @rate is doubled at this point.
 * We cap the cushion to 1ms.
 */
static u32 hystart_ack_delay(const struct sock *sk)
{
	unsigned long rate;

	rate = READ_ONCE(sk->sk_pacing_rate);
	if (!rate)
		return 0;
	return min_t(u64, USEC_PER_MSEC,
		     div64_ul((u64)sk->sk_gso_max_size * 4 * USEC_PER_SEC, rate));
}

static void hystart_update(struct sock *sk, u32 delay)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);
	u32 threshold;

	if (after(tp->snd_una, ca->end_seq))
		bictcp_hystart_reset(sk);

	if (hystart_detect & HYSTART_ACK_TRAIN) {
		u32 now = bictcp_clock_us(sk);

		/* first detection parameter - ack-train detection */
		if ((s32)(now - ca->last_ack) <= hystart_ack_delta_us) {
			ca->last_ack = now;

			threshold = ca->delay_min + hystart_ack_delay(sk);

			/* Hystart ack train triggers if we get ack past
			 * ca->delay_min/2.
			 * Pacing might have delayed packets up to RTT/2
			 * during slow start.
			 */
			if (sk->sk_pacing_status == SK_PACING_NONE)
				threshold >>= 1;

			if ((s32)(now - ca->round_start) > threshold) {
				ca->found = 1;
				pr_debug("hystart_ack_train (%u > %u) delay_min %u (+ ack_delay %u) cwnd %u\n",
					 now - ca->round_start, threshold,
					 ca->delay_min, hystart_ack_delay(sk), tcp_snd_cwnd(tp));
				NET_INC_STATS(sock_net(sk),
					      LINUX_MIB_TCPHYSTARTTRAINDETECT);
				NET_ADD_STATS(sock_net(sk),
					      LINUX_MIB_TCPHYSTARTTRAINCWND,
					      tcp_snd_cwnd(tp));
				tp->snd_ssthresh = tcp_snd_cwnd(tp);
			}
		}
	}

	if (hystart_detect & HYSTART_DELAY) {
		/* obtain the minimum delay of more than sampling packets */
		if (ca->curr_rtt > delay)
			ca->curr_rtt = delay;
		if (ca->sample_cnt < HYSTART_MIN_SAMPLES) {
			ca->sample_cnt++;
		} else {
			if (ca->curr_rtt > ca->delay_min +
			    HYSTART_DELAY_THRESH(ca->delay_min >> 3)) {
				ca->found = 1;
				NET_INC_STATS(sock_net(sk),
					      LINUX_MIB_TCPHYSTARTDELAYDETECT);
				NET_ADD_STATS(sock_net(sk),
					      LINUX_MIB_TCPHYSTARTDELAYCWND,
					      tcp_snd_cwnd(tp));
				tp->snd_ssthresh = tcp_snd_cwnd(tp);
			}
		}
	}
}

static inline u64 get_window_sum(u32 *array, u32 start, u32 end, u32 len)
{
	u64 total = 0, i = 0;
	for(i = start; i <= end; ++i) {
		total += array[i % len];
	}
	return total;
}

static inline bool search_exit_slowstart(struct sock *sk, const u32 delay)
{
	struct bictcp *ca = inet_csk_ca(sk);
	u32 left, right;
	s32 index_prev;
	s32 factor = 0, index = ca->index - 1;
	s64 total = 0, total_pre = 0;
	bool rc = false;

	if (tcp_sk(sk)->rate_app_limited)
		return false;

	if (!ca->epoch_dur_us) {
		BUG();
		return false;
	}

	index_prev = index - delay / ca->epoch_dur_us;
	if (index_prev < TOTAL_NUM_BINS - 1)
		return rc;

	left = index_prev - LOOK_BACK_WINDOW + 1; 
	if ((index - left + 1) < TOTAL_NUM_BINS)
		return false;
	right = index_prev;
	total_pre = get_window_sum(ca->bins, left, right, TOTAL_NUM_BINS);

	left = index - LOOK_BACK_WINDOW + 1;
	right = index;
	total = get_window_sum(ca->bins, left, right, TOTAL_NUM_BINS);

#ifdef LARGE_SOCK_PRIV
	ca->sum_delivered_curr = total;
	ca->sum_delivered_prev = total_pre;
#endif

	if (total_pre == 0) {
		__debug_epoch_info(sk,	__func__, __LINE__);
		return false;
	}

	factor = (s32) (((total_pre << 1) - total) * SCALE_FACTOR_100 / (total_pre << 1)) ;

	if (search_double_cross_exit) {
		if (ca->factor >= search_exit_thresh && factor >= search_exit_thresh)
			rc = true;
	} else if (factor >= search_exit_thresh) {
		rc = true;
	}

	ca->factor = factor;
	return rc;
}

static inline void bictcp_search_normalize(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);
	u32 now = bictcp_clock_us(sk), last_epoch_us = ca->epoch_expires_us - ca->epoch_dur_us;
	u32 n = (now - last_epoch_us) / (ca->epoch_dur_us ? ca->epoch_dur_us : 1);
	u32 acked_bytes = tp->bytes_acked - ca->bytes_acked_prev;
        u32 bytes_per_bin = acked_bytes / (n ? n : 1);
	int i;

	for(i = 0; i < n; i++) {
		if (acked_bytes < bytes_per_bin * 2)
			ca->bins[(ca->index % TOTAL_NUM_BINS)] = acked_bytes;
		else
			ca->bins[(ca->index % TOTAL_NUM_BINS)] = bytes_per_bin;
		acked_bytes -= bytes_per_bin;
		ca->index += 1;
	}

	ca->bytes_acked_prev = tp->bytes_acked;
	return;
}

static void hystart_search_update(struct sock *sk, u32 rtt_us)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);
	u32 last_epoch_expires;
	u32 now = bictcp_clock_us(sk);

	if (ca->found)
                return;

	if (!ca->epoch_expires_us || !ca->epoch_dur_us) {
		bictcp_search_reset(sk);
	}

	ca->epoch_dur_us = (ca->epoch_dur_us ? ca->epoch_dur_us : 1);

#ifdef LARGE_SOCK_PRIV
	ca->latest_rtt_us = rtt_us;
#endif
	if (now >= ca->epoch_expires_us) {
		last_epoch_expires = ca->epoch_expires_us - ca->epoch_dur_us;
		//long idle time
		while (last_epoch_expires + ca->epoch_dur_us < now) {
			// it would run at least once
			ca->index += 1;
			ca->bins[(ca->index % TOTAL_NUM_BINS)] = 0;
			last_epoch_expires += ca->epoch_dur_us;
		}

		if (search_exit_slowstart(sk, rtt_us)) {
			tp->snd_ssthresh = max(tp->snd_cwnd, 10U);

			if (search_enable_mode == 2) {
				tp->snd_cwnd_clamp = max(tp->snd_cwnd + (tp->snd_cwnd >> 3),
									64 * 1024 / tp->mss_cache);
				ca->found = FOUND_SNDCLAMP_SEARCH;

			} else {
				ca->found = FOUND_SSTHRESH_SEARCH;
			}

			__debug_epoch_info(sk, __func__, __LINE__);
			pr_err("exit: search");
			goto out;
		}

		ca->epoch_expires_us = last_epoch_expires + ca->epoch_dur_us;
	}
	
	ca->bins[(ca->index % TOTAL_NUM_BINS)] += tp->bytes_acked - ca->bytes_acked_prev;
	ca->bytes_acked_prev = tp->bytes_acked;

out:
	if (ca->epoch_expires_us == last_epoch_expires + ca->epoch_dur_us)
		__debug_epoch_info(sk, __func__, __LINE__);

	return;
}

__bpf_kfunc static void cubictcp_acked(struct sock *sk, const struct ack_sample *sample)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);
	u32 delay;

	/* Some calls are for duplicates without timetamps */
	if (sample->rtt_us < 0)
		return;

	/* Discard delay samples right after fast recovery */
	if (ca->epoch_start && (s32)(tcp_jiffies32 - ca->epoch_start) < HZ)
		return;

	delay = sample->rtt_us;
	if (delay == 0)
		delay = 1;

	/* first time call or link delay decreases */
	if (ca->delay_min == 0 || ca->delay_min > delay)
		ca->delay_min = delay;

	/* hystart triggers when cwnd is larger than some threshold */
	if (!ca->found && tcp_in_slow_start(tp) && hystart &&
	    tcp_snd_cwnd(tp) >= hystart_low_window)
		hystart_update(sk, delay);

	if (search_enable_mode && tcp_in_slow_start(tp))
		hystart_search_update(sk, sample->rtt_us);

}

static void bictcp_release(struct sock *sk)
{
	struct bictcp *ca = inet_csk_ca(sk);

	__debug_epoch_info(sk, __func__, __LINE__);

	if (ca->bins)
		kfree(ca->bins);
	ca->bins = NULL;
}

static struct tcp_congestion_ops cubictcp __read_mostly = {
	.init		= cubictcp_init,
	.ssthresh	= cubictcp_recalc_ssthresh,
	.cong_avoid	= cubictcp_cong_avoid,
	.set_state	= cubictcp_state,
	.undo_cwnd	= tcp_reno_undo_cwnd,
	.cwnd_event	= cubictcp_cwnd_event,
	.release	= bictcp_release,
	.pkts_acked     = cubictcp_acked,
	.owner		= THIS_MODULE,
	.name		= "cubicv2",
};

BTF_KFUNCS_START(tcp_cubic_check_kfunc_ids)
BTF_ID_FLAGS(func, cubictcp_init)
BTF_ID_FLAGS(func, cubictcp_recalc_ssthresh)
BTF_ID_FLAGS(func, cubictcp_cong_avoid)
BTF_ID_FLAGS(func, cubictcp_state)
BTF_ID_FLAGS(func, cubictcp_cwnd_event)
BTF_ID_FLAGS(func, cubictcp_acked)
BTF_KFUNCS_END(tcp_cubic_check_kfunc_ids)

static const struct btf_kfunc_id_set tcp_cubic_kfunc_set = {
	.owner = THIS_MODULE,
	.set   = &tcp_cubic_check_kfunc_ids,
};

static int __init cubictcp_register(void)
{
	int ret;

	BUILD_BUG_ON(sizeof(struct bictcp) > ICSK_CA_PRIV_SIZE);

	/* Precompute a bunch of the scaling factors that are used per-packet
	 * based on SRTT of 100ms
	 */

	pr_info("TCP Cubicv2 w/ SEARCH implementation registered.");
        pr_cont(" ICSK_CA_PRIV_SIZE: %lu, sizeof(struct bictcp): %lu.\n", 
			ICSK_CA_PRIV_SIZE, sizeof(struct bictcp));

	beta_scale = 8*(BICTCP_BETA_SCALE+beta) / 3
		/ (BICTCP_BETA_SCALE - beta);

	cube_rtt_scale = (bic_scale * 10);	/* 1024*c/rtt */

	/* calculate the "K" for (wmax-cwnd) = c/rtt * K^3
	 *  so K = cubic_root( (wmax-cwnd)*rtt/c )
	 * the unit of K is bictcp_HZ=2^10, not HZ
	 *
	 *  c = bic_scale >> 10
	 *  rtt = 100ms
	 *
	 * the following code has been designed and tested for
	 * cwnd < 1 million packets
	 * RTT < 100 seconds
	 * HZ < 1,000,00  (corresponding to 10 nano-second)
	 */

	/* 1/c * 2^2*bictcp_HZ * srtt */
	cube_factor = 1ull << (10+3*BICTCP_HZ); /* 2^40 */

	/* divide by bic_scale and by constant Srtt (100ms) */
	do_div(cube_factor, bic_scale * 10);

	ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS, &tcp_cubic_kfunc_set);
	if (ret < 0)
		return ret;
	return tcp_register_congestion_control(&cubictcp);
}

static void __exit cubictcp_unregister(void)
{
	tcp_unregister_congestion_control(&cubictcp);
}

module_init(cubictcp_register);
module_exit(cubictcp_unregister);

MODULE_AUTHOR("Sangtae Ha, Stephen Hemminger");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CUBIC TCP");
MODULE_VERSION("2.3");
