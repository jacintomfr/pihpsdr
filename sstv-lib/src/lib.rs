// SSTV modem for piHPSDR-IF, GPL-3.0-or-later.
//
// sstv.rs is hpsdr-rs's src/sstv.rs (commit 49e0ea9) copied UNCHANGED - itself
// adapted from SDRoxide (https://github.com/madmedicnl/sdroxide, GPL-3.0-or-
// later). This file is the glue: the receive side of hpsdr-rs's
// src/sstv_link.rs (snapshot of the picture being decoded, auto-save as PNG,
// FSK ID, unsupported-mode report), behind a small C interface used by
// src/sstv_glue.c. Same behaviour as hpsdr-rs on purpose.

#![allow(dead_code)]

mod sstv;

use std::ffi::{c_char, c_int, CStr};
use std::path::PathBuf;
use std::sync::Mutex;

use sstv::{SstvEvent, SstvMode, SstvRx};

/// RX audio rate: piHPSDR's receiver output (and hpsdr-rs's OUTPUT_RATE).
const SAMPLE_RATE_HZ: f64 = 48_000.0;

struct Snapshot {
    image_id: u32,
    w: u16,
    h: u16,
    rgb: Vec<u8>,
    receiving: bool,
    progress: f32,
    detected: Option<SstvMode>,
    unsupported: Option<String>,
    rx_id: Option<String>,
    saved_count: u32,
    last_saved: String,
}

pub struct Sstv {
    rx: Mutex<SstvRx>,
    snap: Mutex<Snapshot>,
    level: Mutex<(f32, f32)>, // (level, sync quality)
    expected: Mutex<Option<SstvMode>>,
    save_dir: Mutex<Option<PathBuf>>, // auto-save folder, None: off
}

/// Status for the C side (sstv_lib.h, sstv_status_t)
#[repr(C)]
pub struct SstvStatus {
    pub image_id: u32,
    pub w: u16,
    pub h: u16,
    pub receiving: c_int,
    pub progress: f32,
    pub detected: c_int, // index into sstv_mode_label(), -1: none
    pub level: f32,
    pub sync_quality: f32,
    pub saved_count: u32,
    pub unsupported: [c_char; 32],
    pub rx_id: [c_char; 32],
    pub last_saved: [c_char; 256],
}

fn mode_index(m: SstvMode) -> c_int {
    SstvMode::ALL.iter().position(|&x| x == m).map(|i| i as c_int).unwrap_or(-1)
}

fn copy_cstr(dst: &mut [c_char], s: &str) {
    let n = s.len().min(dst.len() - 1);
    for (i, b) in s.as_bytes()[..n].iter().enumerate() {
        dst[i] = *b as c_char;
    }
    dst[n] = 0;
}

/// hpsdr-rs config::sstv_image_path, with the program's name
fn image_path(dir: &PathBuf, label: &str) -> Option<PathBuf> {
    std::fs::create_dir_all(dir).ok()?;
    let secs = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).ok()?.as_secs();
    let mut p = dir.clone();
    p.push(format!("pihpsdr-if_{secs}_{label}.png"));
    Some(p)
}

impl Sstv {
    fn new() -> Self {
        let mut rx = SstvRx::new(SAMPLE_RATE_HZ);
        rx.set_expected(None); // Auto
        Sstv {
            rx: Mutex::new(rx),
            snap: Mutex::new(Snapshot {
                image_id: 0,
                w: 0,
                h: 0,
                rgb: Vec::new(),
                receiving: false,
                progress: 0.0,
                detected: None,
                unsupported: None,
                rx_id: None,
                saved_count: 0,
                last_saved: String::new(),
            }),
            level: Mutex::new((0.0, 0.0)),
            expected: Mutex::new(None),
            save_dir: Mutex::new(None),
        }
    }

    // hpsdr-rs sstv_link.rs SstvHandle::feed_rx
    fn feed_rx(&self, audio: &[f32]) {
        if audio.is_empty() {
            return;
        }
        let mut rx = self.rx.lock().unwrap();
        let mut events = Vec::new();
        rx.process(audio, &mut events);
        *self.level.lock().unwrap() = (rx.level(), rx.sync_quality());
        let receiving = rx.receiving();
        let progress = rx.progress();
        drop(rx);
        let mut snap = self.snap.lock().unwrap();
        snap.receiving = receiving;
        snap.progress = progress;
        for e in events {
            match e {
                SstvEvent::ModeDetected(mode) => {
                    snap.image_id = snap.image_id.wrapping_add(1);
                    let (w, h) = mode.dimensions();
                    snap.w = w;
                    snap.h = h;
                    snap.rgb = vec![0u8; w as usize * h as usize * 3];
                    snap.detected = Some(mode);
                    snap.unsupported = None;
                }
                SstvEvent::Line { y, rgb } => {
                    let w = snap.w as usize;
                    let row = y as usize * w * 3;
                    if row + rgb.len() <= snap.rgb.len() {
                        snap.rgb[row..row + rgb.len()].copy_from_slice(&rgb);
                    }
                }
                SstvEvent::ImageComplete => {
                    let dir = self.save_dir.lock().unwrap().clone();
                    if let Some(dir) = dir {
                        let label = snap.detected.map(|m| m.label()).unwrap_or("Unknown");
                        if let Some(path) = image_path(&dir, label) {
                            if let Some(img) =
                                image::RgbImage::from_raw(snap.w as u32, snap.h as u32, snap.rgb.clone())
                            {
                                match img.save(&path) {
                                    Ok(()) => {
                                        snap.saved_count = snap.saved_count.wrapping_add(1);
                                        snap.last_saved = path.display().to_string();
                                    }
                                    Err(e) => eprintln!("sstv: failed to save {}: {e}", path.display()),
                                }
                            }
                        }
                    }
                }
                SstvEvent::FskId(id) => {
                    snap.rx_id = Some(id);
                }
                SstvEvent::UnsupportedMode { code, name } => {
                    snap.unsupported =
                        Some(name.map(|n| n.to_string()).unwrap_or_else(|| format!("VIS ${code:02X}")));
                }
            }
        }
    }

    // hpsdr-rs sstv_link.rs SstvHandle::restart
    fn restart(&self) {
        self.rx.lock().unwrap().restart();
        let mut snap = self.snap.lock().unwrap();
        snap.image_id = snap.image_id.wrapping_add(1);
        snap.w = 0;
        snap.h = 0;
        snap.rgb.clear();
        snap.receiving = false;
        snap.progress = 0.0;
        snap.detected = None;
    }
}

// ───────────────────────────── C interface ─────────────────────────────

#[no_mangle]
pub extern "C" fn sstv_new() -> *mut Sstv {
    Box::into_raw(Box::new(Sstv::new()))
}

/// # Safety
/// `s` from sstv_new(), not used afterwards.
#[no_mangle]
pub unsafe extern "C" fn sstv_free(s: *mut Sstv) {
    if !s.is_null() {
        drop(Box::from_raw(s));
    }
}

/// Mono 48 kHz receiver audio.
/// # Safety
/// `audio` points to `n` floats.
#[no_mangle]
pub unsafe extern "C" fn sstv_feed_rx(s: *const Sstv, audio: *const f32, n: usize) {
    if s.is_null() || audio.is_null() || n == 0 {
        return;
    }
    (*s).feed_rx(std::slice::from_raw_parts(audio, n));
}

/// # Safety
/// `s` from sstv_new().
#[no_mangle]
pub unsafe extern "C" fn sstv_restart(s: *const Sstv) {
    if !s.is_null() {
        (*s).restart();
    }
}

/// Free-run decoding pinned to a mode (index of sstv_mode_label), -1: Auto.
/// # Safety
/// `s` from sstv_new().
#[no_mangle]
pub unsafe extern "C" fn sstv_set_expected(s: *const Sstv, mode: c_int) {
    if s.is_null() {
        return;
    }
    let m = if mode >= 0 { SstvMode::ALL.get(mode as usize).copied() } else { None };
    *(*s).expected.lock().unwrap() = m;
    (*s).rx.lock().unwrap().set_expected(m);
}

/// Auto-save completed pictures as PNG in `dir` (UTF-8), NULL: off.
/// # Safety
/// `dir` NULL or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn sstv_set_auto_save(s: *const Sstv, dir: *const c_char) {
    if s.is_null() {
        return;
    }
    let d = if dir.is_null() {
        None
    } else {
        CStr::from_ptr(dir).to_str().ok().map(PathBuf::from)
    };
    *(*s).save_dir.lock().unwrap() = d;
}

#[no_mangle]
pub extern "C" fn sstv_mode_count() -> c_int {
    SstvMode::ALL.len() as c_int
}

/// Static, NUL-terminated label of mode `i` ("Scottie 1", ...), NULL if out of range.
#[no_mangle]
pub extern "C" fn sstv_mode_label(i: c_int) -> *const c_char {
    static LABELS: std::sync::OnceLock<Vec<std::ffi::CString>> = std::sync::OnceLock::new();
    let labels = LABELS.get_or_init(|| {
        SstvMode::ALL.iter().map(|m| std::ffi::CString::new(m.label()).unwrap()).collect()
    });
    if i < 0 || i as usize >= labels.len() {
        return std::ptr::null();
    }
    labels[i as usize].as_ptr()
}

/// # Safety
/// `s` from sstv_new(), `st` a valid SstvStatus.
#[no_mangle]
pub unsafe extern "C" fn sstv_get_status(s: *const Sstv, st: *mut SstvStatus) {
    if s.is_null() || st.is_null() {
        return;
    }
    let snap = (*s).snap.lock().unwrap();
    let (level, sq) = *(*s).level.lock().unwrap();
    let o = &mut *st;
    o.image_id = snap.image_id;
    o.w = snap.w;
    o.h = snap.h;
    o.receiving = snap.receiving as c_int;
    o.progress = snap.progress;
    o.detected = snap.detected.map(mode_index).unwrap_or(-1);
    o.level = level;
    o.sync_quality = sq;
    o.saved_count = snap.saved_count;
    copy_cstr(&mut o.unsupported, snap.unsupported.as_deref().unwrap_or(""));
    copy_cstr(&mut o.rx_id, snap.rx_id.as_deref().unwrap_or(""));
    copy_cstr(&mut o.last_saved, &snap.last_saved);
}

/// Copies the picture (interleaved RGB, w*h*3 bytes) into `dst`; returns the
/// number of bytes copied (0: no picture yet or `cap` too small).
/// # Safety
/// `dst` points to `cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn sstv_copy_rgb(s: *const Sstv, dst: *mut u8, cap: usize) -> usize {
    if s.is_null() || dst.is_null() {
        return 0;
    }
    let snap = (*s).snap.lock().unwrap();
    let n = snap.rgb.len();
    if n == 0 || n > cap {
        return 0;
    }
    std::ptr::copy_nonoverlapping(snap.rgb.as_ptr(), dst, n);
    n
}
