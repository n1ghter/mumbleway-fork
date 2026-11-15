//! Choosing the voice bitrate: a rider's fixed number, or one that follows the
//! link.
//!
//! [`super::bandwidth`] fits whatever this module chooses inside the server's
//! allowance, so nothing here needs to know about caps: the server's limit is
//! always applied afterwards and always wins.
//!
//! # What "Auto" listens to
//!
//! The one signal that describes *our outgoing voice* is the server's own count
//! of the packets it received from us — `loss_up` in
//! [`crate::session::quality::Quality`], polled for our own session as for
//! everybody else in the channel. The receive side's loss is about other
//! riders' links and is not used. Two more facts sharpen it: whether voice is
//! tunnelled over TCP, where every extra byte queues behind the last, and the
//! round trip, which climbs long before a congested link starts losing.
//!
//! # What it does not promise
//!
//! **On a mobile link most loss is coverage, not congestion**, and the packet
//! rate is fixed by the frame length, so sending fewer bits per packet does not
//! make a dead spot lose fewer packets. In-band FEC is already at its maximum
//! (see `MIN_PROTECTION_PCT` in the engine). What this buys is the other half:
//! a clean link gets a better voice than the old fixed 24 kbit/s, and a
//! congested one — a crowded cell, a tunnel over TCP — is not asked to carry
//! more than it is carrying.
//!
//! # Why the asymmetry
//!
//! Down is quick and up is slow, as in every rate controller that has to live
//! on a shared link: a step down costs a little quality for a few seconds, a
//! step up into congestion costs words. The server's loss figure is a rolling
//! window of about a minute, so one bad second keeps reading as loss for a
//! while afterwards; [`DOWN_EVERY`] stops that one second from walking the rate
//! all the way to the floor.

use std::time::{Duration, Instant};

/// The rates "Auto" moves between, lowest first, in bits per second.
///
/// Above 48 kbit/s mono speech through a helmet speaker gains nothing a rider
/// can hear, and costs a third more data than 32.
pub const AUTO_RUNGS: [u32; 6] = [16_000, 20_000, 24_000, 32_000, 40_000, 48_000];

/// Where "Auto" starts: one rung above the old fixed default.
pub const AUTO_START_BPS: u32 = 32_000;

/// The ceiling while voice is tunnelled over TCP.
///
/// Every byte in a TCP stream waits for the one before it, so on a link bad
/// enough to have lost UDP, more bytes are more delay.
pub const TUNNELLED_CEILING_BPS: u32 = 24_000;

/// Range a rider may fix the bitrate to, `(min, max, step)`.
///
/// Wider than "Auto" on purpose: a rider on a server whose admin raised the
/// allowance may want more, and anything above the allowance is cut down to it
/// by [`super::bandwidth::fit`], so a high setting cannot make a rider
/// inaudible. The floor is [`super::bandwidth::MIN_AUDIO_BPS`].
pub const FIXED_MIN_BPS: u32 = 8_000;
pub const FIXED_MAX_BPS: u32 = 128_000;
pub const FIXED_STEP_BPS: u32 = 4_000;

/// Outgoing loss, 0 to 1, at which "Auto" steps down.
pub const LOSS_STEP_DOWN: f32 = 0.05;
/// Outgoing loss below which a link counts as clean enough to climb.
pub const LOSS_CLEAN: f32 = 0.01;
/// Round trip, in ms, above which a link counts as congested.
pub const PING_CONGESTED_MS: f32 = 400.0;
/// Round trip, in ms, below which a link may climb.
pub const PING_CLEAN_MS: f32 = 250.0;

/// How long a link must stay clean before "Auto" climbs one rung.
pub const CLEAN_BEFORE_UP: Duration = Duration::from_secs(30);
/// The shortest gap between two steps down. See the module note.
pub const DOWN_EVERY: Duration = Duration::from_secs(15);

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

/// One server's account of our connection.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct LinkReport {
    /// Share of our voice packets, 0 to 1, that never reached the server.
    pub loss_up: f32,
    /// Round trip in milliseconds; 0 when nothing has been measured.
    pub ping_ms: f32,
    /// Whether voice is going over the TCP tunnel.
    pub tunnelled: bool,
}

impl LinkReport {
    /// The worse of two reports, field by field.
    ///
    /// One encoder feeds every server, so a rider on two of them has to fit
    /// the worse link, as with [`super::bandwidth::tightest`].
    pub fn worst(self, other: Self) -> Self {
        Self {
            loss_up: self.loss_up.max(other.loss_up),
            ping_ms: self.ping_ms.max(other.ping_ms),
            tunnelled: self.tunnelled || other.tunnelled,
        }
    }

    fn congested(&self) -> bool {
        self.loss_up >= LOSS_STEP_DOWN || self.ping_ms >= PING_CONGESTED_MS
    }

    fn clean(&self) -> bool {
        self.loss_up < LOSS_CLEAN && self.ping_ms < PING_CLEAN_MS
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

    /// Takes a new report and returns the rate to use from now on.
    pub fn update(&mut self, now: Instant, link: LinkReport) -> u32 {
        if link.tunnelled {
            // A ceiling rather than a step: losing UDP is a fact about the
            // link, not a sample of it, and there is nothing to wait for.
            let ceiling = rung_of(TUNNELLED_CEILING_BPS);
            if self.rung > ceiling {
                self.rung = ceiling;
                self.last_down = Some(now);
            }
            self.clean_since = None;
            return self.bps();
        }

        if link.congested() {
            self.clean_since = None;
            let may_step = self
                .last_down
                .is_none_or(|t| now.saturating_duration_since(t) >= DOWN_EVERY);
            if may_step && self.rung > 0 {
                self.rung -= 1;
                self.last_down = Some(now);
            }
        } else if link.clean() {
            let since = *self.clean_since.get_or_insert(now);
            if now.saturating_duration_since(since) >= CLEAN_BEFORE_UP
                && self.rung + 1 < AUTO_RUNGS.len()
            {
                self.rung += 1;
                // Each rung has to earn the next one afresh.
                self.clean_since = Some(now);
            }
        } else {
            // Between the two: hold where we are and do not count it as clean.
            self.clean_since = None;
        }
        self.bps()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const CLEAN: LinkReport = LinkReport {
        loss_up: 0.0,
        ping_ms: 40.0,
        tunnelled: false,
    };
    const LOSSY: LinkReport = LinkReport {
        loss_up: 0.08,
        ping_ms: 40.0,
        tunnelled: false,
    };

    #[test]
    fn starts_one_rung_above_the_old_default() {
        assert_eq!(AutoBitrate::default().bps(), AUTO_START_BPS);
        assert!(AUTO_START_BPS > super::super::bandwidth::preferred_bps());
    }

    #[test]
    fn a_clean_link_climbs_slowly_to_the_top() {
        let t0 = Instant::now();
        let mut a = AutoBitrate::default();
        // Reports every 5 s, as the quality poller delivers them.
        let mut last = a.bps();
        for i in 0..=6 {
            last = a.update(t0 + Duration::from_secs(5 * i), CLEAN);
        }
        // 30 s of clean link buys exactly one rung.
        assert_eq!(last, 40_000);
        for i in 7..=40 {
            last = a.update(t0 + Duration::from_secs(5 * i), CLEAN);
        }
        assert_eq!(last, *AUTO_RUNGS.last().unwrap());
    }

    #[test]
    fn loss_steps_down_at_once_but_not_every_report() {
        let t0 = Instant::now();
        let mut a = AutoBitrate::default();
        assert_eq!(a.update(t0, LOSSY), 24_000);
        // The rolling window keeps reporting the same burst; that must not walk
        // the rate to the floor in a few seconds.
        assert_eq!(a.update(t0 + Duration::from_secs(5), LOSSY), 24_000);
        assert_eq!(a.update(t0 + Duration::from_secs(10), LOSSY), 24_000);
        assert_eq!(a.update(t0 + Duration::from_secs(15), LOSSY), 20_000);
    }

    #[test]
    fn never_below_the_lowest_rung() {
        let t0 = Instant::now();
        let mut a = AutoBitrate::default();
        let mut last = 0;
        for i in 0..100 {
            last = a.update(t0 + Duration::from_secs(20 * i), LOSSY);
        }
        assert_eq!(last, AUTO_RUNGS[0]);
    }

    #[test]
    fn a_long_round_trip_counts_as_congestion() {
        let mut a = AutoBitrate::default();
        let slow = LinkReport {
            ping_ms: 600.0,
            ..CLEAN
        };
        assert!(a.update(Instant::now(), slow) < AUTO_START_BPS);
    }

    #[test]
    fn some_loss_holds_without_climbing() {
        let t0 = Instant::now();
        let mut a = AutoBitrate::default();
        let marginal = LinkReport {
            loss_up: 0.02,
            ..CLEAN
        };
        for i in 0..20 {
            assert_eq!(
                a.update(t0 + Duration::from_secs(5 * i), marginal),
                AUTO_START_BPS
            );
        }
    }

    #[test]
    fn a_blip_resets_the_climb() {
        let t0 = Instant::now();
        let mut a = AutoBitrate::default();
        a.update(t0, CLEAN);
        a.update(
            t0 + Duration::from_secs(25),
            LinkReport {
                loss_up: 0.02,
                ..CLEAN
            },
        );
        // 30 s after the first clean report, but only 5 after the blip.
        assert_eq!(
            a.update(t0 + Duration::from_secs(30), CLEAN),
            AUTO_START_BPS
        );
    }

    #[test]
    fn tunnelling_caps_the_rate_immediately() {
        let t0 = Instant::now();
        let mut a = AutoBitrate::default();
        for i in 0..=30 {
            a.update(t0 + Duration::from_secs(5 * i), CLEAN);
        }
        assert_eq!(a.bps(), 48_000);
        let tcp = LinkReport {
            tunnelled: true,
            ..CLEAN
        };
        assert_eq!(
            a.update(t0 + Duration::from_secs(200), tcp),
            TUNNELLED_CEILING_BPS
        );
        // And it does not climb while tunnelled.
        for i in 41..=80 {
            assert_eq!(a.update(t0 + Duration::from_secs(5 * i), tcp), 24_000);
        }
    }

    #[test]
    fn the_worse_link_decides() {
        let tcp = LinkReport {
            tunnelled: true,
            ..CLEAN
        };
        let w = LOSSY.worst(tcp);
        assert!(w.tunnelled);
        assert_eq!(w.loss_up, LOSSY.loss_up);
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
