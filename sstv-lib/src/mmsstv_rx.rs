//! Port of the MMSSTV (JE3HHT Makoto Mori / JA7UDE, LGPL) SSTV receive chain,
//! used for SSTV RX in place of sstv.rs's SstvRx (sstv.rs keeps the TX side,
//! the mode list and the events). Measured against SstvRx on synthetic
//! pictures with noise, plain noise and off-air recordings: lower pixel
//! error at every SNR, VIS still decoded at 0 dB, fewer wrong-mode locks,
//! no false lock in an hour of receiver-filtered noise; SstvRx copes better
//! with a station tuned +100 Hz off (MMSSTV's fixed VIS / AFC windows).
//! MMSSTV source: LGPL (github.com/n5ac/mmsstv), compatible with this
//! program's GPL-3.0-or-later.
//!
//! Source files (github.com/n5ac/mmsstv): sstv.cpp / sstv.h (CSSTVDEM,
//! CSSTVSET, CSYNCINT, CHILL, CLVL), fir.cpp (MakeFilter, CFIR2, CIIR,
//! CIIRTANK, MakeHilbert), Main.cpp (SyncSSTV, ReSyncSSTV, DrawSSTVNormal,
//! AutoStopJob, DrawSSTV, RedrawSampFreq), ComLib.cpp (YCtoRGB).
//! Everything runs at SampFreq = SampBase = 12000 Hz, as MMSSTV does when
//! its sound card is set to 12 kHz; the 48 kHz input is low-passed and
//! decimated by 4 first. Defaults are those of a fresh MMSSTV install
//! (Main.cpp 509-535, Mmsstv.ini): wide BPF, sense level 1, Hilbert demod,
//! AFC on, AutoSync on, AutoStop OFF, AutoSlant OFF, SyncAccuracy 1,
//! UseRxBuff 1, LMS / notch off, FSK-ID decode off.
//! Differences from a fresh MMSSTV install: Auto slant and FSK-ID decode are
//! ON here (MMSSTV ships them off; measured: slant removes the clock-error
//! penalty, FSK-ID 5/5 down to 3 dB with no false ID in an hour of noise).
//! Choosing a mode (set_expected(Some(..))) starts receiving in it at once,
//! as MMSSTV's forced start (sstv.cpp 2240, m_SyncMode 256) does.
#![allow(dead_code, clippy::all)]

use crate::sstv::{SstvEvent, SstvMode};

const SF: f64 = 12000.0;
const PI: f64 = 3.1415926535897932384626433832795;

// ---- MMSSTV mode enum (sstv.h 450-495) ----
const SM_R36: usize = 0;
const SM_R72: usize = 1;
const SM_AVT: usize = 2;
const SM_SCT1: usize = 3;
const SM_SCT2: usize = 4;
const SM_SCTDX: usize = 5;
const SM_MRT1: usize = 6;
const SM_MRT2: usize = 7;
const SM_SC2_180: usize = 8;
const SM_SC2_120: usize = 9;
const SM_SC2_60: usize = 10;
const SM_PD50: usize = 11;
const SM_PD90: usize = 12;
const SM_PD120: usize = 13;
const SM_PD160: usize = 14;
const SM_PD180: usize = 15;
const SM_PD240: usize = 16;
const SM_PD290: usize = 17;
const SM_P3: usize = 18;
const SM_P5: usize = 19;
const SM_P7: usize = 20;
const SM_MR73: usize = 21;
const SM_MR90: usize = 22;
const SM_MR115: usize = 23;
const SM_MR140: usize = 24;
const SM_MR175: usize = 25;
const SM_MP73: usize = 26;
const SM_MP115: usize = 27;
const SM_MP140: usize = 28;
const SM_MP175: usize = 29;
const SM_ML180: usize = 30;
const SM_ML240: usize = 31;
const SM_ML280: usize = 32;
const SM_ML320: usize = 33;
const SM_R24: usize = 34;
const SM_RM8: usize = 35;
const SM_RM12: usize = 36;
const SM_MN73: usize = 37;
const SM_END: usize = 43;

/// CSSTVSET::GetTiming (sstv.cpp 1188-1278), ms per line, enum order.
const TIMING: [f64; SM_END] = [
    150.0, 300.0, 375.0, 428.22, 277.692, 1050.3, 446.446, 226.798, 711.0437, 475.52248, 240.3846,
    388.160, 703.040, 508.480, 804.416, 754.24, 1000.0, 937.28, 409.375, 614.0625, 818.75,
    286.3, 352.3, 450.3, 548.3, 684.3, 570.0, 902.0, 1090.0, 1370.0, 363.3, 483.3, 565.3, 645.3,
    200.0, 66.89709, 100.0, 570.0, 858.0, 1090.0, 428.5, 548.5, 704.5,
];
/// SSTVModeList (sstv.cpp 493-503).
const MODE_NAMES: [&str; SM_END] = [
    "Robot 36", "Robot 72", "AVT 90", "Scottie 1", "Scottie 2", "ScottieDX", "Martin 1", "Martin 2",
    "SC2 180", "SC2 120", "SC2 60", "PD50", "PD90", "PD120", "PD160", "PD180", "PD240", "PD290",
    "P3", "P5", "P7", "MR73", "MR90", "MR115", "MR140", "MR175", "MP73", "MP115", "MP140", "MP175",
    "ML180", "ML240", "ML280", "ML320", "Robot 24", "B/W 8", "B/W 12", "MP73-N", "MP110-N",
    "MP140-N", "MC110-N", "MC140-N", "MC180-N",
];

fn to_mode(i: usize) -> Option<SstvMode> {
    Some(match i {
        SM_R36 => SstvMode::Robot36,
        SM_R72 => SstvMode::Robot72,
        SM_SCT1 => SstvMode::Scottie1,
        SM_SCT2 => SstvMode::Scottie2,
        SM_SCTDX => SstvMode::ScottieDx,
        SM_MRT1 => SstvMode::Martin1,
        SM_MRT2 => SstvMode::Martin2,
        SM_SC2_180 => SstvMode::WraaseSc2_180,
        SM_SC2_120 => SstvMode::WraaseSc2_120,
        SM_PD50 => SstvMode::Pd50,
        SM_PD90 => SstvMode::Pd90,
        SM_PD120 => SstvMode::Pd120,
        SM_PD160 => SstvMode::Pd160,
        SM_PD180 => SstvMode::Pd180,
        SM_PD240 => SstvMode::Pd240,
        SM_PD290 => SstvMode::Pd290,
        _ => return None,
    })
}

fn is_sct(m: usize) -> bool {
    matches!(m, SM_SCT1 | SM_SCT2 | SM_SCTDX)
}
fn is_pd(m: usize) -> bool {
    (SM_PD50..=SM_PD290).contains(&m)
}

// ───────────────────────── fir.cpp ports ─────────────────────────

/// Kaiser I0 (fir.cpp 310-325).
fn i0(x: f64) -> f64 {
    let mut sum = 1.0;
    let mut xj = 1.0;
    let mut j = 1.0;
    loop {
        xj *= (0.5 * x) / j;
        sum += xj * xj;
        j += 1.0;
        if (0.00000001 * sum) - (xj * xj) > 0.0 {
            break;
        }
    }
    sum
}

#[derive(PartialEq, Clone, Copy)]
enum FType {
    Lpf,
    Bpf,
}

/// MakeFilter (fir.cpp 332-430): Kaiser-windowed (att >= 21) or plain
/// truncated-sinc FIR, `tap + 1` coefficients.
fn make_filter(tap: usize, typ: FType, fs: f64, fcl: f64, fch: f64, att: f64, gain: f64) -> Vec<f64> {
    let fc = if typ == FType::Lpf { fcl } else { (fch - fcl) / 2.0 };
    let alpha = if att >= 50.0 {
        0.1102 * (att - 8.7)
    } else if att >= 21.0 {
        0.5842 * (att - 21.0).powf(0.4) + 0.07886 * (att - 21.0)
    } else {
        0.0
    };
    let half = tap / 2;
    let mut hp = vec![0f64; half + 1];
    let sum = PI * 2.0 * fc / fs;
    for j in 0..=half {
        if j == 0 {
            hp[j] = fc * 2.0 / fs;
        } else if att >= 21.0 {
            let fm = (2 * j) as f64 / tap as f64;
            let win = i0(alpha * (1.0 - fm * fm).sqrt()) / i0(alpha);
            hp[j] = (1.0 / (PI * j as f64)) * (j as f64 * sum).sin() * win;
        } else {
            hp[j] = (1.0 / (PI * j as f64)) * (j as f64 * sum).sin();
        }
    }
    let mut s = hp[0];
    for j in 1..=half {
        s += 2.0 * hp[j];
    }
    if s > 0.0 {
        for v in hp.iter_mut() {
            *v /= s;
        }
    }
    if typ == FType::Bpf {
        let w0 = PI * (fcl + fch) / fs;
        for j in 0..=half {
            hp[j] *= 2.0 * (j as f64 * w0).cos();
        }
    }
    let mut out = Vec::with_capacity(tap + 1);
    for j in (0..=half).rev() {
        out.push(hp[j] * gain);
    }
    for j in 1..=half {
        out.push(hp[j] * gain);
    }
    out
}

/// MakeHilbert (fir.cpp 432-478).
fn make_hilbert(n_: usize, fs: f64, fc1: f64, fc2: f64) -> Vec<f64> {
    let l = (n_ / 2) as i64;
    let t = 1.0 / fs;
    let w1 = 2.0 * PI * fc1;
    let w2 = 2.0 * PI * fc2;
    let mut h = vec![0f64; n_ + 1];
    for n in 0..=n_ as i64 {
        let (x1, x2) = if n == l {
            (0.0, 0.0)
        } else {
            let a = (n - l) as f64 * w1 * t;
            let b = (n - l) as f64 * w2 * t;
            (a.cos() / a, b.cos() / b)
        };
        let w = 0.54 - 0.46 * (2.0 * PI * n as f64 / n_ as f64).cos();
        h[n as usize] = -(2.0 * fc2 * t * x2 - 2.0 * fc1 * t * x1) * w;
    }
    h
}

/// CFIR2 (fir.cpp 1063-1171): y = sum h[k] x[t-k], k = 0..=tap.
struct Fir2 {
    z: Vec<f64>,
    w: usize,
    tap: usize,
}
impl Fir2 {
    fn new(tap: usize) -> Self {
        Fir2 { z: vec![0.0; (tap + 1) * 2], w: 0, tap }
    }
    fn run(&mut self, d: f64, h: &[f64]) -> f64 {
        let t = self.tap;
        self.z[self.w + t + 1] = d;
        self.z[self.w] = d;
        let mut acc = 0.0;
        let mut p = self.w + t + 1;
        for k in 0..=t {
            acc += self.z[p] * h[k];
            p -= 1;
        }
        self.w += 1;
        if self.w > t {
            self.w = 0;
        }
        acc
    }
}

/// CIIRTANK (fir.cpp 40-74): 2-pole resonator.
#[derive(Clone)]
struct Tank {
    z1: f64,
    z2: f64,
    a0: f64,
    b1: f64,
    b2: f64,
}
impl Tank {
    fn new(f: f64, bw: f64) -> Self {
        let mut t = Tank { z1: 0.0, z2: 0.0, a0: 0.0, b1: 0.0, b2: 0.0 };
        t.set_freq(f, SF, bw);
        t
    }
    fn set_freq(&mut self, f: f64, smp: f64, bw: f64) {
        self.b1 = 2.0 * (-PI * bw / smp).exp() * (2.0 * PI * f / smp).cos();
        self.b2 = -(-2.0 * PI * bw / smp).exp();
        self.a0 = if bw != 0.0 {
            (2.0 * PI * f / smp).sin() / ((smp / 6.0) / bw)
        } else {
            (2.0 * PI * f / smp).sin()
        };
    }
    fn run(&mut self, d: f64) -> f64 {
        let mut d = d * self.a0;
        d += self.z1 * self.b1;
        d += self.z2 * self.b2;
        self.z2 = self.z1;
        if d.abs() < 1e-37 {
            d = 0.0;
        }
        self.z1 = d;
        d
    }
}

/// CIIR + MakeIIR (fir.cpp 953-1060), Butterworth only (bc = 0).
struct Iir {
    a: [f64; 48],
    b: [f64; 32],
    z: [f64; 32],
    order: usize,
}
impl Iir {
    fn new(fc: f64, fs: f64, order: usize) -> Self {
        let mut s = Iir { a: [0.0; 48], b: [0.0; 32], z: [0.0; 32], order };
        let wa = (PI * fc / fs).tan();
        let w0 = 1.0;
        let mut n = (order & 1) as f64 + 1.0;
        for j in 1..=order / 2 {
            let pa = (j - 1) * 3;
            let pb = (j - 1) * 2;
            let zt = (n * PI / (2.0 * order as f64)).cos();
            let ww = wa * w0;
            s.a[pa] = 1.0 + ww * 2.0 * zt + ww * ww;
            s.a[pa + 1] = -2.0 * (ww * ww - 1.0) / s.a[pa];
            s.a[pa + 2] = -(1.0 - ww * 2.0 * zt + ww * ww) / s.a[pa];
            s.b[pb] = ww * ww / s.a[pa];
            s.b[pb + 1] = 2.0 * s.b[pb];
            n += 2.0;
        }
        if order & 1 == 1 {
            let j = order / 2;
            let pa = j * 3;
            let pb = j * 2;
            s.a[pa] = 1.0 + wa * w0;
            s.a[pa + 1] = -(wa * w0 - 1.0) / s.a[pa];
            s.b[pb] = wa * w0 / s.a[pa];
            s.b[pb + 1] = s.b[pb];
        }
        s
    }
    fn run(&mut self, mut d: f64) -> f64 {
        let (mut pa, mut pb, mut pz) = (0usize, 0usize, 0usize);
        for _ in 0..self.order / 2 {
            d += self.z[pz] * self.a[pa + 1] + self.z[pz + 1] * self.a[pa + 2];
            let o = d * self.b[pb] + self.z[pz] * self.b[pb + 1] + self.z[pz + 1] * self.b[pb];
            self.z[pz + 1] = self.z[pz];
            if d.abs() < 1e-37 {
                d = 0.0;
            }
            self.z[pz] = d;
            d = o;
            pa += 3;
            pb += 2;
            pz += 2;
        }
        if self.order & 1 == 1 {
            d += self.z[pz] * self.a[pa + 1];
            let o = d * self.b[pb] + self.z[pz] * self.b[pb];
            if d.abs() < 1e-37 {
                d = 0.0;
            }
            self.z[pz] = d;
            d = o;
        }
        d
    }
}

// ───────────────────────── sstv.h helpers ─────────────────────────

/// CSmooz (sstv.h 80-145): moving average.
struct Smooz {
    bp: Vec<f64>,
    wp: usize,
    max: usize,
    cnt: usize,
}
impl Smooz {
    fn new(n: usize) -> Self {
        let n = n.max(1);
        Smooz { bp: vec![0.0; n], wp: 0, max: n, cnt: 0 }
    }
    fn reset(&mut self) {
        self.cnt = 0;
        self.wp = 0;
    }
    fn set_data(&mut self, d: f64) -> f64 {
        for v in self.bp.iter_mut() {
            *v = d;
        }
        self.wp = 0;
        self.cnt = self.max;
        d
    }
    fn avg(&mut self, d: f64) -> f64 {
        self.bp[self.wp] = d;
        self.wp += 1;
        if self.wp >= self.max {
            self.wp = 0;
        }
        if self.cnt < self.max {
            self.cnt += 1;
        }
        let s: f64 = self.bp[..self.cnt].iter().sum();
        if self.cnt > 0 {
            s / self.cnt as f64
        } else {
            0.0
        }
    }
}

/// CLVL (sstv.h 223-298) with m_agcfast = 1 (sstv.cpp 1470).
struct Lvl {
    cur: f64,
    peak_max: f64,
    peak_agc: f64,
    peak: f64,
    cur_max: f64,
    max: f64,
    agc: f64,
    cnt_peak: i32,
    cnt: i32,
    cnt_max: i32,
}
impl Lvl {
    fn new() -> Self {
        Lvl {
            cur: 0.0,
            peak_max: 0.0,
            peak_agc: 0.0,
            peak: 0.0,
            cur_max: 0.0,
            max: 0.0,
            agc: 1.0,
            cnt_peak: 0,
            cnt: 0,
            cnt_max: (SF * 100.0 / 1000.0) as i32,
        }
    }
    fn do_(&mut self, d: f64) {
        self.cur = d;
        let a = d.abs();
        if self.max < a {
            self.max = a;
        }
        self.cnt += 1;
    }
    /// Main.cpp 5734 calls Fix() from the GUI timer; it acts only once
    /// 100 ms of samples have gone by, so it is called per sample here.
    fn fix(&mut self) {
        if self.cnt < self.cnt_max {
            return;
        }
        self.cnt = 0;
        self.cnt_peak += 1;
        if self.peak < self.max {
            self.peak = self.max;
        }
        if self.cnt_peak >= 5 {
            self.cnt_peak = 0;
            self.peak_max = self.max;
            self.peak_agc = (self.peak_agc + self.max) * 0.5;
            self.peak = 0.0;
        } else if self.peak_max < self.max {
            self.peak_max = self.max;
        }
        self.cur_max = self.max;
        self.agc = if self.cur_max > 32.0 { 16384.0 / self.cur_max } else { 16384.0 / 32.0 };
        self.max = 0.0;
    }
}

/// CHILL (sstv.cpp 3005-3087): Hilbert FM discriminator. At SampBase 12000
/// m_tap = 12, m_df = 0 (one-sample phase difference).
struct Hill {
    h: Vec<f64>,
    z: Vec<f64>,
    a0: f64,
    off: f64,
    out: f64,
    tap: usize,
    htap: usize,
    iir: Iir,
}
impl Hill {
    fn new() -> Self {
        let tap = 12;
        Hill {
            h: make_hilbert(tap, SF, 100.0, SF / 2.0 - 100.0),
            z: vec![0.0; tap + 1],
            a0: 0.0,
            off: (2.0 * PI * 1900.0) / SF,
            out: 32768.0 * SF / (2.0 * PI * 800.0),
            tap,
            htap: tap / 2,
            iir: Iir::new(1800.0, SF, 3),
        }
    }
    fn run(&mut self, d: f64) -> f64 {
        // DoFIR (fir.cpp 29-38): z[0] oldest, z[tap] newest, h not reversed.
        self.z.copy_within(1.., 0);
        self.z[self.tap] = d;
        let mut y = 0.0;
        for i in 0..=self.tap {
            y += self.z[i] * self.h[i];
        }
        let mut a = self.z[self.htap];
        if a != 0.0 {
            a = y.atan2(a);
        }
        let mut dd = a - self.a0;
        self.a0 = a;
        if dd >= PI {
            dd -= PI * 2.0;
        } else if dd <= -PI {
            dd += PI * 2.0;
        }
        dd += self.off;
        self.iir.run(dd * self.out)
    }
}

/// CSYNCINT (sstv.h 560-590, sstv.cpp 1290-1411), wide (m_fNarrow = 0).
struct SyncInt {
    list: [u32; 8],
    cnt: u32,
    acnt: u32,
    int_pos: i64,
    int_max: i32,
}
struct MsTable {
    ms: [u32; SM_END],
    msll: u32,
    msl: u32,
    msh: u32,
}
impl MsTable {
    /// CSSTVSET::InitIntervalPara (sstv.cpp 574-586).
    fn new() -> Self {
        let mut ms = [0u32; SM_END];
        for i in 0..SM_END {
            ms[i] = (TIMING[i] * SF / 1000.0) as u32;
        }
        ms[SM_AVT] = 0;
        MsTable { ms, msll: (50.0 * SF / 1000.0) as u32, msl: (63.0 * SF / 1000.0) as u32, msh: (1390.0 * 3.0 * SF / 1000.0) as u32 }
    }
}
impl SyncInt {
    fn new() -> Self {
        SyncInt { list: [0; 8], cnt: 0, acnt: 0, int_pos: 0, int_max: 0 }
    }
    fn reset(&mut self) {
        *self = SyncInt::new();
    }
    fn check_sub(&self, am: usize, t: &MsTable) -> bool {
        let e: i32 = match am {
            SM_SC2_60 | SM_SC2_120 => return false,
            SM_R24 | SM_R36 | SM_MRT2 | SM_PD50 | SM_PD240 => 8 - 4,
            SM_RM8 | SM_RM12 => 0,
            37..=42 => return false, // narrow modes, wide CSYNCINT
            _ => 8 - 3,
        };
        let deff = (3.0 * SF / 1000.0) as u32;
        let cml = t.ms[am].wrapping_sub(deff);
        let cmh = t.ms[am] + deff;
        let mut i: i32 = 8 - 2;
        while i >= e {
            let w = self.list[i as usize];
            let mut f = false;
            if w > t.msl {
                for k in 1..=3u32 {
                    let ww = w / k;
                    if ww > cml && ww < cmh {
                        f = true;
                    }
                }
            }
            if !f {
                return false;
            }
            i -= 1;
        }
        true
    }
    fn check(&self, t: &MsTable) -> usize {
        let deff = (3.0 * SF / 1000.0) as u32;
        let w = self.list[7];
        for k in 1..=3u32 {
            let ww = w / k;
            if ww > t.msl && ww < t.msh {
                for i in 0..SM_END {
                    if t.ms[i] != 0 && ww > t.ms[i] - deff && ww < t.ms[i] + deff && self.check_sub(i, t) {
                        return i + 1;
                    }
                }
            } else {
                break;
            }
        }
        0
    }
    fn inc(&mut self) {
        self.cnt = self.cnt.wrapping_add(1);
    }
    fn trig(&mut self, d: i32) {
        self.int_max = d;
        self.int_pos = self.cnt as i64;
    }
    fn max(&mut self, d: i32) {
        if self.int_max < d {
            self.int_max = d;
            self.int_pos = self.cnt as i64;
        }
    }
    fn start(&mut self, t: &MsTable) -> usize {
        let mut ss = 0;
        if self.int_max != 0 {
            let dif = (self.int_pos as u32).wrapping_sub(self.acnt);
            if dif > t.msll {
                self.acnt = dif;
                self.list.copy_within(1.., 0);
                self.list[7] = self.acnt;
                if self.acnt > t.msl {
                    ss = self.check(t);
                }
                self.acnt = self.int_pos as u32;
            }
            self.int_max = 0;
        }
        ss
    }
}

/// CSSTVSET for one mode (sstv.cpp 588-596, 655-1186); values in samples.
#[derive(Clone)]
struct ModeSet {
    mode: usize,
    tw: f64,
    ks: f64,
    ks2: f64,
    of: f64,
    ofp: f64,
    cg: f64,
    cb: f64,
    sg: f64,
    sb: f64,
    wd: usize,
    l: i32,
    lm: i32,
    kss: f64,
    ks2s: f64,
    ksb: usize,
    afcw: i32,
    afcb: i32,
    afce: i32,
    bw: usize,
    bh: usize,
}
impl ModeSet {
    fn new(mode: usize) -> Self {
        Self::with_rate(mode, SF)
    }
    /// CSSTVSET::SetSampFreq after a slant correction changed m_SampFreq
    /// (sstv.cpp 655-1186): m_WD and m_LM are only set by SetMode (594-595)
    /// and stay as they were.
    fn set_samp(&mut self, samp: f64) {
        let (wd, lm) = (self.wd, self.lm);
        *self = Self::with_rate(self.mode, samp);
        self.wd = wd;
        self.lm = lm;
    }
    fn with_rate(mode: usize, samp: f64) -> Self {
        let s = |ms: f64| ms * samp / 1000.0;
        let (mut ks, mut ks2, of, ofp, sg, cg, sb, cb, l);
        ks2 = 0.0;
        let gen = |ks: f64, sgv: f64| (sgv, ks + sgv, sgv + sgv, ks + sgv + sgv);
        match mode {
            SM_R36 => {
                ks = s(88.0);
                ks2 = s(44.0);
                of = s(12.0);
                ofp = s(10.7);
                sg = s(88.0 + 1.25);
                cg = (88.0 + 3.5) * SF / 1000.0; // uses SampFreq, not m_SampFreq (sstv.cpp 665)
                sb = s(94.0);
                cb = sb + ks2;
                l = 240;
            }
            SM_R72 => {
                ks = s(138.0);
                ks2 = s(69.0);
                of = s(12.0);
                ofp = s(10.7);
                sg = s(144.0);
                cg = sg + ks2;
                sb = s(219.0);
                cb = sb + ks2;
                l = 240;
            }
            _ => {
                let (k, o, op, sgm, ll) = match mode {
                    SM_SCT2 => (88.064, 10.5, 10.8, Some(89.564), 256),
                    SM_SCTDX => (345.6, 10.5, 10.2, Some(347.1), 256),
                    SM_MRT1 => (146.432, 5.434, 7.2, Some(147.004), 256),
                    SM_MRT2 => (73.216, 5.434, 7.4, Some(73.788), 256),
                    SM_SC2_180 => (235.0, 6.0437, 7.8, None, 256),
                    SM_SC2_120 => (156.5, 6.02248, 7.5, None, 256),
                    SM_PD50 => (91.520, 22.080, 19.300, None, 128),
                    SM_PD90 => (170.240, 22.080, 18.900, None, 128),
                    SM_PD120 => (121.600, 22.080, 19.400, None, 248),
                    SM_PD160 => (195.584, 22.080, 18.900, None, 200),
                    SM_PD180 => (183.04, 22.080, 18.900, None, 248),
                    SM_PD240 => (244.48, 22.080, 18.900, None, 248),
                    SM_PD290 => (228.80, 22.080, 18.900, None, 616 / 2),
                    _ => (138.24, 10.5, 10.7, Some(139.74), 256), // SCT1 (default)
                };
                ks = s(k);
                of = s(o);
                ofp = s(op);
                let sgv = match sgm {
                    Some(v) => s(v),
                    None => ks,
                };
                let g = gen(ks, sgv);
                sg = g.0;
                cg = g.1;
                sb = g.2;
                cb = g.3;
                l = ll;
            }
        }
        let tw = TIMING[mode] * samp / 1000.0;
        let (kss, ks2s, ksb) = match mode {
            SM_PD120 | SM_PD160 | SM_PD180 | SM_PD240 | SM_PD290 => {
                let kss = ks - ks / 480.0;
                (kss, ks2 - ks2 / 480.0, (kss / 1280.0) as usize)
            }
            SM_SCTDX => {
                let kss = ks - ks / 1280.0;
                (kss, ks2 - ks2 / 1280.0, (kss / 1280.0) as usize)
            }
            SM_SC2_180 => (ks, ks2, (ks / 1280.0) as usize),
            _ => {
                let kss = ks - ks / 240.0;
                (kss, ks2 - ks2 / 240.0, (kss / 640.0) as usize)
            }
        };
        let (afcw, afcb) = match mode {
            SM_MRT1 | SM_MRT2 | SM_SC2_120 | SM_SC2_180 => ((2.0 * SF / 1000.0) as i32, (1.0 * SF / 1000.0) as i32),
            _ => ((3.0 * SF / 1000.0) as i32, (1.5 * SF / 1000.0) as i32),
        };
        let (bw, bh) = match mode {
            SM_PD120 | SM_PD180 | SM_PD240 => (640, 496),
            SM_PD160 => (512, 400),
            SM_PD290 => (800, 616),
            _ => (320, 256),
        };
        let _ = &mut ks;
        ModeSet {
            mode,
            tw,
            ks,
            ks2,
            of,
            ofp,
            cg,
            cb,
            sg,
            sb,
            wd: tw as usize,
            l,
            lm: (tw * l as f64) as i32 + 1,
            kss,
            ks2s,
            ksb: ksb.max(1),
            afcw,
            afcb,
            afce: afcb + afcw,
            bw,
            bh,
        }
    }
}

const NO_POS: i32 = 0x7fffffff;

/// Anti-alias decimator to 12 kHz (local; MMSSTV gets 12 kHz from the card).
struct Decim {
    h: Vec<f64>,
    hist: Vec<f64>,
    pos: usize,
    factor: usize,
    phase: usize,
}
impl Decim {
    fn new(rate: f64) -> Self {
        let factor = (rate / SF).round().max(1.0) as usize;
        let n = 96 * factor / 4 + 1;
        let fc = 4800.0 / rate;
        let mid = (n / 2) as f64;
        let mut h: Vec<f64> = (0..n)
            .map(|i| {
                let t = i as f64 - mid;
                let s = if t == 0.0 { 2.0 * fc } else { (2.0 * PI * fc * t).sin() / (PI * t) };
                let w = 0.42 - 0.5 * (2.0 * PI * i as f64 / (n - 1) as f64).cos()
                    + 0.08 * (4.0 * PI * i as f64 / (n - 1) as f64).cos();
                s * w
            })
            .collect();
        let g: f64 = h.iter().sum();
        for v in h.iter_mut() {
            *v /= g;
        }
        Decim { hist: vec![0.0; n], h, pos: 0, factor, phase: 0 }
    }
    fn push(&mut self, x: f64) -> Option<f64> {
        if self.factor == 1 {
            return Some(x);
        }
        let n = self.h.len();
        self.hist[self.pos] = x;
        self.pos = (self.pos + 1) % n;
        self.phase += 1;
        if self.phase < self.factor {
            return None;
        }
        self.phase = 0;
        let mut acc = 0.0;
        let mut p = self.pos;
        for k in (0..n).rev() {
            p = if p == 0 { n - 1 } else { p - 1 };
            acc += self.h[n - 1 - k] * self.hist[p];
        }
        Some(acc)
    }
}

/// MMSSTV receiver (CSSTVDEM + the receive half of TMmsstv).
pub struct MmRx {
    rate: f64,
    dec: Decim,
    events: Vec<SstvEvent>,
    in_level: f32,
    autostop: bool,
    expected: Option<SstvMode>,

    // ---- CSSTVDEM ----
    ad: f64,
    hbpf: Vec<f64>,
    hbpfs: Vec<f64>,
    bpf: Fir2,
    lvl: Lvl,
    iir11: Tank,
    iir12: Tank,
    iir13: Tank,
    iir19: Tank,
    iirfsk: Tank,
    lpf11: Iir,
    lpf12: Iir,
    lpf13: Iir,
    lpf19: Iir,
    lpffsk: Iir,
    hill: Hill,
    sint1: SyncInt,
    sint2: SyncInt,
    ms: MsTable,
    slvl: f64,
    slvl2: f64,
    slvl3: f64,

    sync: bool,
    sync_mode: i32,
    sync_time: i32,
    vis_data: i32,
    vis_cnt: i32,
    next_mode: usize,
    skip: i32,
    req_save: bool,

    // AFC
    avg: Smooz,
    afcavg: Smooz,
    afc_count: i32,
    afc_data: f64,
    afc_lock: f64,
    afc_diff: f64,
    afc_fq: i32,
    afc_gard: i32,
    afc_dis: i32,
    afc_int: i32,
    afc_low: f64,
    afc_high: f64,
    afc_syncval: f64,
    afc_bwh: f64,

    // demod output store (m_Buf / m_B12 pages, kept contiguous like m_StgBuf)
    st: ModeSet,
    buf: Vec<i16>,
    b12: Vec<i16>,
    w_cnt: usize,
    w_line: usize,
    r_page: usize,
    w_bgn: i32,
    stg_lines: usize,
    stg_alloc: usize,

    // ---- TMmsstv receive side ----
    r_base: i64,
    ofs: i64,
    sync_pos: i32,
    sync_rpos: i32,
    sync_max: i32,
    sync_min: i32,
    ax: i32,
    ay: i32,
    dsel: usize,
    d36: [[i32; 800]; 2],
    y36: [i32; 800],
    sync_acc_n: i32,
    // auto stop / auto sync
    as_apos: [i32; 16],
    as_cnt: i32,
    as_acnt: i32,
    as_pos: i32,
    as_dis: bool,
    as_cur_y: i32,
    auto_sync_pos: i32,
    auto_sync_dis: i32,
    auto_sync_count: i32,
    auto_sync_diff: i32,
    mult: i32,

    // picture
    img: Vec<u8>,
    dirty: Vec<bool>,
    emit_y: i32,
    // auto slant (KRSA, Main.cpp 3560-3623)
    slant: bool,
    samp: f64,
    req_samp_chg: bool,
    as_bgn_pos: i32,
    as_bit_mask: u32,
    as_avg: Smooz,
    as_lmt: [f64; 7],
    as_posl: [i32; 4],
    // FSK ID (CSSTVDEM::DecodeFSK, sstv.cpp 2378-2606)
    fskid: bool,
    fsk_mode: i32,
    fsk_time: i32,
    fsk_cnt: usize,
    fsk_bcnt: i32,
    fsk_nexti: i32,
    fsk_nextd: f64,
    fsk_s: u8,
    fsk_c: u8,
    fsk_data: [u8; 20],
    fsk_nr: i32,
    fsk_rec: Option<String>,
    nsamp: u64,
    dbg: bool,
}

impl MmRx {
    pub fn new(rate: f64) -> Self {
        let tap = (24.0 * SF / 11025.0) as usize; // CalcBPF wide (sstv.cpp 1529)
        let hbpf = make_filter(tap, FType::Bpf, SF, 1100.0, 2600.0, 20.0, 1.0);
        let hbpfs = make_filter(tap, FType::Bpf, SF, 400.0, 2500.0, 20.0, 1.0);
        let lpf = || Iir::new(50.0, SF, 2);
        let st = ModeSet::new(SM_SCT1);
        let mut s = MmRx {
            rate,
            dec: Decim::new(rate),
            events: Vec::new(),
            in_level: 0.0,
            autostop: false, // MMSSTV default: Auto stop off (it dropped a real weak picture off the air)
            expected: None,
            ad: 0.0,
            hbpf,
            hbpfs,
            bpf: Fir2::new(tap),
            lvl: Lvl::new(),
            iir11: Tank::new(1080.0, 80.0),
            iir12: Tank::new(1200.0, 100.0),
            iir13: Tank::new(1320.0, 80.0),
            iir19: Tank::new(1900.0, 100.0),
            iirfsk: Tank::new(2100.0, 100.0),
            lpf11: lpf(),
            lpf12: lpf(),
            lpf13: lpf(),
            lpf19: lpf(),
            lpffsk: lpf(),
            hill: Hill::new(),
            sint1: SyncInt::new(),
            sint2: SyncInt::new(),
            ms: MsTable::new(),
            slvl: 3500.0,
            slvl2: 3500.0 * 0.5,
            slvl3: 5700.0,
            sync: false,
            sync_mode: 0,
            sync_time: 0,
            vis_data: 0,
            vis_cnt: 0,
            next_mode: 0,
            skip: 0,
            req_save: false,
            avg: Smooz::new((2.5 * SF / 1000.0) as usize),
            afcavg: Smooz::new(15),
            afc_count: 0,
            afc_data: 0.0,
            afc_lock: 0.0,
            afc_diff: 0.0,
            afc_fq: 0,
            afc_gard: 0,
            afc_dis: 0,
            afc_int: (100.0 * SF / 1000.0) as i32,
            afc_low: 0.0,
            afc_high: 0.0,
            afc_syncval: 0.0,
            afc_bwh: 0.0,
            st,
            buf: Vec::new(),
            b12: Vec::new(),
            w_cnt: 0,
            w_line: 0,
            r_page: 0,
            w_bgn: 0,
            stg_lines: 0,
            stg_alloc: (257.0 * 1100.0 * SF / 1000.0) as usize,
            r_base: 0,
            ofs: 0,
            sync_pos: -1,
            sync_rpos: -1,
            sync_max: 0,
            sync_min: 0,
            ax: -1,
            ay: -5,
            dsel: 0,
            d36: [[0; 800]; 2],
            y36: [0; 800],
            sync_acc_n: 0,
            as_apos: [0; 16],
            as_cnt: 0,
            as_acnt: 0,
            as_pos: 0,
            as_dis: false,
            as_cur_y: 0,
            auto_sync_pos: NO_POS,
            auto_sync_dis: 0,
            auto_sync_count: 0,
            auto_sync_diff: 0,
            mult: 1,
            img: Vec::new(),
            dirty: Vec::new(),
            emit_y: -10,
            slant: true, // MMSSTV ships Auto slant off; on here
            samp: SF,
            req_samp_chg: false,
            as_bgn_pos: NO_POS,
            as_bit_mask: 0,
            as_avg: Smooz::new(16),
            // m_ASLmt (Main.cpp 1266-1272), Hz of sampling-rate error
            as_lmt: [25.0, 10.0, 2.0, 0.5, 0.2, 0.2, 0.08].map(|v: f64| v * SF / 11025.0),
            as_posl: [64, 128, 160, 220],
            fskid: true, // MMSSTV ships FSK-ID decode off; on here
            fsk_mode: 0,
            fsk_time: 0,
            fsk_cnt: 0,
            fsk_bcnt: 0,
            fsk_nexti: 0,
            fsk_nextd: 0.0,
            fsk_s: 0,
            fsk_c: 0,
            fsk_data: [0; 20],
            fsk_nr: 0,
            fsk_rec: None,
            nsamp: 0,
            dbg: false,
        };
        s.init_afc();
        s
    }

    pub fn set_expected(&mut self, mode: Option<SstvMode>) {
        // MMSSTV has no "free-run only this mode": picking a mode is a forced
        // start in it (sstv.cpp 2240); None (Auto) just keeps hunting. Only on
        // a change -- the UI may pass the same choice again every frame.
        if mode == self.expected {
            return;
        }
        self.expected = mode;
        if let Some(m) = mode {
            if let Some(i) = (0..SM_END).find(|&i| to_mode(i) == Some(m)) {
                self.start(i);
            }
        }
    }
    pub fn restart(&mut self) {
        self.stop();
        self.sync_mode = 0;
    }
    pub fn level(&self) -> f32 {
        self.in_level
    }
    pub fn sync_quality(&self) -> f32 {
        ((self.sync_max - self.sync_min) as f32 / 16384.0).clamp(0.0, 1.0)
    }
    pub fn receiving(&self) -> bool {
        self.sync
    }
    pub fn progress(&self) -> f32 {
        if !self.sync {
            return 0.0;
        }
        (self.ay as f32 / self.st.l as f32).clamp(0.0, 1.0)
    }

    pub fn process(&mut self, audio: &[f32], out: &mut Vec<SstvEvent>) {
        for &a in audio {
            self.in_level += (a.abs() - self.in_level) * 0.0005;
            if let Some(x) = self.dec.push(a as f64) {
                // Sound.cpp 341-366: 16-bit PCM samples go into CSSTVDEM::Do.
                self.dem_do(x * 32768.0);
            }
        }
        out.append(&mut self.events);
    }

    // ───────────── CSSTVDEM ─────────────

    fn init_tone(&mut self, dfq: i32) {
        if self.afc_fq != dfq {
            let f = dfq as f64;
            self.iir11.set_freq(1080.0 + f, SF, 80.0);
            self.iir12.set_freq(1200.0 + f, SF, 100.0);
            self.iir13.set_freq(1320.0 + f, SF, 80.0);
            self.iir19.set_freq(1900.0 + f, SF, 100.0);
            self.iirfsk.set_freq(2100.0 + f, SF, 100.0);
            self.afc_fq = dfq;
        }
    }

    fn init_afc(&mut self) {
        self.afcavg.reset();
        self.afc_data = ((1900.0 - 1200.0) * 16384.0) / 400.0;
        self.afc_lock = self.afc_data;
        self.afc_diff = 0.0;
        self.afc_gard = 10;
        self.afc_count = 0;
        self.afc_dis = 0;
        self.init_tone(0);
        self.afc_low = (1900.0 - 1000.0) * 16384.0 / 400.0;
        self.afc_high = (1900.0 - 1325.0) * 16384.0 / 400.0;
        self.afc_syncval = (1900.0 - 1200.0) * 16384.0 / 400.0;
        self.afc_bwh = 400.0 / 16384.0;
    }

    fn sync_freq(&mut self, d: f64) {
        let d = d - 128.0;
        if d <= self.afc_low && d >= self.afc_high {
            if self.afc_dis == 0 && self.afc_count >= self.st.afcb && self.afc_count <= self.st.afce {
                self.afc_data = self.avg.avg(d);
                if self.afc_count == self.st.afce {
                    if self.afc_gard != 0 {
                        self.afc_lock = self.afcavg.set_data(self.afc_data);
                        self.afc_gard = 0;
                    } else {
                        self.afc_lock = self.afcavg.avg(self.afc_data);
                    }
                    self.afc_diff = self.afc_syncval - self.afc_lock;
                    if self.dbg && (self.as_cur_y < 3 || self.as_cur_y % 32 == 1) { eprintln!("{:.3}s AFC data {:.0} lock {:.0} diff {:.0} = {:.1} Hz y {}", self.nsamp as f64 / SF, self.afc_data, self.afc_lock, self.afc_diff, self.afc_diff * self.afc_bwh, self.as_cur_y); }
                    self.init_tone((self.afc_diff * self.afc_bwh) as i32);
                    self.afc_dis = self.afc_int;
                }
            }
            self.afc_count += 1;
        } else {
            if self.afc_count >= self.st.afcb && self.afc_gard != 0 {
                self.afc_gard -= 1;
                if self.afc_gard == 0 {
                    let l = self.afc_lock;
                    self.afcavg.set_data(l);
                }
            }
            self.afc_count = 0;
            if self.afc_dis != 0 {
                self.afc_dis -= 1;
            }
        }
    }

    /// SSTVSET.SetMode(mode) + Start() (sstv.cpp 1717-1747), with the
    /// bookkeeping DrawSSTV does for the picture it replaces (Main.cpp 4512).
    fn start(&mut self, mode: usize) {
        if self.dbg { eprintln!("{:.3}s START {} (sync_mode {}, vis {:#x})", self.nsamp as f64 / SF, MODE_NAMES[mode], self.sync_mode, self.vis_data); }
        let m = match to_mode(mode) {
            Some(m) => m,
            None => {
                self.events.push(SstvEvent::UnsupportedMode { code: 0, name: Some(MODE_NAMES[mode]) });
                return;
            }
        };
        if self.sync {
            self.flush_rows(true);
            self.events.push(if self.req_save { SstvEvent::ImageComplete } else { SstvEvent::LockDropped });
        }
        self.req_save = false;
        self.samp = SF;
        self.req_samp_chg = false;
        self.st = ModeSet::new(mode);
        self.init_afc();
        self.sync = false;
        self.skip = 0;
        self.w_line = 0;
        self.w_cnt = 0;
        self.r_page = 0;
        self.r_base = 0;
        self.stg_lines = 0;
        self.buf.clear();
        self.b12.clear();
        self.w_bgn = 2;
        self.sync = true;
        self.sync_mode = 0;
        let (w, h) = (self.st.bw, self.st.bh);
        self.img = vec![0u8; w * h * 3];
        self.dirty = vec![false; h];
        self.emit_y = -10;
        self.events.push(SstvEvent::ModeDetected(m));
    }

    /// CSSTVDEM::Stop (sstv.cpp 1769-1791).
    fn stop(&mut self) {
        if self.afc_fq != 0 {
            self.init_tone(0);
        }
        self.sint1.reset();
        self.sint2.reset();
        self.w_bgn = 0;
        self.sync_mode = 512;
        self.sync = false;
        self.skip = 0;
    }

    fn unsupported_vis(&mut self, code: i32, mode: Option<usize>) {
        let c = (code & 0x7f) as u8;
        let name = match mode {
            Some(m) => Some(MODE_NAMES[m]),
            None => SstvMode::unsupported_name(c),
        };
        self.events.push(SstvEvent::UnsupportedMode { code: c, name });
    }

    /// CSSTVDEM::Do (sstv.cpp 1819-2322), one 12 kHz sample (16-bit scale).
    fn dem_do(&mut self, s: f64) {
        self.nsamp += 1;
        let mut d = (s + self.ad) * 0.5;
        self.ad = s;
        d = if self.sync || self.sync_mode >= 3 { self.bpf.run(d, &self.hbpf) } else { self.bpf.run(d, &self.hbpfs) };
        self.lvl.do_(d);
        self.lvl.fix();
        let ad = d * self.lvl.agc;
        let mut d = ad * 32.0;
        d = d.clamp(-16384.0, 16384.0);

        let d12 = self.lpf12.run(self.iir12.run(d).abs());
        let d19 = self.lpf19.run(self.iir19.run(d).abs());
        let dsp = self.lpffsk.run(self.iirfsk.run(d).abs());
        self.decode_fsk(d19 as i32, dsp as i32);
        if let Some(call) = self.fsk_rec.take() {
            // TimerTimer (Main.cpp 3211-3231): an ID received during a picture
            // stops it (RxAutoPush(TRUE)); the call goes to the log / HisCall.
            if self.sync {
                self.stop();
                self.write_history(false);
                self.sync_mode = 0;
            }
            self.events.push(SstvEvent::FskId(call));
        }

        // !m_Sync || m_SyncRestart (always 1) || m_SyncAVT
        self.sint1.inc();
        self.sint2.inc();
        let d11 = self.lpf11.run(self.iir11.run(d).abs());
        let (slvl, slvl2) = (self.slvl, self.slvl2);
        match self.sync_mode {
            0 => {
                if !self.sync {
                    let v = self.sint1.start(&self.ms);
                    if v > 0 {
                        self.freerun_start(v - 1);
                    } else if d12 > d19 && d12 > slvl2 && (d12 - d19) >= slvl2 {
                        self.sint2.max(d12 as i32);
                    } else {
                        let v = self.sint2.start(&self.ms);
                        if v > 0 {
                            match v - 1 {
                                SM_SCT1 | SM_MRT1 | SM_MRT2 | SM_SC2_180 => self.freerun_start(v - 1),
                                _ => {}
                            }
                        }
                    }
                    // m_sint3 (1900 Hz sync of the MMSSTV narrow modes) not ported
                }
                if d12 > d19 && d12 > slvl && (d12 - d19) >= slvl {
                    self.sync_mode += 1;
                    self.sync_time = (15.0 * SF / 1000.0) as i32;
                    if !self.sync {
                        self.sint1.trig(d12 as i32);
                    }
                }
            }
            1 => {
                if !self.sync && d12 > d19 && d12 > slvl2 && (d12 - d19) >= slvl2 {
                    self.sint2.max(d12 as i32);
                }
                if d12 > d19 && d12 > slvl && (d12 - d19) >= slvl {
                    if !self.sync {
                        self.sint1.max(d12 as i32);
                    }
                    self.sync_time -= 1;
                    if self.sync_time == 0 {
                        self.sync_mode += 1;
                        self.sync_time = (30.0 * SF / 1000.0) as i32;
                        self.vis_data = 0;
                        self.vis_cnt = 8;
                    }
                } else {
                    self.sync_mode = 0;
                }
            }
            2 | 9 => {
                let d13 = self.lpf13.run(self.iir13.run(d).abs());
                self.sync_time -= 1;
                if self.sync_time == 0 {
                    if (d11 < d19 && d13 < d19) || (d11 - d13).abs() < slvl2 {
                        if self.dbg { eprintln!("{:.3}s VIS reject cnt {} d11 {:.0} d13 {:.0} d19 {:.0}", self.nsamp as f64 / SF, self.vis_cnt, d11, d13, d19); }
                        self.sync_mode = 0;
                    } else {
                        self.sync_time = (30.0 * SF / 1000.0) as i32;
                        self.vis_data >>= 1;
                        if d11 > d13 {
                            self.vis_data |= 0x80;
                        }
                        if self.dbg { eprintln!("{:.3}s VIS bit {} d11 {:.0} d13 {:.0} d19 {:.0} d12 {:.0}", self.nsamp as f64 / SF, (d11 > d13) as u8, d11, d13, d19, d12); }
                        self.vis_cnt -= 1;
                        if self.vis_cnt == 0 {
                            self.vis_done();
                        }
                    }
                }
            }
            3 => {
                self.sync_time -= 1;
                if self.sync_time == 0 {
                    if d12 > d19 && d12 > slvl {
                        let nm = self.next_mode;
                        if to_mode(nm).is_some() {
                            if self.sync && self.r_base >= (self.st.lm * 65 / 100) as i64 {
                                self.req_save = true;
                            }
                            self.sync_mode = 256;
                        } else {
                            // AVT / MMSSTV-only mode: reported, not decoded
                            self.unsupported_vis(self.vis_data, Some(nm));
                            self.sync_mode = 0;
                        }
                    } else {
                        self.sync_mode = 0;
                    }
                }
            }
            256 => {
                let nm = self.next_mode;
                self.start(nm);
            }
            512 => {
                self.sync_time = (SF * 0.5) as i32;
                self.sync_mode += 1;
            }
            513 => {
                self.sync_time -= 1;
                if self.sync_time == 0 {
                    self.sync_mode = 0;
                }
            }
            _ => {}
        }

        if self.sync {
            let mut d = self.hill.run(self.lvl.cur);
            if self.lvl.cur_max > 16.0 {
                self.sync_freq(d);
            }
            d += self.afc_diff;
            if self.skip > 0 {
                self.skip -= 1;
            } else {
                // m_Buf is short: clamped here (MMSSTV converts double->short)
                self.buf.push((-d).clamp(-32768.0, 32767.0) as i16);
                self.b12.push(d12.clamp(-32768.0, 32767.0) as i16);
                self.inc_wp();
            }
        }
    }

    /// The 8 VIS bits are in (sstv.cpp 1990-2122).
    fn vis_done(&mut self) {
        let v = self.vis_data;
        if self.sync_mode == 2 {
            self.sync_mode += 1;
            self.next_mode = match v {
                0x82 => 35, // RM8
                0x86 => 36, // RM12
                0x84 => SM_R24,
                0x88 => SM_R36,
                0x0c => SM_R72,
                0x44 => SM_AVT,
                0x3c => SM_SCT1,
                0xb8 => SM_SCT2,
                0xcc => SM_SCTDX,
                0xac => SM_MRT1,
                0x28 => SM_MRT2,
                0xb7 => SM_SC2_180,
                0x3f => SM_SC2_120,
                0xbb => SM_SC2_60,
                0xdd => SM_PD50,
                0x63 => SM_PD90,
                0x5f => SM_PD120,
                0xe2 => SM_PD160,
                0x60 => SM_PD180,
                0xe1 => SM_PD240,
                0xde => SM_PD290,
                0x71 => SM_P3,
                0x72 => SM_P5,
                0xf3 => SM_P7,
                0x23 => {
                    self.sync_mode = 9;
                    self.vis_data = 0;
                    self.vis_cnt = 8;
                    return;
                }
                _ => {
                    self.sync_mode = 0;
                    return;
                }
            };
        } else {
            self.sync_mode = 3;
            self.next_mode = match v {
                0x45 => SM_MR73,
                0x46 => SM_MR90,
                0x49 => SM_MR115,
                0x4a => SM_MR140,
                0x4c => SM_MR175,
                0x25 => SM_MP73,
                0x29 => SM_MP115,
                0x2a => SM_MP140,
                0x2c => SM_MP175,
                0x85 => SM_ML180,
                0x86 => SM_ML240,
                0x89 => SM_ML280,
                0x8a => SM_ML320,
                _ => {
                    self.sync_mode = 0;
                    return;
                }
            };
        }
    }

    fn freerun_start(&mut self, mode: usize) {
        if to_mode(mode).is_some() {
            self.start(mode);
        }
        // a free-run match on an MMSSTV-only mode would start that mode
        // there; it is ignored here
    }

    /// CSSTVDEM::IncWP (sstv.cpp 2324-2337): a page (m_WD samples) is full.
    fn inc_wp(&mut self) {
        self.w_cnt += 1;
        if self.w_cnt >= self.st.wd {
            self.w_cnt = 0;
            self.w_line += 1;
            // m_StgBuf copy (Main.cpp 4573-4580): limited allocation
            if (self.stg_lines + 1) * self.st.wd < self.stg_alloc {
                self.stg_lines += 1;
            }
            self.draw_sstv();
        }
    }

    // ───────────── TMmsstv receive side ─────────────

    /// DrawSSTV(void) (Main.cpp 4498-4616) + the SyncAccuracy step of the
    /// GUI timer (Main.cpp 3146-3155).
    fn draw_sstv(&mut self) {
        while self.sync && self.r_page < self.w_line {
            if self.w_bgn != 0 {
                if self.w_bgn != 1 {
                    self.sync_rpos = -1;
                    self.sync_pos = -1;
                    self.sync_acc_n = 0;
                    self.dsel = 0;
                    self.ax = -1;
                    self.ay = -5;
                }
                self.w_bgn = 1;
                self.sync_sstv();
                if self.w_bgn != 0 {
                    return;
                }
                self.init_auto_stop();
                self.auto_sync_count = 0;
                self.auto_sync_dis = 0;
            }
            let wd = self.st.wd;
            let start = self.r_page * wd;
            self.draw_page(start, start + wd);
            self.r_base += wd as i64;
            if self.ay > self.st.l {
                if self.sync {
                    self.stop();
                    self.write_history(true);
                }
                break;
            }
            self.r_page += 1;
        }
        if self.req_samp_chg {
            // m_ReqSampChg = 1 -> next timer tick RedrawSampFreq(TRUE)
            self.req_samp_chg = false;
            if self.sync {
                self.update_samp_freq();
            }
        }
        if self.sync && self.sync_acc_n & 1 == 0 && self.ay >= 16 {
            self.sync_acc_n = 1;
            self.update_samp_freq();
        }
    }

    /// WriteHistory (Main.cpp 4461-4486): saved when sw or > 65 % drawn.
    fn write_history(&mut self, sw: bool) {
        let f = sw || self.ay > (self.st.l * 65 / 100);
        self.flush_rows(true);
        self.events.push(if f { SstvEvent::ImageComplete } else { SstvEvent::LockDropped });
    }

    /// SyncSSTV (Main.cpp 3343-3391): first line phase from the 1200 Hz
    /// detector summed over the first 4 (3 for >= 1 s lines) pages.
    fn sync_sstv(&mut self) {
        let e = if self.st.tw >= self.samp { 3 } else { 4 };
        if self.w_line >= e {
            let tw = self.st.tw;
            let wdn = tw as usize + 2;
            let mut bp = vec![0i64; wdn];
            let mut n = 0usize;
            for pg in 0..e {
                for i in 0..self.st.wd {
                    let x = (n as f64 % tw) as usize;
                    bp[x] += self.b12[pg * self.st.wd + i] as i64;
                    n += 1;
                }
            }
            let mut best = 0usize;
            let mut max = 0i64;
            for (i, &v) in bp.iter().enumerate() {
                if max < v {
                    max = v;
                    best = i;
                }
            }
            let mut n = (best as f64 - self.st.ofp) as i64;
            n = -n;
            if is_sct(self.st.mode) && n < 0 {
                n += self.st.wd as i64;
            }
            n -= (self.hill.htap / 4) as i64;
            self.ofs = n;
            self.r_base = n;
            self.w_bgn = 0;
            if self.dbg { eprintln!("SyncSSTV peak {} rBase {}", best, n); }
        }
    }

    /// AdjustSyncPos (Main.cpp 5002-5063).
    fn adjust_sync_pos(&self, n: i64) -> i64 {
        let mut n = (n as f64 - self.st.ofp) as i64;
        n = -n;
        let samp = self.samp;
        let ms = |v: f64| v / 1000.0 * samp;
        match self.st.mode {
            SM_SCT1 | SM_SCT2 | SM_SCTDX => {
                if n < 0 {
                    n += self.st.tw as i64;
                }
            }
            SM_MRT1 | SM_SC2_180 | SM_SC2_120 => n = (n as f64 + ms(0.45)) as i64,
            SM_MRT2 => n = (n as f64 + if self.stg_lines < 20 { ms(0.30) } else { ms(0.40) }) as i64,
            SM_R36 | SM_R72 => n = (n as f64 + ms(0.16)) as i64,
            _ => {}
        }
        n
    }

    /// ReSyncSSTV (Main.cpp 5065-5111): phase from up to 32 stored lines.
    fn resync_sstv(&mut self) {
        let tw = self.st.tw;
        let wdn = tw as usize;
        let mut bp = vec![0i64; wdn + 2];
        let mut n = 0usize;
        for i in 0..self.stg_lines.min(32) {
            for j in 0..self.st.wd {
                let x = (n as f64 % tw) as usize;
                bp[x] += self.b12[i * self.st.wd + j] as i64;
                n += 1;
            }
        }
        let mut best = 0usize;
        let mut max = 0i64;
        for i in 0..wdn {
            if max < bp[i] {
                max = bp[i];
                best = i;
            }
        }
        let mut n = self.adjust_sync_pos(best as i64);
        n -= (self.hill.htap / 4) as i64;
        self.ofs = n;
        self.r_base = n;
    }

    /// RedrawSampFreq(FALSE) -> UpdateSampFreq (Main.cpp 5155-5203): resync
    /// and redraw everything stored so far.
    fn update_samp_freq(&mut self) {
        self.st.set_samp(self.samp);
        if self.dbg { eprintln!("{:.3}s REDRAW samp {:.2} TW {:.3} y {}", self.nsamp as f64 / SF, self.samp, self.st.tw, self.ay); }
        self.resync_sstv();
        self.init_auto_stop();
        self.as_dis = true;
        self.r_base = self.ofs;
        let wd = self.st.wd;
        let lim = self.stg_lines * wd;
        for i in 0..self.stg_lines {
            self.draw_page(i * wd, lim);
            self.r_base += wd as i64;
        }
        self.as_dis = false;
        self.flush_rows(false);
    }

    /// InitAutoStop (Main.cpp 3393-3455), slant parts omitted (off).
    fn init_auto_stop(&mut self) {
        self.as_bgn_pos = NO_POS;
        self.as_bit_mask = 0;
        self.as_avg.reset(); // m_ASAvg.SetCount(16)
        let l = self.st.l;
        self.as_posl = match self.st.mode {
            SM_PD50 | SM_PD90 => [48, 64, 72, 110],
            SM_PD160 => [48, 80, 126, 160],
            SM_PD290 => [64, 128, 160, 240],
            _ => [64, 128, 160, l - 36],
        };
        self.as_apos = [0; 16];
        self.as_cnt = 0;
        self.as_acnt = 0;
        self.as_pos = 0;
        self.as_cur_y = 0;
        self.auto_sync_pos = NO_POS;
        self.mult = (self.st.tw / 320.0) as i32;
        self.auto_sync_diff = self.mult * 3;
        let cap = (45.0 * SF / 11025.0) as i32;
        if self.auto_sync_diff > cap {
            self.auto_sync_diff = cap;
        }
    }

    fn set_skip(&mut self, df: i32) {
        let mut df = df;
        if df < 0 {
            df = (df as f64 + self.st.tw) as i32;
        }
        self.skip = df;
        if self.dbg { eprintln!("{:.3}s SKIP {} y {}", self.nsamp as f64 / SF, df, self.ay); }
        self.sync_pos = -1;
        self.sync_rpos = -1;
        self.auto_sync_pos = NO_POS;
        self.auto_sync_dis = 6;
        self.auto_sync_count += 1;
        if self.as_cnt != 0 {
            self.as_cnt -= 1;
        }
    }

    /// AutoStopJob (Main.cpp 3476-3627) with AutoSync = 1, UseRxBuff = 1,
    /// AutoSlant (KRSA) = 0, AutoStop = off.
    fn auto_stop_job(&mut self) -> bool {
        let tw = self.st.tw;
        let mut pos = self.sync_pos - self.st.ofp as i32;
        let h = (tw / 2.0) as i32;
        if pos > h {
            pos = (pos as f64 - tw) as i32;
        }
        self.as_pos = pos;
        let m = self.mult;
        let span = self.sync_max - self.sync_min;
        if self.as_acnt >= 8 {
            let lim = if self.as_acnt >= 16 { 14 * m } else { 10 * m };
            let n = self.as_apos.iter().filter(|&&a| (pos - a).abs() <= lim).count();
            if n < 4 {
                if !self.as_dis && n >= 2 && self.auto_sync_pos != NO_POS && self.auto_sync_count == 0 && span > 5000 {
                    let df = pos - self.as_apos[15];
                    let k = if self.slant { 5 } else { 2 };
                    if df.abs() <= k * m {
                        let df = pos - self.auto_sync_pos;
                        if df.abs() >= k * m {
                            self.set_skip(pos);
                        }
                    }
                }
                if n < 2 || span < 8192 {
                    self.as_cnt += 1;
                }
                if self.autostop && !self.as_dis && self.as_cnt >= 8 {
                    // RxAutoPush(TRUE) (Main.cpp 5615-5633)
                    self.stop();
                    self.write_history(false);
                    self.sync_mode = 0;
                    return true;
                }
            } else {
                self.auto_sync_pos = pos;
                self.as_cnt -= 2;
                if self.as_cnt < 0 {
                    self.as_cnt = 0;
                }
            }
            if !self.as_dis && self.auto_sync_dis == 0 && self.auto_sync_pos != NO_POS && self.auto_sync_count != 0 && span > 5000 {
                let df = pos - self.as_apos[15];
                if df.abs() <= self.auto_sync_diff && pos.abs() >= self.auto_sync_diff {
                    self.set_skip(pos);
                }
            }
        }
        if self.auto_sync_dis != 0 {
            self.auto_sync_dis -= 1;
        }
        self.as_acnt += 1;
        self.as_apos.copy_within(1.., 0);
        self.as_apos[15] = pos;
        if self.slant && self.auto_sync_count == 0 && self.as_acnt >= 5 {
            let a = &self.as_apos;
            let mut df = 0;
            for k in 0..5 {
                df = df.max((a[15 - k] - a[14 - k]).abs());
            }
            if df < 8 * m {
                let pos = self.sqerr_pos(5);
                if self.as_bgn_pos == NO_POS {
                    self.as_bgn_pos = pos;
                    self.as_cur_y = 0;
                } else if self.as_cur_y >= 3 {
                    let mut d = (self.as_bgn_pos - pos) as f64 * self.samp / tw;
                    d /= self.as_cur_y as f64;
                    let mut smp = self.as_avg.avg(self.samp - d);
                    let d = self.samp - smp;
                    let (y, mk, lm, ap) = (self.as_cur_y, self.as_bit_mask, self.as_lmt, self.as_posl);
                    let ad = d.abs();
                    if (mk & 1 == 0 && ad >= lm[0])
                        || (mk & 1 == 0 && y >= 16 && ad >= lm[1])
                        || (mk & 1 == 0 && y >= 32 && ad >= lm[2])
                        || (mk & 2 == 0 && y >= ap[0] && ad >= lm[3])
                        || (mk & 4 == 0 && y >= ap[1] && ad >= lm[4])
                        || (mk & 8 == 0 && y >= ap[2] && ad >= lm[5])
                        || (mk & 16 == 0 && y >= ap[3] && ad >= lm[6])
                    {
                        if y >= 32 { self.as_bit_mask |= 1; }
                        if y >= ap[0] { self.as_bit_mask |= 2; }
                        if y >= ap[1] { self.as_bit_mask |= 4; }
                        if y >= ap[2] { self.as_bit_mask |= 8; }
                        if y >= ap[3] { self.as_bit_mask |= 16; }
                        if !self.as_dis {
                            if smp > SF * 1100.0 / 1060.0 {
                                smp = SF * 1100.0 / 1060.0;
                            }
                            // NormalSampFreq(smp, 50) (ComLib.cpp 203-207)
                            self.samp = ((smp * 50.0 + 0.5) as i64) as f64 / 50.0;
                            self.req_samp_chg = true;
                            if self.dbg { eprintln!("{:.3}s SLANT y {} d {:.3} -> samp {:.2}", self.nsamp as f64 / SF, y, d, self.samp); }
                        }
                    }
                }
            }
        }
        self.as_cur_y += 1;
        false
    }

    /// GetSqerrPos (Main.cpp 3459-3473): least-squares value at the newest
    /// of the last n sync positions.
    fn sqerr_pos(&self, n: usize) -> i32 {
        let (mut t, mut l, mut tt, mut tl) = (0f64, 0f64, 0f64, 0f64);
        for i in 0..n {
            let p = self.as_apos[15 - i] as f64;
            let fi = i as f64;
            t += fi;
            l += p;
            tt += fi * fi;
            tl += fi * p;
        }
        ((l * tt - t * tl) / (n as f64 * tt - t * t)) as i32
    }

    /// CSSTVDEM::DecodeFSK (sstv.cpp 2378-2606). m = 1900 Hz detector (mark,
    /// "1"), s = 2100 Hz detector (FSKSPACE, "0"). 100 ms space (FSKGARD),
    /// 1900 Hz start bit, then 6-bit symbols LSB first at 22 ms (FSKINTVAL):
    /// 0x2A, chars - 0x20, 0x01, XOR checksum.
    fn decode_fsk(&mut self, m: i32, s: i32) {
        let sf = SF; // SSTVSET.m_SampFreq at the base rate (rate changes only touch picture timing)
        let intval = 22.0;
        let d = (m - s).abs();
        match self.fsk_mode {
            0 => {
                if s > m && d >= 2048 {
                    self.fsk_time = ((100 / 2) as f64 * sf / 1000.0) as i32;
                    self.fsk_mode = 1;
                }
            }
            1 => {
                if s > m && d >= 2048 {
                    self.fsk_time -= 1;
                    if self.fsk_time == 0 {
                        self.fsk_time = (100.0 * sf / 1000.0) as i32;
                        self.fsk_mode = 2;
                    }
                } else {
                    self.fsk_mode = 0;
                }
            }
            2 => {
                self.fsk_time -= 1;
                if self.fsk_time == 0 {
                    self.fsk_mode = 0;
                } else if m > s && d >= 2048 {
                    self.fsk_time = ((22 / 2) as f64 * sf / 1000.0) as i32;
                    self.fsk_mode = 3;
                }
            }
            3 => {
                self.fsk_time -= 1;
                if self.fsk_time == 0 {
                    if m > s && d >= 2048 {
                        self.fsk_time = 0;
                        self.fsk_nextd = intval / 1000.0 * sf;
                        self.fsk_nexti = self.fsk_nextd as i32;
                        self.fsk_bcnt = 0;
                        self.fsk_c = 0;
                        self.fsk_mode = 4;
                    } else {
                        self.fsk_mode = 0;
                    }
                }
            }
            _ => {
                self.fsk_time += 1;
                if self.fsk_time >= self.fsk_nexti {
                    if d < 2048 {
                        self.fsk_mode = 0;
                        return;
                    }
                    self.fsk_nextd += intval / 1000.0 * sf;
                    self.fsk_nexti = self.fsk_nextd as i32;
                    self.fsk_c >>= 1;
                    if m > s {
                        self.fsk_c |= 0x20;
                    }
                    self.fsk_bcnt += 1;
                    if self.fsk_bcnt >= 6 {
                        self.fsk_bcnt = 0;
                        self.fsk_symbol();
                        self.fsk_c = 0;
                    }
                }
            }
        }
    }

    fn fsk_symbol(&mut self) {
        if self.dbg { eprintln!("{:.3}s FSK sym mode {} c {:#04x}", self.nsamp as f64 / SF, self.fsk_mode, self.fsk_c); }
        let c = self.fsk_c;
        match self.fsk_mode {
            4 => {
                if c == 0x2a || c == 0x2d {
                    self.fsk_cnt = 0;
                    self.fsk_bcnt = 0;
                    self.fsk_s = 0;
                    self.fsk_mode = if c == 0x2a { 5 } else { 16 };
                } else {
                    self.fsk_mode = 0;
                }
            }
            5 => {
                if c == 0x01 {
                    self.fsk_mode = if self.fsk_cnt >= 1 { 6 } else { 0 };
                } else {
                    self.fsk_s ^= c;
                    self.fsk_data[self.fsk_cnt] = c + 0x20;
                    self.fsk_cnt += 1;
                    if self.fsk_cnt >= 17 {
                        self.fsk_mode = 0;
                    }
                }
            }
            6 => {
                self.fsk_s &= 0x3f;
                if c == self.fsk_s && self.fskid {
                    // StrCopy(m_fskcall, SkipSpace(data), 16); clipsp()
                    let txt: String = self.fsk_data[..self.fsk_cnt].iter().map(|&b| b as char).collect();
                    let call: String = txt.trim_start_matches(' ').chars().take(16).collect::<String>().trim_end_matches(' ').to_string();
                    if self.dbg { eprintln!("{:.3}s FSKID {:?}", self.nsamp as f64 / SF, call); }
                    self.fsk_rec = Some(call);
                    self.fsk_mode = 7;
                    self.fsk_cnt = 0;
                    self.fsk_s = 0;
                } else {
                    self.fsk_mode = 0;
                }
            }
            7 => {
                // RST / serial number after the call (m_fskNRS); not reported
                if c == 0x01 {
                    self.fsk_mode = if self.fsk_cnt >= 1 { 8 } else { 0 };
                } else if c == 0x02 {
                    self.fsk_s = 0x02;
                    self.fsk_nr = 0;
                    self.fsk_mode = 9;
                } else if c >= 0x10 {
                    self.fsk_s ^= c;
                    self.fsk_cnt += 1;
                    if self.fsk_cnt >= 9 {
                        self.fsk_mode = 0;
                    }
                } else {
                    self.fsk_mode = 0;
                }
            }
            9 => {
                self.fsk_s ^= c;
                self.fsk_nr = (self.fsk_nr << 6) + c as i32;
                self.fsk_cnt += 1;
                if self.fsk_cnt >= 2 {
                    self.fsk_mode = 10;
                }
            }
            16 => {
                self.fsk_s ^= c;
                self.fsk_mode = if c == 0x15 { 17 } else { 0 };
            }
            17 => {
                self.fsk_s ^= c;
                self.fsk_mode = 18;
            }
            _ => {
                // 8, 10: checksum of the RST part; 18: narrow-mode switch
                // (MMSSTV-only modes) - no output here
                self.fsk_mode = 0;
            }
        }
    }

    fn pix(v: i16) -> i32 {
        // GetPixelLevel: (d - m_DemOff) * m_DemWhite/Black, int truncation
        (v as i32 as f64 * (128.0 / 16384.0)) as i32
    }
    fn pic(&self, i: usize, lim: usize) -> i32 {
        // GetPictureLevel (Main.cpp 3650-3665): the larger of now and KSB later
        let k = self.st.ksb;
        if i + k < lim && self.buf[i] < self.buf[i + k] {
            Self::pix(self.buf[i + k])
        } else {
            Self::pix(self.buf[i])
        }
    }
    fn lim256(d: i32) -> i32 {
        d.clamp(0, 255)
    }
    fn yc_rgb(y: i32, ry: i32, by: i32) -> (u8, u8, u8) {
        let y = (y - 16) as f64;
        let (ry, by) = (ry as f64, by as f64);
        let r = (1.164457 * y + 1.596128 * ry) as i32;
        let g = (1.164457 * y - 0.813022 * ry - 0.391786 * by) as i32;
        let b = (1.164457 * y + 2.017364 * by) as i32;
        (Self::lim256(r) as u8, Self::lim256(g) as u8, Self::lim256(b) as u8)
    }
    fn put(&mut self, row: i32, x: i32, rgb: (u8, u8, u8)) {
        if row < 0 || row as usize >= self.st.bh {
            return;
        }
        let i = (row as usize * self.st.bw + x as usize) * 3;
        self.img[i] = rgb.0;
        self.img[i + 1] = rgb.1;
        self.img[i + 2] = rgb.2;
        self.dirty[row as usize] = true;
    }
    /// Emit finished rows as Line events (all dirty rows when `all`).
    fn flush_rows(&mut self, _all: bool) {
        let m = match to_mode(self.st.mode) {
            Some(m) => m,
            None => return,
        };
        let (w, h) = m.dimensions();
        for r in 0..self.dirty.len() {
            if self.dirty[r] {
                self.dirty[r] = false;
                if r < h as usize {
                    let s = r * self.st.bw * 3;
                    let rgb = self.img[s..s + w as usize * 3].to_vec();
                    self.events.push(SstvEvent::Line { y: r as u16, rgb });
                }
            }
        }
    }

    /// DrawSSTVNormal (Main.cpp 3715-4087) for buf[start..start+WD], with
    /// GetPictureLevel looking no further than `lim`.
    fn draw_page(&mut self, start: usize, lim: usize) {
        let st = self.st.clone();
        let mode = st.mode;
        let wbm = st.bw as f64;
        let mut n = self.r_base;
        let mut ay_l = -5i32;
        let (mut row1, mut row2) = (-1i32, -1i32);
        for i in 0..st.wd {
            let idx = start + i;
            if n < 0 {
                n += 1;
                continue;
            }
            if idx >= self.buf.len() {
                break;
            }
            let mut ps = (n as f64) % st.tw;
            let y = (n as f64 / st.tw) as i32;
            if ay_l != y {
                self.ay = y;
                ay_l = y;
                if y != self.emit_y {
                    self.flush_rows(false);
                    self.emit_y = y;
                }
                row2 = -1;
                if is_sct(mode) {
                    row1 = if y > 0 && y <= 256 { y - 1 } else { -1 };
                } else if is_pd(mode) {
                    if y >= 0 && y < st.l {
                        row1 = y * 2;
                        row2 = y * 2 + 1;
                    } else {
                        row1 = -1;
                    }
                } else {
                    row1 = if y >= 0 && (y as usize) < st.bh { y } else { -1 };
                }
            }
            let sp = self.b12[idx] as i32;
            if ps as i32 == 0 {
                if self.sync && self.sync_pos != -1 {
                    self.auto_stop_job();
                }
                self.sync_min = sp;
                self.sync_max = sp;
                self.sync_rpos = self.sync_pos;
            } else if self.sync_max < sp {
                self.sync_max = sp;
                self.sync_pos = ps as i32;
            } else if self.sync_min > sp {
                self.sync_min = sp;
            }
            if ps >= st.of {
                ps -= st.of;
                let xof = |p: f64, k: f64| (p * wbm / k) as i32;
                match mode {
                    SM_SCT1 | SM_SCT2 | SM_SCTDX => {
                        let lvl = |s: &Self| if mode == SM_SCTDX { Self::pix(s.buf[idx]) } else { s.pic(idx, lim) };
                        if ps < st.ks {
                            let x = xof(ps, st.kss);
                            if x != self.ax && x >= 0 && x < 320 {
                                self.ax = x;
                                let d = Self::lim256(lvl(self) + 128);
                                let (g, b) = (self.d36[0][x as usize], self.d36[1][x as usize]);
                                self.put(row1, x, (d as u8, g as u8, b as u8));
                            }
                        } else if ps < st.cg {
                            let x = xof(ps - st.sg, st.kss);
                            if x != self.ax && x >= 0 && x < 320 {
                                self.ax = x;
                                self.d36[0][x as usize] = Self::lim256(lvl(self) + 128);
                            }
                        } else if ps < st.cb {
                            let x = xof(ps - st.sb, st.kss);
                            if x != self.ax && x >= 0 && x < 320 {
                                self.ax = x;
                                self.d36[1][x as usize] = Self::lim256(lvl(self) + 128);
                            }
                        }
                    }
                    SM_R36 => {
                        if ps < st.ks {
                            let x = xof(ps, st.kss);
                            if self.ax != x && x >= 0 && x < 320 {
                                self.ax = x;
                                self.y36[x as usize] = Self::lim256(self.pic(idx, lim) + 128);
                            }
                        } else if ps < st.cg {
                            if ps - st.sg >= 0.0 {
                                let d = Self::pix(self.buf[idx]);
                                if d >= 64 || d < -64 {
                                    self.dsel = if d >= 0 { 1 } else { 0 };
                                } else {
                                    self.dsel ^= 1;
                                }
                            }
                        } else if ps < st.cb {
                            let x = xof(ps - st.sb, st.ks2s);
                            if self.ax != x && x >= 0 && x < 320 && y < 240 {
                                self.ax = x;
                                let d = Self::pix(self.buf[idx]);
                                let xu = x as usize;
                                self.d36[self.dsel][xu] = d;
                                let c = Self::yc_rgb(self.y36[xu], self.d36[0][xu], self.d36[1][xu]);
                                self.put(row1, x, c);
                            }
                        }
                    }
                    SM_R72 => {
                        let wb = st.bw as i32;
                        if ps < st.ks {
                            let x = xof(ps, st.kss);
                            if self.ax != x && x >= 0 && x < wb {
                                self.ax = x;
                                self.y36[x as usize] = Self::lim256(self.pic(idx, lim) + 128);
                            }
                        } else if ps < st.cg {
                            let x = xof(ps - st.sg, st.ks2s);
                            if self.ax != x && x >= 0 && x < wb {
                                self.ax = x;
                                self.d36[1][x as usize] = Self::pix(self.buf[idx]);
                            }
                        } else if ps < st.cb {
                            let x = xof(ps - st.sb, st.ks2s);
                            if self.ax != x && x >= 0 && x < wb && y < st.l {
                                self.ax = x;
                                let d = Self::pix(self.buf[idx]);
                                let xu = x as usize;
                                let c = Self::yc_rgb(self.y36[xu], self.d36[1][xu], d);
                                self.put(row1, x, c);
                            }
                        }
                    }
                    m if is_pd(m) => {
                        let wb = st.bw as i32;
                        if ps < st.ks {
                            let x = xof(ps, st.kss);
                            if self.ax != x && x >= 0 && x < wb {
                                self.ax = x;
                                self.y36[x as usize] = Self::lim256(self.pic(idx, lim) + 128);
                            }
                        } else if ps < st.cg {
                            let x = xof(ps - st.sg, st.kss);
                            if self.ax != x && x >= 0 && x < wb {
                                self.ax = x;
                                self.d36[1][x as usize] = Self::pix(self.buf[idx]);
                            }
                        } else if ps < st.cb {
                            let x = xof(ps - st.sb, st.kss);
                            if self.ax != x && x >= 0 && x < wb {
                                self.ax = x;
                                self.d36[0][x as usize] = Self::pix(self.buf[idx]);
                            }
                        } else if ps < st.cb + st.ks {
                            let x = xof(ps - st.cb, st.kss);
                            if self.ax != x && x >= 0 && x < wb && y < st.l {
                                self.ax = x;
                                if row1 >= 0 {
                                    let xu = x as usize;
                                    let (ry, by) = (self.d36[1][xu], self.d36[0][xu]);
                                    let c = Self::yc_rgb(self.y36[xu], ry, by);
                                    self.put(row1, x, c);
                                    let d = self.pic(idx, lim) + 128;
                                    let c2 = Self::yc_rgb(d, ry, by);
                                    self.put(row2, x, c2);
                                }
                            }
                        }
                    }
                    _ => {
                        // Martin (G B R) and SC2 (R G B)
                        let wb = st.bw as i32;
                        if ps < st.ks {
                            let x = xof(ps, st.kss);
                            if x != self.ax && x >= 0 && x < wb {
                                self.ax = x;
                                self.d36[0][x as usize] = Self::lim256(self.pic(idx, lim) + 128);
                            }
                        } else if ps < st.cg {
                            let x = xof(ps - st.sg, st.kss);
                            if x != self.ax && x >= 0 && x < wb {
                                self.ax = x;
                                self.d36[1][x as usize] = Self::lim256(self.pic(idx, lim) + 128);
                            }
                        } else if ps < st.cb {
                            let x = xof(ps - st.sb, st.kss);
                            if x != self.ax && x >= 0 && x < wb {
                                self.ax = x;
                                let d = Self::lim256(self.pic(idx, lim) + 128);
                                let xu = x as usize;
                                let (a, b) = (self.d36[0][xu] as u8, self.d36[1][xu] as u8);
                                if mode == SM_MRT1 || mode == SM_MRT2 {
                                    self.put(row1, x, (d as u8, a, b));
                                } else {
                                    self.put(row1, x, (a, b, d as u8));
                                }
                            }
                        }
                    }
                }
            }
            n += 1;
        }
    }
}
