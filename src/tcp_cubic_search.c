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
static int initial_ssthresh __read_mostly;
static int bic_scale __read_mostly = 41;
static int tcp_friendliness __read_mostly = 1;

static int hystart __read_mostly = 0; 	/* Disabled Hystart to use SEARCH */
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


//////////////////////// SEARCH ////////////////////////
/**
   disable search with command:
 		sudo sh -c "echo '0' > /sys/module/cubic_with_search/parameters/search"
   enable search with command:
 		sudo sh -c "echo '1' > /sys/module/cubic_with_search/parameters/search"
*/

#define SEARCH_BINS 10		/* Number of bins in a window */
#define SEARCH_TOTAL_BINS 25 	/* Total number of bins containing essential
				   bins to cover RTT shift */

static int search __read_mostly = 1;
static int search_window_size_time __read_mostly = 35;
static int search_thresh __read_mostly = 35;
static int cwnd_rollback __read_mostly = 1;
static int do_intpld __read_mostly = 1;

// Module parameters used by SEARCH
module_param(search, int, 0644);
MODULE_PARM_DESC(search, "Enable SEARCH algorithm 0: disabled, 1: enabled");
module_param(search_window_size_time, int, 0644);
MODULE_PARM_DESC(search_window_size_time, "Multiply with (initial RTT / 10) to set the window size");
module_param(search_thresh, int, 0644);
MODULE_PARM_DESC(search_thresh, "Threshold for exiting from slow start in percentage");
module_param(cwnd_rollback, int, 0644);
MODULE_PARM_DESC(cwnd_rollback, "Decrease the cwnd to its value in 2 initial RTT ago");
module_param(do_intpld, int, 0644);
MODULE_PARM_DESC(do_intpld, "Do interpolation for calculating previous delivered bytes window");

/* BIC TCP Parameters */
struct bictcp {
	u32	cnt;		/* increase cwnd by 1 after ACKs */
	u32	last_max_cwnd;	/* last maximum snd_cwnd */
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

	//////////////////////// SEARCH ////////////////////////
	u32	bin[SEARCH_TOTAL_BINS]; 	/* array to keep bytes for bins */
	u32	bin_duration_us; 		/* duration of each bin in microsecond */
	u32	bin_total; 			/* total number of bins */
	u32	bin_end_us; 			/* end time of the latest bin in microsecond */
	u8	stop_search; 			/* the choke/exit point based on SEARCH is found */
	u64	prev_bytes_acked;		/* previous total bytes acked */
	////////////////////////////////////////////////////////
};

static inline void bictcp_search_reset(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);
	memset(ca->bin, 0, sizeof(ca->bin));
	ca->bin_duration_us = 0;
	ca->bin_total = 0;
	ca->bin_end_us = 0;
	ca->stop_search = 0;
	ca->prev_bytes_acked = tp->bytes_acked;
}

static inline void bictcp_reset(struct bictcp *ca)
{
	ca->cnt = 0;
	ca->last_max_cwnd = 0;
	ca->last_cwnd = 0;
	ca->last_time = 0;
	ca->bic_origin_point = 0;
	ca->bic_K = 0;
	ca->delay_min = 0;
	ca->epoch_start = 0;
	ca->ack_cnt = 0;
	ca->tcp_cwnd = 0;
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


static void bictcp_init(struct sock *sk)
{
	struct bictcp *ca = inet_csk_ca(sk);

	bictcp_reset(ca);

	if (hystart)
		bictcp_hystart_reset(sk);

	if (search)
		bictcp_search_reset(sk);

	if (!hystart && initial_ssthresh)
		tcp_sk(sk)->snd_ssthresh = initial_ssthresh;
}

static void bictcp_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
	struct bictcp *ca = inet_csk_ca(sk);

	switch(event) {
	case CA_EVENT_TX_START:
		s32 delta;
		u32 now = tcp_jiffies32;
		delta = now - tcp_sk(sk)->lsndtime;

		/* We were application limited (idle) for a while.
		 * Shift epoch_start to keep cwnd growth to cubic curve.
		 */
		if (ca->epoch_start && delta > 0) {
			ca->epoch_start += delta;
			if (after(ca->epoch_start, now))
				ca->epoch_start = now;
		}
		break;
	case CA_EVENT_CWND_RESTART:
		if (search)
			bictcp_search_reset(sk);
		break;
	default:
		break;
	}
	return;
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
	 *			 2
	 * x    = ( 2 * x  +  a / x  ) / 3
	 *  k+1	  k	 k
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
	if (t < ca->bic_K)			    /* below origin*/
		bic_target = ca->bic_origin_point - delta;
	else					  /* above origin*/
		bic_target = ca->bic_origin_point + delta;

	/* cubic function - calc bictcp_cnt*/
	if (bic_target > cwnd) {
		ca->cnt = cwnd / (bic_target - cwnd);
	} else {
		ca->cnt = 100 * cwnd;	      /* very small increment*/
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

static void bictcp_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);

	if (!tcp_is_cwnd_limited(sk))
		return;

	if (tcp_in_slow_start(tp)) {

		if (hystart && after(ack, ca->end_seq))
			bictcp_hystart_reset(sk);
		acked = tcp_slow_start(tp, acked);
		if (!acked)
			return;
	}
	bictcp_update(ca, tp->snd_cwnd, acked);
	tcp_cong_avoid_ai(tp, ca->cnt, acked);
}

static u32 bictcp_recalc_ssthresh(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);

	ca->epoch_start = 0;	/* end of epoch */

	/* Wmax and fast convergence */
	if (tp->snd_cwnd < ca->last_max_cwnd && fast_convergence)
		ca->last_max_cwnd = (tp->snd_cwnd * (BICTCP_BETA_SCALE + beta))
			/ (2 * BICTCP_BETA_SCALE);
	else
		ca->last_max_cwnd = tp->snd_cwnd;

	return max((tp->snd_cwnd * beta) / BICTCP_BETA_SCALE, 2U);
}

static void bictcp_state(struct sock *sk, u8 new_state)
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
static u32 hystart_ack_delay(struct sock *sk)
{
	unsigned long rate;

	rate = READ_ONCE(sk->sk_pacing_rate);
	if (!rate)
		return 0;
	return min_t(u64, USEC_PER_MSEC,
		     div64_ul((u64)GSO_MAX_SIZE * 4 * USEC_PER_SEC, rate));
}

static void hystart_update(struct sock *sk, u32 delay)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);
	u32 threshold;

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
					 ca->delay_min, hystart_ack_delay(sk), tp->snd_cwnd);
				NET_INC_STATS(sock_net(sk),
					      LINUX_MIB_TCPHYSTARTTRAINDETECT);
				NET_ADD_STATS(sock_net(sk),
					      LINUX_MIB_TCPHYSTARTTRAINCWND,
					      tp->snd_cwnd);
				tp->snd_ssthresh = tp->snd_cwnd;
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
					      tp->snd_cwnd);
				tp->snd_ssthresh = tp->snd_cwnd;
			}
		}
	}
}

// function to update missed bins
static void search_update_missed_bins(struct sock *sk)
{
	struct bictcp *ca = inet_csk_ca(sk);

	s32 missed_bin = 0;
	u32 now_us = bictcp_clock_us(sk);

	missed_bin = (now_us - ca->bin_end_us) / ca->bin_duration_us;

	if (missed_bin > 0) {
		ca->bin_total += missed_bin;
		ca->bin_end_us += missed_bin * ca->bin_duration_us;

		if (missed_bin >= SEARCH_TOTAL_BINS)
		    memset(ca->bin, 0, sizeof(ca->bin));

		else {
			while(missed_bin >= 0){
				ca->bin[(ca->bin_total - missed_bin) % SEARCH_TOTAL_BINS] = 0;
				missed_bin--;
			}
		}
	}
}

// Function to calculate delivered bytes for a window considering additional logic
static inline u64 search_calculate_window_bytes(struct sock *sk, u32 index)
{
	struct bictcp *ca = inet_csk_ca(sk);

	u32 i;
	u64 delivered_bytes = 0;

	for (i = index - SEARCH_BINS + 1; i <= index; i++)
		delivered_bytes += ca->bin[i % SEARCH_TOTAL_BINS];

	return delivered_bytes;
}

// Function to handle slow start exit condition
static void search_exit_slow_start(struct sock *sk, u32 rtt_us)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);

	u64 difference_bytes_acked = 0;
	u32 i = 0;
	u32 congestion_index = 0;
	u32 initial_rtt = 0;

	if (cwnd_rollback == 1) {
		u32 rollback_cwnd = tp->snd_cwnd;

		initial_rtt = ca->bin_duration_us * SEARCH_BINS * 10 / search_window_size_time;
		congestion_index = ca->bin_total - ((2 * initial_rtt) / ca->bin_duration_us);

		if (ca->bin_total - congestion_index > SEARCH_TOTAL_BINS)
			congestion_index = ca->bin_total - SEARCH_TOTAL_BINS + 1;

		for (i = congestion_index + 1 ; i <= ca->bin_total; i++)
			difference_bytes_acked += ca->bin[i % SEARCH_TOTAL_BINS];

		rollback_cwnd = difference_bytes_acked / tp->mss_cache;

		if (rollback_cwnd < tp->snd_cwnd)
			tp->snd_cwnd = max(TCP_INIT_CWND, tp->snd_cwnd - rollback_cwnd);
	}

	ca->stop_search = 1;
	tp->snd_ssthresh = tp->snd_cwnd;

}

// Function to perform interpolation and return the interpolated value
static u64 search_interpolate_delivered_bytes(struct sock *sk, u32 rtt_us, u32 curr_index, u32 prev_index, u64 left_bytes, u64 right_bytes) {

	struct bictcp *ca = inet_csk_ca(sk);
	u32 now_us = bictcp_clock_us(sk);
	u32 time_left = 0, time_right = 0;
	u64 interpolated_delv_bytes = 0;
	u32 proportion = 0;

	time_right = ca->bin_end_us - ((curr_index - prev_index) * ca->bin_duration_us);
	time_left = time_right - ca->bin_duration_us;

	// Check if the time difference for interpolation is non-zero to avoid division by zero
	if (right_bytes != left_bytes) {
		// Perform interpolation
		proportion = ((now_us - rtt_us) - time_left) / ca->bin_duration_us;
		interpolated_delv_bytes = left_bytes + proportion * (right_bytes - left_bytes);
	}

	return interpolated_delv_bytes;
}

//////////////////////// SEARCH ////////////////////////
static void search_update(struct sock *sk, u32 rtt_us)
{

	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);

	u64 acked_bytes = 0;
	u32 curr_index = 0;
	s32 prev_index = 0;
	u64 curr_delv_bytes = 0, prev_delv_bytes = 0;
	u64 prev_delv_bytes_under = 0,  prev_delv_bytes_over = 0;
	s32 norm_diff = 0;
	u32 now_us = bictcp_clock_us(sk);

	/* by receiving the first ack packet, initialize bin duration and bin end time */
	if (ca->bin_duration_us == 0) {
		ca->bin_duration_us = (rtt_us * search_window_size_time) / (SEARCH_BINS * 10);
		ca->bin_end_us = now_us + ca->bin_duration_us;
	}

	/* check if it's reached the bin boundary */
	if (now_us > ca->bin_end_us) {
		/* Check and update missed bins */
		search_update_missed_bins(sk);

		/* update delivered bytes in bin */
		acked_bytes = tp->bytes_acked - ca->prev_bytes_acked;
		ca->bin[ca->bin_total % SEARCH_TOTAL_BINS] = acked_bytes;
		ca->prev_bytes_acked = tp->bytes_acked;

		/* calculate indices for the current window and previous window after shifting by current RTT */
		curr_index = ca->bin_total;
		prev_index = ca->bin_total - (rtt_us/ca->bin_duration_us);

		/* check if there is enough bins after shift for computing previous window */
		if (prev_index >= SEARCH_BINS && SEARCH_TOTAL_BINS - (curr_index - prev_index) >= SEARCH_BINS) {

			/* Calculate delivered bytes for the current and previous windows */
			curr_delv_bytes = search_calculate_window_bytes(sk, curr_index);
			prev_delv_bytes_over = search_calculate_window_bytes(sk, prev_index);
			prev_delv_bytes_under = search_calculate_window_bytes(sk, prev_index - 1);

			if (do_intpld == 1)
				prev_delv_bytes = search_interpolate_delivered_bytes(sk, rtt_us, curr_index, prev_index, prev_delv_bytes_under, prev_delv_bytes_over);
			else
				prev_delv_bytes = prev_delv_bytes_over;


			if (prev_delv_bytes > 0) {
				norm_diff = ((2 * prev_delv_bytes) - curr_delv_bytes)*100 / (2 * prev_delv_bytes);

				/* check for exit condition */
				if ((2 * prev_delv_bytes) >= curr_delv_bytes && norm_diff >= search_thresh)
					search_exit_slow_start(sk, rtt_us);
			}
		}

		/* update bin-related parameters for the next bin */
		ca->bin_end_us = ca->bin_end_us + ca->bin_duration_us;
		ca->bin_total++;
		ca->bin[ca->bin_total % SEARCH_TOTAL_BINS] = 0;
	}
}
//////////////////////////////////////////////////////////////

static void bictcp_acked(struct sock *sk, const struct ack_sample *sample)
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

	//////////////////////// SEARCH ////////////////////////
	if (search > 0 && !ca->stop_search) {

		if (!tcp_in_slow_start(tp))
			ca->stop_search = 1;
		else
			/* implement search algorithm */
       		search_update(sk, delay);
	}

	/* hystart triggers when cwnd is larger than some threshold */
	if (!ca->found && tcp_in_slow_start(tp) && hystart &&
	    tp->snd_cwnd >= hystart_low_window)
		hystart_update(sk, delay);
}

static struct tcp_congestion_ops cubicsearch __read_mostly = {
	.init		= bictcp_init,
	.ssthresh	= bictcp_recalc_ssthresh,
	.cong_avoid	= bictcp_cong_avoid,
	.set_state	= bictcp_state,
	.undo_cwnd	= tcp_reno_undo_cwnd,
	.cwnd_event	= bictcp_cwnd_event,
	.pkts_acked	= bictcp_acked,
	.owner		= THIS_MODULE,
	.name		= "cubic_search",
};

static int __init cubicsearch_register(void)
{
	BUILD_BUG_ON(sizeof(struct bictcp) > ICSK_CA_PRIV_SIZE);

	/* Precompute a bunch of the scaling factors that are used per-packet
	 * based on SRTT of 100ms
	 */

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

	return tcp_register_congestion_control(&cubicsearch);
}

static void __exit cubicsearch_unregister(void)
{
	tcp_unregister_congestion_control(&cubicsearch);
}

module_init(cubicsearch_register);
module_exit(cubicsearch_unregister);

MODULE_AUTHOR("Sangtae Ha, Stephen Hemminger");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TCP CUBIC w/ SEARCH");
MODULE_VERSION("2.3");
