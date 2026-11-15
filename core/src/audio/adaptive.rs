//! Choosing the voice bitrate: a rider's fixed number, or one that follows the
//! link.
//!
//! [`super::bandwidth`] fits whatever this module chooses inside the server's
//! allowance, so nothing here needs to know about caps: the server's limit is
//! always applied afterwards and always wins.
//!
//! # What "Auto" listens to
//!
//! Two facts about *our outgoing voice*, each taken fresh:
//!
//! - **Loss**: the server's count of the packets it received from us —
//!   `from_client` in `UserStats` for our own session, which the roster already
//!   polls. **The difference between two polls**, through [`LinkTracker`], and
//!   not the ratio the roster shows. That ratio is over the server's rolling
//!   window, and the window is 300 s on the servers seen so far. The first
//!   version of this module read it: ten seconds in a tunnel walked the rate to
//!   the floor within a minute, and the window kept reporting the tunnel for
//!   five more.
//! - **Queueing delay**: our latest UDP round trip above the lowest one of the
//!   last two minutes. **Not the ping in `UserStats`**, which is the mean over
//!   the whole session (see `session::pings`): after an hour of good link it
//!   barely moves for a bad minute, and after a bad spell it stays high long
//!   after the link recovered. The first version read that too.
//!
//! Delay matters as much as loss because cellular links rarely drop under
//! load. Operators buffer deeply, so a link asked for more than it can carry
//! says so in seconds of delay rather than in lost packets (Jiang et al.,
//! "Tackling Bufferbloat in 3G/4G Networks", 2012).
//!
//! # The rules, and why they hold back
//!
//! The loss bands are Google Congestion Control's (draft-ietf-rmcat-gcc): under
//! 2% a link may climb, from 2% to 10% it holds, above 10% it steps down.
//! **Moderate loss is held through on purpose.** On a moving phone most loss is
//! coverage, not congestion, and the packet rate is fixed by the frame length,
//! so fewer bits per packet do not make a dead spot lose fewer packets. For the
//! same reason near-total loss ([`LOSS_OUTAGE`]) is an outage — a tunnel, a
//! hole between masts — and holds rather than steps down: there is nothing a
//! lower rate would get through.
//!
//! Down is quick and up is slow, as in every rate controller that has to live
//! on a shared link: a step down costs a little quality for a few seconds, a
//! step up into congestion costs words. One ping spike — a handover, a
//! retransmission — is not a queue, so a delay has to show on two pings in a
//! row.
//!
//! Over the TCP tunnel the server's counters stand still (they count UDP), so
//! "Auto" is blind to loss there and holds at most the old fixed default.
//!
//! # What is deliberately not consulted: other riders' links
//!
//! It is tempting to lower the rate when a *listener* is struggling, so that a
//! rider at the edge of coverage can hear the others. Considered on 2026-11-15
//! and not done, for reasons worth keeping:
//!
//! - The only figure the server gives about another rider's reception is loss,
//!   over the same 300 s window, and their ping is a session mean. On a phone,
//!   loss means coverage, where a lower rate does not help; congestion, where it
//!   would, shows as delay, which is not visible for anybody else.
//! - The worst of many links is nearly always bad. If each rider spends 5% of a
//!   ride somewhere weak, then with ten riders somebody is weak about 40% of the
//!   time, and following the worst means sitting near the floor. This is the
//!   "loss path multiplicity" problem of multicast congestion control
//!   (Bhattacharyya, Towsley, Kurose, 1998).
//! - Those loss figures are reported by the listener's own client, so one
//!   broken client could pull everybody down. Deafened listeners receive no
//!   voice, listeners in linked channels are not visible at all, and riders on
//!   the official client would not adapt in return anyway.
//! - On cellular the uplink, not the downlink, is usually the limiting
//!   direction, so the struggling rider's weak side is their own transmission,
//!   which this module already follows.
//!
//! If it is ever wanted, the signal should come from the listener itself — its
//! own queueing delay and buffer underruns — as a rare, standing hint through
//! `session::peers`, and it should only lower the ceiling to the old default,
//! never below.

use std::collections::VecDeque;
use std::time::{Duration, Instant};

/// The rates "Auto" moves between, lowest first, in bits per second.
///
/// Measured on real speech with the engine's FEC setting: 16 kbit/s is
/// mediumband (6 kHz), 24 is super-wideband (12 kHz), and from 32 Opus codes
/// the full band. Above 48 mono speech gains little a rider can hear — and
/// through a helmet microphone on the hands-free profile, whose band ends at 4
/// or 8 kHz, the extra bits mostly go to a band that was never captured.
pub const AUTO_RUNGS: [u32; 6] = [16_000, 20_000, 24_000, 32_000, 40_000, 48_000];

/// Where "Auto" starts: one rung above the old fixed default.
pub const AUTO_START_BPS: u32 = 32_000;

/// The ceiling while voice is tunnelled over TCP: the old fixed default,
/// because over TCP "Auto" cannot see loss at all.
pub const TUNNELLED_CEILING_BPS: u32 = 24_000;

/// Range a rider may fix the bitrate to, `(min, max, step)`.
///
/// Wider than "Auto" on purpose: a rider on a server whose admin raised the
/// allowance may want more, and anything above the allowance is cut down to it
/// by [`super::bandwidth::fit`], so a high setting cannot make a rider
/// inaudible. The floor is [`super::bandwidth::MIN_AUDIO_BPS`]. In-band FEC
/// survives the whole range: with the engine's protection setting Opus stays in
/// a mode that carries it even at 128 kbit/s.
pub const FIXED_MIN_BPS: u32 = 8_000;
pub const FIXED_MAX_BPS: u32 = 128_000;
pub const FIXED_STEP_BPS: u32 = 4_000;

/// Loss, 0 to 1, below which a link may climb. Google Congestion Control's.
pub const LOSS_CLEAN: f32 = 0.02;
/// Loss above which "Auto" steps down. Also GCC's.
pub const LOSS_STEP_DOWN: f32 = 0.10;
/// Loss at or above which it is an outage rather than congestion: hold.
pub const LOSS_OUTAGE: f32 = 0.5;
/// Packets a loss figure has to cover before it means anything: three seconds
/// of speech. Between phrases only a ping goes out every few seconds, and one
/// of those lost is not a 100% loss.
pub const MIN_LOSS_SAMPLE: u32 = 150;

/// Queueing delay, in ms, at which "Auto" steps down.
pub const QUEUE_STEP_DOWN_MS: f32 = 150.0;
/// Queueing delay, in ms, under which a link counts as clean.
pub const QUEUE_CLEAN_MS: f32 = 50.0;
/// How long the lowest round trip is remembered as the empty-queue baseline.
pub const BASE_RTT_WINDOW: Duration = Duration::from_secs(120);

/// How long a link must stay clean before "Auto" climbs one rung.
pub const CLEAN_BEFORE_UP: Duration = Duration::from_secs(30);
/// The shortest gap between two steps down, so that two reports of the same
/// trouble cost one rung rather than two.
pub const DOWN_EVERY: Duration = Duration::from_secs(10);

/// What the rider asked for.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BitrateMode {
    /// Follow the link.
    Auto,
    /// Always this many bits per second, still inside the server's allowance.
    Fixed(u32),
}

impl BitrateMode {
    /// A fixed rate, clamped and rounded to the range the setting offers.
    pub fn fixed(bps: u32) -> Self {
        let clamped = bps.clamp(FIXED_MIN_BPS, FIXED_MAX_BPS);
        let steps = (clamped - FIXED_MIN_BPS + FIXED_STEP_BPS / 2) / FIXED_STEP_BPS;
        Self::Fixed(FIXED_MIN_BPS + steps * FIXED_STEP_BPS)
    }
}

/// New evidence about our outgoing link. Each figure is `None` when there is
/// nothing new to say about it, which is neither good news nor bad.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct LinkReport {
    /// Share of our packets, 0 to 1, lost since the last figure.
    pub loss_up: Option<f32>,
    /// Round trip above the recent lowest, in ms: the queue we are waiting in.
    pub queue_ms: Option<f32>,
    /// Whether voice is going over the TCP tunnel to any server.
    pub tunnelled: bool,
}

/// Turns what one server says about our connection into [`LinkReport`]
/// figures. One per server: the counts and the baseline are per connection.
#[derive(Debug, Default, Clone)]
pub struct LinkTracker {
    /// `(received, lost)` as of the last poll.
    totals: Option<(u32, u32)>,
    /// Received and lost since the last figure was given out.
    pending: (u32, u32),
    /// Round trips of the last [`BASE_RTT_WINDOW`].
    rtts: VecDeque<(Instant, f32)>,
    last_rtt: Option<f32>,
    last_queue: Option<f32>,
    /// Whether this server's voice is going over TCP.
    pub tunnelled: bool,
}

impl LinkTracker {
    /// Takes the server's cumulative counts of our packets — received (good
    /// plus late) and lost — and returns the loss since the last figure once
    /// enough packets have gone by to mean something.
    pub fn on_totals(&mut self, received: u32, lost: u32) -> Option<f32> {
        let (r0, l0) = self.totals.replace((received, lost))?;
        if received < r0 || lost < l0 {
            // A new connection counts from zero again.
            self.pending = (0, 0);
            return None;
        }
        self.pending.0 = self.pending.0.saturating_add(received - r0);
        self.pending.1 = self.pending.1.saturating_add(lost - l0);
        let sent = self.pending.0.saturating_add(self.pending.1);
        if sent < MIN_LOSS_SAMPLE {
            return None;
        }
        let loss = self.pending.1 as f32 / sent as f32;
        self.pending = (0, 0);
        Some(loss)
    }

    /// Takes our latest UDP round trip and returns the queueing delay above
    /// the recent lowest, once two pings in a row agree on it.
    ///
    /// The session republishes the latest ping every second, so a value equal
    /// to the last one is the same ping and is ignored; zero means none yet.
    pub fn on_rtt(&mut self, now: Instant, rtt_ms: f32) -> Option<f32> {
        if rtt_ms.is_nan() || rtt_ms <= 0.0 || Some(rtt_ms) == self.last_rtt {
            return None;
        }
        self.last_rtt = Some(rtt_ms);
        self.rtts.push_back((now, rtt_ms));
        while self
            .rtts
            .front()
            .is_some_and(|(t, _)| now.saturating_duration_since(*t) > BASE_RTT_WINDOW)
        {
            self.rtts.pop_front();
        }
        let base = self
            .rtts
            .iter()
            .map(|(_, r)| *r)
            .fold(f32::INFINITY, f32::min);
        let queue = rtt_ms - base;
        // The smaller of this and the last: one spike is not a queue.
        let confirmed = self.last_queue.map(|q| q.min(queue));
        self.last_queue = Some(queue);
        confirmed
    }
}

/// The "Auto" state: which rung, and since when the link has been clean.
#[derive(Debug, Clone)]
pub struct AutoBitrate {
    rung: usize,
    clean_since: Option<Instant>,
    last_down: Option<Instant>,
}

impl Default for AutoBitrate {
    fn default() -> Self {
        Self {
            rung: rung_of(AUTO_START_BPS),
            clean_since: None,
            last_down: None,
        }
    }
}

fn rung_of(bps: u32) -> usize {
    AUTO_RUNGS.iter().rposition(|r| *r <= bps).unwrap_or(0)
}

impl AutoBitrate {
    /// The rate "Auto" currently wants, before any server cap.
    pub fn bps(&self) -> u32 {
        AUTO_RUNGS[self.rung]
    }

    /// Takes new evidence and returns the rate to use from now on.
    pub fn update(&mut self, now: Instant, link: LinkReport) -> u32 {
        if link.tunnelled {
            // A ceiling rather than a step: over TCP there is no loss figure
            // to wait for, and the old default is the known quantity.
            let ceiling = rung_of(TUNNELLED_CEILING_BPS);
            if self.rung > ceiling {
                self.rung = ceiling;
                self.last_down = Some(now);
            }
            self.clean_since = None;
            return self.bps();
        }

        let loss = link.loss_up;
        let queue = link.queue_ms;

        let congested = loss.is_some_and(|l| l > LOSS_STEP_DOWN && l < LOSS_OUTAGE)
            || queue.is_some_and(|q| q >= QUEUE_STEP_DOWN_MS);
        if congested {
            self.clean_since = None;
            let may_step = self
                .last_down
                .is_none_or(|t| now.saturating_duration_since(t) >= DOWN_EVERY);
            if may_step && self.rung > 0 {
                self.rung -= 1;
                self.last_down = Some(now);
            }
            return self.bps();
        }

        // Moderate loss, an outage, or some queueing: hold, and start the
        // clean count again.
        let unclean =
            loss.is_some_and(|l| l >= LOSS_CLEAN) || queue.is_some_and(|q| q >= QUEUE_CLEAN_MS);
        if unclean {
            self.clean_since = None;
            return self.bps();
        }
        if loss.is_none() && queue.is_none() {
            return self.bps();
        }

        let since = *self.clean_since.get_or_insert(now);
        if now.saturating_duration_since(since) >= CLEAN_BEFORE_UP
            && self.rung + 1 < AUTO_RUNGS.len()
        {
            self.rung += 1;
            // Each rung has to earn the next one afresh.
            self.clean_since = Some(now);
        }
        self.bps()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn loss(l: f32) -> LinkReport {
        LinkReport {
            loss_up: Some(l),
            ..Default::default()
        }
    }

    fn queue(q: f32) -> LinkReport {
        LinkReport {
            queue_ms: Some(q),
            ..Default::default()
        }
    }

    const CLEAN: LinkReport = LinkReport {
        loss_up: Some(0.0),
        queue_ms: Some(5.0),
        tunnelled: false,
    };

    fn at(t0: Instant, secs: u64) -> Instant {
        t0 + Duration::from_secs(secs)
    }

    #[test]
    fn starts_one_rung_above_the_old_default() {
        assert_eq!(AutoBitrate::default().bps(), AUTO_START_BPS);
        assert!(AUTO_START_BPS > super::super::bandwidth::preferred_bps());
    }

    #[test]
    fn a_clean_link_climbs_slowly_to_the_top() {
        let t0 = Instant::now();
        let mut a = AutoBitrate::default();
        // Reports every 5 s, as the polls deliver them.
        let mut last = a.bps();
        for i in 0..=6 {
            last = a.update(at(t0, 5 * i), CLEAN);
        }
        // 30 s of clean link buys exactly one rung.
        assert_eq!(last, 40_000);
        for i in 7..=40 {
            last = a.update(at(t0, 5 * i), CLEAN);
        }
        assert_eq!(last, *AUTO_RUNGS.last().unwrap());
    }

    #[test]
    fn heavy_loss_steps_down_at_once_but_not_every_report() {
        let t0 = Instant::now();
        let mut a = AutoBitrate::default();
        assert_eq!(a.update(t0, loss(0.2)), 24_000);
        assert_eq!(a.update(at(t0, 5), loss(0.2)), 24_000);
        assert_eq!(a.update(at(t0, 10), loss(0.2)), 20_000);
    }

    #[test]
    fn moderate_loss_holds_without_climbing() {
        // GCC's middle band: on a phone this is coverage, and a lower rate
        // would not get more through.
        let t0 = Instant::now();
        let mut a = AutoBitrate::default();
        for i in 0..40 {
            assert_eq!(a.update(at(t0, 5 * i), loss(0.05)), AUTO_START_BPS);
        }
    }

    #[test]
    fn an_outage_holds_rather_than_walking_to_the_floor() {
        // The regression this module was rewritten for: a tunnel is total
        // loss, and fewer bits per packet do not get through a tunnel.
        let t0 = Instant::now();
        let mut a = AutoBitrate::default();
        for i in 0..12 {
            assert_eq!(a.update(at(t0, 5 * i), loss(1.0)), AUTO_START_BPS);
        }
    }

    #[test]
    fn a_queue_steps_down() {
        let mut a = AutoBitrate::default();
        assert_eq!(a.update(Instant::now(), queue(400.0)), 24_000);
    }

    #[test]
    fn no_evidence_changes_nothing() {
        let t0 = Instant::now();
        let mut a = AutoBitrate::default();
        a.update(t0, CLEAN);
        for i in 1..20 {
            a.update(at(t0, 5 * i), LinkReport::default());
        }
        // Silence is not thirty seconds of clean link.
        assert_eq!(a.bps(), AUTO_START_BPS);
    }

    #[test]
    fn a_blip_resets_the_climb() {
        let t0 = Instant::now();
        let mut a = AutoBitrate::default();
        a.update(t0, CLEAN);
        a.update(at(t0, 25), loss(0.05));
        // 30 s after the first clean report, but only 5 after the blip.
        assert_eq!(a.update(at(t0, 30), CLEAN), AUTO_START_BPS);
    }

    #[test]
    fn never_below_the_lowest_rung() {
        let t0 = Instant::now();
        let mut a = AutoBitrate::default();
        let mut last = 0;
        for i in 0..100 {
            last = a.update(at(t0, 20 * i), loss(0.3));
        }
        assert_eq!(last, AUTO_RUNGS[0]);
    }

    #[test]
    fn tunnelling_caps_the_rate_and_stops_the_climb() {
        let t0 = Instant::now();
        let mut a = AutoBitrate::default();
        for i in 0..=30 {
            a.update(at(t0, 5 * i), CLEAN);
        }
        assert_eq!(a.bps(), 48_000);
        let tcp = LinkReport {
            tunnelled: true,
            ..CLEAN
        };
        assert_eq!(a.update(at(t0, 200), tcp), TUNNELLED_CEILING_BPS);
        for i in 41..=80 {
            assert_eq!(a.update(at(t0, 5 * i), tcp), 24_000);
        }
    }

    #[test]
    fn loss_is_counted_since_the_last_poll_not_since_connecting() {
        let mut t = LinkTracker::default();
        assert_eq!(t.on_totals(10_000, 2_000), None, "a baseline, not a figure");
        // A clean stretch after a bad history reads as clean.
        assert_eq!(t.on_totals(10_250, 2_000), Some(0.0));
        // And a bad stretch after a clean one reads as bad at once.
        let l = t.on_totals(10_450, 2_050).unwrap();
        assert!((l - 0.2).abs() < 1e-6, "{l}");
    }

    #[test]
    fn a_few_packets_are_not_a_figure() {
        let mut t = LinkTracker::default();
        t.on_totals(100, 0);
        // One lost ping between phrases is not a 100% loss.
        assert_eq!(t.on_totals(100, 1), None);
        assert_eq!(t.on_totals(101, 1), None);
        // It is kept, and counted once enough has gone by.
        let l = t.on_totals(250, 1).unwrap();
        assert!(l > 0.0 && l < 0.01, "{l}");
    }

    #[test]
    fn a_reconnect_starts_the_counts_again() {
        let mut t = LinkTracker::default();
        t.on_totals(5_000, 10);
        assert_eq!(t.on_totals(20, 0), None);
        assert_eq!(t.on_totals(270, 0), Some(0.0));
    }

    #[test]
    fn queueing_is_measured_against_the_recent_lowest() {
        let t0 = Instant::now();
        let mut t = LinkTracker::default();
        assert_eq!(t.on_rtt(t0, 50.0), None);
        assert_eq!(t.on_rtt(at(t0, 5), 60.0), Some(0.0));
        // The same ping republished is not a new one.
        assert_eq!(t.on_rtt(at(t0, 6), 60.0), None);
        // A single spike is not a queue...
        assert_eq!(t.on_rtt(at(t0, 10), 400.0), Some(10.0));
        // ...two in a row are.
        assert_eq!(t.on_rtt(at(t0, 15), 380.0), Some(330.0));
        // No measurement yet is not a measurement.
        assert_eq!(t.on_rtt(at(t0, 20), 0.0), None);
    }

    #[test]
    fn the_baseline_forgets_old_lows() {
        let t0 = Instant::now();
        let mut t = LinkTracker::default();
        t.on_rtt(t0, 40.0);
        // Moved onto a slower network: 300 ms is its empty queue, not a
        // queue, once the old low has aged out.
        t.on_rtt(at(t0, 200), 300.0);
        assert_eq!(t.on_rtt(at(t0, 205), 301.0), Some(0.0));
    }

    #[test]
    fn fixed_rates_snap_to_the_slider() {
        assert_eq!(BitrateMode::fixed(0), BitrateMode::Fixed(FIXED_MIN_BPS));
        assert_eq!(
            BitrateMode::fixed(1_000_000),
            BitrateMode::Fixed(FIXED_MAX_BPS)
        );
        assert_eq!(BitrateMode::fixed(33_000), BitrateMode::Fixed(32_000));
        assert_eq!(BitrateMode::fixed(35_000), BitrateMode::Fixed(36_000));
    }
}
