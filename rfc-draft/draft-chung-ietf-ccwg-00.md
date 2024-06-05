---
title: SEARCH -- a New Slow Start Algorithm for TCP and QUIC 
# abbrev: tcp-search 
docname: draft-search-slowstart
date: 2024-06-04
# date: 2024-05
# date: 2024

# stand_alone: true

ipr: trust200902
area: Web and Internet Transport
wg: congestcontrol
kw: Internet-Draft
cat: std
stream: IETF

coding: us-ascii
pi:    # can use array (if all yes) or hash here
#  - toc
#  - sortrefs
#  - symrefs
  toc: yes
  sortrefs:   # defaults to yes
  symrefs: yes

author:
      -
        ins: J. Chung 
        name: Jae Won Chung
        org: Viasat Inc
        abbrev: viasat
        street: 300 Nickerson Rd, #100
        city: Marlborough, MA
        code: 01752
        country: United States of America
        email: jaewon.chung@viasat.com
      -
        ins: M. Kachooei
        name: Maryam Ataei Kachooei
        org: Worcester Polytechnic Institute
        abbrev: WPI
        street: 100 Institute Rd
        city: Worcester, MA
        code: 01609
        country: United States of America 
        email: mataeikachooei@wpi.edu
      -
        ins: F. Li
        name: Feng Li
        org: Viasat Inc
        abbrev: viasat
        street: 300 Nickerson Rd, #100
        city: Marlborough, MA
        code: 01752
        country: United States of America
        email: feng.li@viasat.com
      -
        ins: M. Claypool
        name: Mark Claypool
        org: Worcester Polytechnic Institute
        abbrev: WPI
        street: 100 Institute Rd
        city: Worcester, MA
        code: 01609
        country: United States of America 
        email: claypool@cs.wpi.edu

normative:
#        - rfc2119
#        - rfc5226
#        - rfc5681
#        - rfc7942
  RFC2119:
  RFC5226:
  RFC5681:
  RFC7942:

informative:
  CKC24:
    title: "Improving QUIC Slow Start Behavior in Wireless Networks with SEARCH"
    author:
       - 
        ins: A. Cronin 
        name: Amber Cronin
        org: Worcester Polytechnic Institute
       -
        ins: M. Kachooei
        name: Maryam Ataei Kachooei
        org: Worcester Polytechnic Institute
       -
        ins: J. Chung 
        name: Jae Won Chung
        org: Viasat Inc
       -
        ins: F. Li 
        name: Feng Li 
        org: Viasat Inc 
       - 
        ins: B. Peters 
        name: Benjamin Peters
        org: Viasat Inc
       - 
        ins: M. Claypool
        name: Mark Claypool
        org: Worcester Polytechnic Institute
    seriesinfo: "Proceedings of the IEEE International Symposium on Local and Metropolitan Area Networks (LANMAN), Boston, MA, USA"
    target: https://web.cs.wpi.edu/~claypool/papers/quic-search-lanman-24/paper.pdf 
    date: 2024
  
  KCL24:
    title: "Improving TCP Slow Start Performance in Wireless Networks with SEARCH"
    author:
       -
        ins: M. Kachooei
        name: Maryam Ataei Kachooei
        org: Worcester Polytechnic Institute
       -
        ins: J. Chung 
        name: Jae Won Chung
        org: Viasat Inc
       -
        ins: F. Li 
        name: Feng Li 
        org: Viasat Inc 
       - 
        ins: B. Peters 
        name: Benjamin Peters
        org: Viasat Inc
       - 
        ins: J. Chung 
        name: Josh Chung
        org: Lexington Christian Academy 
       -
        ins: M. Claypool
        name: Mark Claypool
        org: Worcester Polytechnic Institute
    seriesinfo: "The World of Wireless, Mobile and Multimedia Networks conference (WoWMoM), Perth, Australia"
    #target: https://web.cs.wpi.edu/~claypool/papers/search-wowmom-24/paper.pdf 
    date: 2024

  HYSTART:
    title: "Taming the Elephants: New TCP Slow Start"
    author:
       -
        ins: S. Ha
        name: Sangtae Ha
       -
        ins: I. Rhee
        name: Injong Rhee
    seriesinfo: "Computer Networks vol. 55, no. 9, pp. 2092-2110, DOI 10.1016/j.comnet.2011.01.014"
    target: https://doi.org/10.1016/j.comnet.2011.01.014
    date: 2024

entity:
        SELF: "[RFCXXXX]"

# --- note_IESG_Note

--- abstract

TCP slow start is designed to ramp up to the network congestion point
quickly, doubling the congestion window each round-trip time until the
congestion point is reached, whereupon TCP exits the slow start phase.
Unfortunately, the default Linux TCP slow start implementation -- TCP
Cubic with HyStart {{HYSTART}} -- can cause premature exit from slow
start, especially over wireless links, degrading link utilization.
However, without HyStart, TCP exits slow start too late, causing
unnecessary packet loss.  To improve TCP slow start performance, this
document proposes using the Slow start Exit At Right CHokepoint
(SEARCH) algorithm {{KCL24}} where the TCP sender determines the
congestion point based on acknowledged deliveries -- specifically, the
sender computes the delivered bytes compared to the expected delivered
bytes, smoothed to account for link latency variation and normalized
to accommodate link capacities, and exits slow start if the delivered
bytes are lower than expected.  We implemented SEARCH as a Linux
kernel v5.16 module and evaluated it over WiFi, 4G/LTE, and low earth
orbit (LEO) and geosynchronous (GEO) satellite links.  Analysis of the
results show that the SEARCH reliably exits from slow start after the
congestion point is reached but before inducing packet loss.

--- middle


Introduction        {#problems}
============

The TCP slow start mechanism starts sending data rates cautiously yet
rapidly increases towards the congestion point, approximately doubling
the congestion window (cwnd) each round-trip time (RTT).
Unfortunately, default implementations of TCP slow start, such as TCP
Cubic with HyStart {{HYSTART}} in Linux, often result in a premature
exit from the slow start phase, or, if HyStart is disabled, excessive
packet loss upon overshooting the congestion point.  Exiting slow
start too early curtails TCP's ability to capitalize on unused link
capacity, a setback that is particularly pronounced in high
bandwidth-delay product (BDP) networks (e.g., GEO satellites) where
the time to grow the congestion window to the congestion point is
substantial.  Conversely, exiting slow start too late overshoots the
link's capacity, inducing necessary congestion and packet loss,
particularly problematic for links with large (bloated) bottleneck
queues.

To determine the slow start exit point, we propose that the TCP sender
monitor the acknowledged delivered bytes in an RTT and compare that to
what is expected based on the bytes acknowledged as delivered during
the previous RTT.  Large differences between delivered bytes and
expected delivered bytes is then the indicator that slow start has
reached the network congestion point and the slow start phase should
exit.  We call our approach the Slow start Exit At Right CHokepoint
(SEARCH) algorithm.  SEARCH is based on the principle that during slow
start, the congestion window expands by one maximum segment size (MSS)
for each acknowledgment (ACK) received, prompting the transmission of
two segments and effectively doubling the sending rate each RTT.
However, when the network surpasses the congestion point, the delivery
rate does not double as expected, signaling that the slow start phase
should exit.  Specifically, the current delivered bytes should be
twice the delivered bytes one RTT ago.  To accommodate links with a
wide range in capacities, SEARCH normalizes the difference based on
the current delivery rate and since link latencies can vary over time
independently of data rates (especially for wireless links), SEARCH
smooths the measured delivery rates over several RTTs.

This document describes the current version of the SEARCH algorithm,
version 3. Active work on the SEARCH algorithm is continuing.

This document is organized as follows: Section 2 provides terminology
and definitions relevant used throughout this document; Section 3
describes the SEARCH algorithm in detail; Section 4 provides
justification for algorithm settings; Section 5  describes the
implementation status; Section 6 describes security considerations;
Section 7 notes that there are no IANA considerations; Section 8
closes with acknowledgments; and Section 9 provides references.


Terminology and Definitions
=========================

The key words "MUST", "MUST NOT", "REQUIRED", "SHALL", "SHALL NOT",
"SHOULD", "SHOULD NOT", "RECOMMENDED", "MAY", and "OPTIONAL" in this
document are to be interpreted as described in RFC 2119, BCP 14
{{RFC2119}} and indicate requirement levels for compliant CoAP
implementations.

In this document, the term "byte" is used in its now customary sense
as a synonym for "octet".

*ACK:* a TCP acknowledgement.

*bins:* the aggregate (total) of acknowledged delivery bytes over a
small time window.

*congestion window (cwnd):* A TCP state variable that limits the
 amount of data a TCP can send. At any given time, a TCP flow MUST NOT
 send data with a sequence number higher than the sum of the highest
 acknowledged sequence number and the minimum of the cwnd and receiver
 window.

*norm_diff:* the normalized difference in current delivered bytes and
 previously delivered bytes.

*round-trip time (RTT):* the round-trip time for a segment sent until
 the acknowledgement is received.

*THRESH:* the norm_diff value above which SEARCH will consider the
 congestion point to be reached and the slow start phase exits.


SEARCH Algorithm 
================

The concept that during the slow start phase, the delivered bytes
should double each RTT until the congestion point is reached is core
to the SEARCH algorithm. In SEARCH, when the bytes delivered one RTT
prior is half the bytes delivered currently, the bitrate is not yet at
capacity, whereas when the bytes delivered prior are more than half
the bytes delivered currently, the link capacity has been reached and
TCP exits slow start.

One challenge in monitoring delivered data across multiple RTTs is
latency variability for some links. Variable latency in the absence of
congestion - common in some wireless links - can cause RTTs to differ
over time even when the network is not yet at the congestion point.
This variability complicates comparing delivered bytes one RTT prior
to those delivered currently in that a lowered latency can make it
seem like the total bytes delivered currently is too low compared to
the total delivered one RTT ago, making it seem like the link is at
the congestion point when it is not.

To counteract link latency variability, SEARCH tracks delivered data
over several RTTs in a sliding window providing a more stable basis
for comparison.  Since tracking individual segment delivery times is
prohibitive in terms of memory use, the data within the sliding window
is aggregated over bins representing small, fixed time periods. The
window then slides over bin-by-bin, rather than sliding every
acknowledgement (ACK), reducing both the computational load (since
SEARCH only triggers at the bin boundary) and the memory requirements
(since delivered byte totals are kept for a bin-sized time interval
instead of for each segment).

The SEARCH algorithm (that runs on the TCP sender only) is shown
below.

The parameters in CAPS (lines 1-6) are constants, with the INITIAL_RTT
(line 1) obtained via the first round-trip time measured in the TCP
connection.

The variables in Initialization (lines 7-9) are set once, upon
establishment of a TCP connection.

The variable *now* on lines 9, 10 and 24 is the current system time
when the code is called.

The variable sequence_num and rtt above line 10 are obtained upon
arrival of an acknowledgement from the receiver.

The variable *cwnd* on line 41 is the current congestion window.

Lines 1-6 set the predefined parameters for the SEARCH algorithm.  The
window size (WINDOW SIZE) is 3.5 times the initial RTT.  The delivered
bytes over a SEARCH window is approximated using 10 (W) bins, with an
additional 15 additional bins (EXTRA_BINS) bins (for a total of 25
(NUM_BINS)) to allow comparison of the current delivered bytes to the
previously delivered bytes one RTT earlier.  The bin duration
(BIN_DURATION) is the window size divided by the number of
bins. The threshold (THRESH) is set to 0.35 and is the upper bound of
the permissible difference between the previously delivered bytes and
the current delivered bytes (normalized) above which slow start exits.

Lines 7 to 9 do one-time initialization of SEARCH variables when a TCP
connection is established.

After initialization, SEARCH only acts when acknowledgements (ACKS) are
received and even then, only when the current time (*now*) has passed
the end of the latest bin boundary (stored in the variable bin_end).
This check happens on line 10 and if the bin boundary is passed, the
bin statistics are updated in the function update_bins(), lines 24-31.

In update_bins(), under most TCP connections, the time (*now*) is in
the successive bin, but in some cases (such as during an RTT spike or
a TCP connection without data to send), more than one bin boundary may
have been passed.  Line 24 computes how many bins have been passed.
In lines 26 to 28, for each bin passed, the bin\[\] variable is set to
0.  For the latest bin, the delivered bytes is updated by taking the
latest sequence number (from the ACK) and subtracting the previously
recorded sequence number in the last bin boundary (line 30).  In line
31, the current sequence number is stored (in prev_seq_num) for
computing the delivered bytes the next time a bin boundary is passed.

Once the bins are updated, lines 12-14 check if enough bins have been
filled to run SEARCH.  This requires at least W (10) bins (i.e., on
SEARCH window worth of bytes-delivered data), but also enough bins to
shift back by an RTT to compute a window (10) bins one RTT ago, too.

If there is enough bin data to run SEARCH, lines 15 and 17 compute the
current and previously delivered bytes over a window (W) of bins,
respectively.  This sum is computed in the function sum_bins(), lines
32-38. For previously delivered bytes, shifting by an RTT may mean the
SEARCH window lands between bin boundaries, so the sum is interpolated
by the fraction of each of the end bins.

The function sum_bins(), idx1 and idx2 are the indices into the
bin\[\] array for the start and end of the bin summation and as
explained above, fraction is the proportion (from 0 to 1) of the end
bins to use in the summation.  In lines 33-35, the summation loops
through the bin\[\] array for the middle bins, modulo the number of
bins allocated (NUM_BINS) and then adds the fractions of the end bins
in lines 36 and 37.

Once bin sums are tallied, line 18 calculates the difference between
the expected delivered bytes (2 * prev_delv) and the current delivered
bytes (curr_delv), normalized by dividing by the expected delivered
bytes.  In line 19, this normalized difference value (norm_diff) is
compared to the threshold (THRESH).  If norm_diff is larger than
THRESH, that means the current delivered bytes is lower than expected
(i.e., they didn't double over the previous RTT) and slow start exits.
Slow start exit is handled by the function exit_slow_start() on lines
39-42.

In slow_start_exit(), since SEARCH had to pass the congestion point in
order to ascertain that the chokepoint has been reached, it can reduce
the congestion window (cwnd) to instead be at the congestion point
instead of above it.  Detection of the chokepoint condition is delayed
by almost exactly two RTTs, so lines 39 and 40 compute the extra bytes
(past the congestion point) that have been added to the congestion
window and these are subtracted from the congestion window (line 41).
Setting the slow start threshold (ssthresh) to the congestion window
(cwnd) effectively exits slow start.

SEARCH 3.0 ALGORITHM

~~~~
Parameters:
1: WINDOW_SIZE = INITIAL_RTT x 3.5  
2: W = 10  
3: EXTRA_BINS = 15
4: NUM_BINS = W + EXTRA_BINS  
5: BIN_DURATION = WINDOW_SIZE / W  
6: THRESH = 0.35  

Initialization():
7: bin[NUM_BINS] = {}
8: curr_idx = -1 
9: bin_end = *now* + BIN_DURATION  

ACK_arrived(sequence_num, rtt):
  // Check if passed bin boundary.
10: if (*now* > bin_end) then  
11:   update_bins()

      // Check if enough data for SEARCH.
12:   prev_idx = curr_idx - (rtt / BIN_DURATION)
13:   if (prev_idx >= W) and
14:      (curr_idx - prev_idx) <= EXTRA_BINS then  

        // Run SEARCH check.
15:     curr_delv = sum_bins(curr_idx - W, curr_idx)
16:     fraction = rtt mod BIN_DURATION
17:     prev_delv = sum_bins(prev_idx - W, prev_idx, fraction)
18:     norm_diff = (2 * prev_delv - curr_delv) / (2 * prev_delv)
19:     if (norm_diff >= THRESH) then
20:       exit_slow_start()
21:     end if

22:   end if // Enough data for SEARCH.

23: end if // Each ACK.

// Update bin statistics, accounting for cases where more
// than one bin boundary might have been passed.
update_bins():
24: passed_bins = (*now* - bin_end) / BIN_DURATION + 1
25: bin_end += passed_bins * BIN_DURATION
26: for i = curr_idx to (curr_idx + passed_bins)
27:   bin[i mod NUM_BINS] = 0
28: end for
29: curr_idx += passed_bins
30: bin[curr_idx mod NUM_BINS] = sequence_num - prev_seq_num
31: prev_seq_num = sequence_num

// Add up bins, interpolating a fraction of each bin on the
// end (default is 0).
sum_bins(idx1, idx2, fraction = 0):
32: sum = 0
33: for i = idx1+1 to idx2-1
34:   sum += bin[i mod NUM_BINS]
35: end for
36: sum += bin[idx1] * fraction
37: sum += bin[idx2] * (1-fraction)
38: return sum

// Exit slow start by setting cwnd and ssthresh.
exit_slow_start():
39: cong_idx = curr_idx - 2 * INITIAL_RTT / BIN_DURATION
40: overshoot = sum_bins(cong_idx, curr_idx)
41: cwnd -= overshoot
42: ssthresh = cwnd
~~~~


SEARCH Parameters
=================

This section provides justification and some sensitivity analysis for
key SEARCH algorithm constants.

**Window Size (WINDOW_SIZE)**

The SEARCH window smooths over RTT fluctuations in a connection that
are unrelated to congestion. The window size must be large enough to
encapsulate meaningful link variation, yet small in order to allow
SEARCH to respond near when slow start reaches link capacity. In order
to determine an appropriate window size, we analyzed RTT variation
over time for GEO, LEO, and 4G LTE links for TCP during slow start.
See {{KCL24}} for details.

The SEARCH window size needs to be large enough to capture the
observed periodic oscillations in the RTT values. In order to
determine the oscillation period, we use a Fast Fourier Transform
(FFT) to convert measured RTT values from the time domain to the
frequency domain.  For GEO satellites, the primary peak is at 0.5 Hz,
meaning there is a large, periodic cycle that occurs about every 2
seconds.  Given the minimum RTT for a GEO connection of about 600 ms,
this means the RTT cycle occurs about every 3.33 RTTs.  Thus, a window
size of about 3.5 times the minimum RTT should smooth out the latency
variation for this type of link.

While the RTT periodicity for LEO links is not as pronounced as they
are in GEO links, the analysis yields a similar window size.  The FFT
of LEO RTTs has a dominant peak at 10 Hz, so a period of about 0.1
seconds. With LEO's minimum RTT of about 30 ms, the period is about
3.33 RTTs, similar to that for GEO. Thus, a window size of about 3.5
times the minimum RTT should smooth out the latency variation for this
type of link, too.

Similarly to the LEO link, the LTE network does not have a strong RTT
periodicity.  The FFT of LTE RTTs has a dominant peak at 6 Hz, with a
period of about 0.17 seconds.  With the minimum RTT of the LTE network
of about 60 ms, this means a window size of about 2.8 times the
minimum RTT is needed.  A SEARCH default of 3.5 times the minimum RTT
exceeds this, so should smooth out the variance for this type of link
as well.

** Threshold (THRESH) **

The threshold (THRESH) determines when the difference between the
bytes delivered currently and the bytes delivered during the previous
RTT is large enough to exit the slow start phase.  A small threshold
is desirable to exit slow start close to the `at capacity' point
(i.e., without overshooting too much), but the threshold must be large
enough not to trigger an exit from slow start prematurely due to noise
in the measurements.

During slow start, the congestion window doubles each RTT.  In ideal
conditions and with an initial cwnd of 1 (1 is used as an example, but
typical congestion windows start at 10 or more), this results in a
sequence of delivered bytes that follows a doubling pattern (1, 2, 4,
8, 16, ...). Once the link capacity is reached, the delivered bytes
each RTT cannot increase despite cwnd growth.  For example, consider a
window that is 4x the size of the RTT.  After 5 RTTs, the current
delivered window comprises 2, 4, 8, 16, while the previous delivered
window is 1, 2, 4, 8.  The current delivered bytes is 30, exactly
double the bytes delivered in the previous window.  Thus, SEARCH would
compute the normalized difference as zero.

Once the cwnd ramps up to meet full link capacity, the delivered bytes
plateau.  Continuing the example, if the link capacity is reached when
cwnd is 16, the delivered bytes growth would be 1, 2, 4, 8, 16, 16.
The current delivered window is 4+8+16+16 = 44, while the previously
delivered window is 2+4+8+16 = 30.  Here, the normalized difference
between 2x the previously delivered window and the current delivered
window is about (60-44)/60 = 0.27.  After 5 more RTTs, the previous
delivered and current delivered bytes would both be 16 + 16 + 16 + 16
= 64 and the normalized difference would be (128 - 64) / 64 = 0.5.

Thus, the norm values typically range from 0 (before the congestion
point) to 0.5 (well after the congestion point) with values between 0
and 0.5 when the congestion point has been reached but not surpassed
by the full window.

To generalize this relationship, the theoretical underpinnings of this
behavior can be quantified by integrating the area under the
congestion window curve for a closed-form equation for both the
current delivered bytes (curr_delv) and the previously delivered bytes
(prev_delv).  The normalized difference can be computed based on the
RTT round relative to the "at capacity" round.  While SEARCH seeks to
detect the "at capacity" point as soon as possible after reaching it,
it must also avoid premature exit in the case of noise on the link.
The 0.35 threshold value chosen does this and can be detected about 
2 RTTs of reaching capacity.

**Number of Bins (NUM_BINS)**

Dividing the delivered byte window into bins reduces the sender's
memory load by aggregating data into manageable segments instead of
tracking each packet.  This approach simplifies data handling and
minimizes the frequency of window updates, enhancing sender
efficiency.  However, more bins provide more fidelity to actual
delivered byte totals and allow SEARCH to make decisions (i.e.,
compute if it should exit slow start) more often, but require more
memory for each flow.  The sensitivity analysis previously conducted
aimed to identify the impact of the number of bins used by SEARCH and
the ability to exit slow start in a timely fashion.

Using a window size of 3.5x the initial RTT and a threshold of 0.35,
we varied the number of bins from 5 to 40 and observed the impact on
SEARCH's performance over GEO, LEO and 4G LTE downloads.  For all
three link types, a bin size 10 of provides nearly identical
performance as SEARCH running with more bins, while 10 also minimizes
early exits from slow start while having an "at chokepoint" percentage
that is close to the maximum.


Deployment and Performance Evaluation
=====================================

Evaluation of hundreds of downloads of TCP with SEARCH across GEO,
LEO, and 4G LTE network links compared to TCP with HyStart and TCP
without HyStart shows SEARCH almost always exits after capacity has
been reached but before packet loss has occurred.  This results in
capacity limits being reached quickly while avoiding inefficiencies
caused by lost packets.

Evaluation of a SEARCH implementation in an open source QUIC library
(QUICly) over an emulated GEO satellite link validates the
implementation, illustrating how SEARCH detects the congestion point
and exits slow start before packet loss occurs.  Evaluation over a
commercial GEO satellite link shows SEARCH can provide a median
improvement of up to 3 seconds (14%) compared to the baseline by
limiting cwnd growth when capacity is reached and delaying any packet
loss due to congestion.

Details can be found at {{KCL24}} and {{CKC24}}.


Implementation Status
=====================

This section records the status of known implementations of the
algorithm defined by this specification at the time of posting of this
Internet-Draft, and is based on a proposal described in [RFC7942]. The
description of implementations in this section is intended to assist
the IETF in its decision processes in progressing drafts to RFCs.
Please note that the listing of any individual implementation here
does not imply endorsement by the IETF.  Furthermore, no effort has
been spent to verify the information presented here that was supplied
by IETF contributors.  This is not intended as, and must not be
construed to be, a catalog of available implementations or their
features.  Readers are advised to note that other implementations may
exist.

According to [RFC7942], "this will allow reviewers and working groups
to assign due consideration to documents that have the benefit of
running code, which may serve as evidence of valuable experimentation
and feedback that have made the implemented protocols more mature. It
is up to the individual working groups to use this information as they
see fit".

As of the time of writing, the following implementations of SEARCH
have been publicly released:

Linux TCP

Source code URL:

<https://github.com/Project-Faster/tcp_ss_search.git>

Source: WPI
Maturity: production
License: GPL?
Contact: claypool@cs.wpi.edu
Last updated: May 2024

QUIC

Source code URLs:

<https://github.com/Project-Faster/quicly/tree/generic-slowstart>
<https://github.com/AmberCronin/quicly>
<https://github.com/AmberCronin/qperf>


Source: WPI
Maturity: production
License: BSD-style
Contact: claypool@cs.wpi.edu
Last updated: May 2024



Security Considerations
=======================

This proposal makes no changes to the underlying security of transport
protocols or congestion control algorithms.  SEARCH shares the same
security considerations as the existing standard congestion control
algorithm [RFC5681].


IANA Considerations
===================

This document has no IANA actions. Here we are using that phrase,
suggested by [RFC5226], because SEARCH does not modify or extend the
wire format of any network protocol, nor does it add new dependencies
on assigned numbers.  SEARCH involves only a change to the slow start
part of the congestion control algorithm of a transport sender, and
does not involve changes in the network, the receiver, or any network
protocol.

Note to RFC Editor: this section may be removed on publication as an RFC.


Acknowledgements
================

Much of the content of this draft is the result of discussions with
the Congestion Control Research Group (CCRG) at WPI
<https://web.cs.wpi.edu/~claypool/ccrg>. In addition, feedback and
discussions of early versions of SEARCH with the technical group at
Viasat has been invaluable.


References
==============


--- back

Historical Note {#compat}
===============


<!--  LocalWords:  SEARCH: a New Slow Start Algorithm for TCP and QUIC 
-->
<!--  LocalWords:  
-->
