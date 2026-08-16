#!/usr/bin/env python3
"""
inspect_audio.py — look at what the engine actually produced, not at a summary of it.

WHY THIS EXISTS. Every measurement in this project so far has been a scalar: cents error,
level, periodicity, band energy. Scalars are how VH-001 survived — a broken shifter scored
healthy on three of them at once, because a no-op preserves formants, preserves periodicity
and leaves no gaps. A scalar can only answer the question you thought to ask. A spectrogram
shows you the question you did not.

The specific thing that prompted this: after the VH-001 fix, large downward shifts sound
"like a square wave" — the pulse rate is right but each pulse does not fill its period.
That is a shape, not a number, and no scalar in the repo would have named it.

FOUR VIEWS, because they fail in different places:

  spectrogram  frequency content over time. Shows harmonic structure, sidebands, and the
               broadband vertical stripes that a discontinuity makes.
  waveform     the samples themselves, zoomed. This is the ONLY view that shows the shape
               of the gap between pulses, which is the current question.
  f0           pitch over time, unconstrained. Shows octave jumps and tracking failure
               that a median over the whole file averages away.
  duty         fraction of each output period carrying energy. The scalar version of
               "the grain does not fill the period" — included so the artefact can be
               regression-tested once it is understood.

PNG OUTPUT IS OPTIONAL. matplotlib is imported lazily and only for --png. Without it every
view still prints as text, so this runs anywhere the rest of the harness runs. Same
reasoning as tools/wav.hpp not taking libsndfile: take the dependency at the moment it is
necessary, not before.

USAGE
  python3 tools/inspect_audio.py FILE.wav                       # all text views
  python3 tools/inspect_audio.py FILE.wav --png out/            # + images
  python3 tools/inspect_audio.py FILE.wav --from 11.2 --to 16.2 # one sweep segment
  python3 tools/inspect_audio.py FILE.wav --view waveform --from 12.0 --to 12.05
  python3 tools/inspect_audio.py A.wav B.wav --png out/         # compare two renders

READING THE ASCII SPECTROGRAM. Rows are log-spaced frequency bands, low at the BOTTOM.
Columns are time. Characters are level relative to the loudest cell in the plot:

    #  0 to -6 dB     +  -6 to -18      -  -18 to -30
    .  -30 to -45     (space) below -45

Harmonics read as stacked horizontal lines. A discontinuity reads as a full-height vertical
column. Vibrato reads as those lines wobbling. If the bottom rows are empty on a downward
shift, the fundamental is not there and something is wrong.
"""

import os
import sys
import wave

import numpy as np


# --- io ---------------------------------------------------------------------

def read_wav(path):
    with wave.open(path, "rb") as w:
        n, sr, ch = w.getnframes(), w.getframerate(), w.getnchannels()
        x = np.frombuffer(w.readframes(n), dtype=np.int16).astype(np.float64) / 32768.0
    if ch == 2:
        x = x.reshape(-1, 2).mean(axis=1)
    return x, sr


def slice_time(x, sr, t0, t1):
    a = 0 if t0 is None else max(0, int(t0 * sr))
    b = len(x) if t1 is None else min(len(x), int(t1 * sr))
    return x[a:b], a / sr


# --- analysis ---------------------------------------------------------------

def stft(x, sr, win=2048, hop=512):
    """Magnitude STFT. Hann window, no padding — a frame that would run off the end is
    dropped rather than zero-padded, because a half-empty frame reads as a level drop and
    invites exactly the kind of edge artefact hunt that wastes an afternoon."""
    if len(x) < win:
        return np.zeros((win // 2 + 1, 0)), np.array([]), np.fft.rfftfreq(win, 1 / sr)
    w = np.hanning(win)
    frames = 1 + (len(x) - win) // hop
    S = np.empty((win // 2 + 1, frames))
    for i in range(frames):
        S[:, i] = np.abs(np.fft.rfft(x[i * hop:i * hop + win] * w))
    t = (np.arange(frames) * hop + win / 2) / sr
    f = np.fft.rfftfreq(win, 1 / sr)
    return S, t, f


def log_bands(S, f, fmin=50.0, fmax=8000.0, rows=28):
    """Collapse linear FFT bins into log-spaced bands.

    WHY LOG: pitch is logarithmic, so an octave shift should look like a constant vertical
    translation. On a linear axis the low fundamental that matters most here occupies about
    one pixel and the top octave — which carries almost nothing on a downshifted voice —
    occupies half the plot.
    """
    edges = np.geomspace(fmin, fmax, rows + 1)
    out = np.zeros((rows, S.shape[1]))
    for r in range(rows):
        m = (f >= edges[r]) & (f < edges[r + 1])
        if m.any():
            out[r] = S[m].mean(axis=0)
        elif S.shape[1]:
            # Band narrower than the FFT resolution: take the nearest single bin rather
            # than leaving a false empty row that reads as missing energy.
            out[r] = S[int(np.argmin(np.abs(f - edges[r])))]
    return out, edges


def f0_track(x, sr, lo=40.0, hi=2000.0, win=4096, hop=1024):
    """YIN-style F0 per frame. UNCONSTRAINED — no search band around an expected value.

    This is deliberate and load-bearing. A tracker given a band around the EXPECTED output
    F0 will happily lock onto a subharmonic of whatever is actually there and report
    success; that is precisely how VH-001 passed a measurement sweep. If you narrow this
    search to 'help' it, you have removed the only reason the tool is trustworthy.
    """
    ts, fs, conf = [], [], []
    half = win // 2
    maxlag = min(int(sr / lo), half)
    minlag = max(2, int(sr / hi))
    for i in range(0, len(x) - win, hop):
        s = x[i:i + win]
        rms = np.sqrt(np.mean(s * s))
        ts.append((i + half) / sr)
        if rms < 1e-4:
            fs.append(np.nan); conf.append(0.0); continue
        s = s - s.mean()
        a, b = s[:half], s
        d = np.array([np.sum((a - b[t:t + half]) ** 2) for t in range(maxlag)])
        cum = np.cumsum(d[1:]) / np.arange(1, maxlag)
        dn = np.ones(maxlag); dn[1:] = d[1:] / (cum + 1e-20)
        tau = -1
        for t in range(minlag, maxlag):
            if dn[t] < 0.2:
                while t + 1 < maxlag and dn[t + 1] < dn[t]:
                    t += 1
                tau = t; break
        if tau < 0:
            tau = minlag + int(np.argmin(dn[minlag:]))
        fs.append(sr / tau)
        conf.append(float(1.0 - dn[tau]))
    return np.array(ts), np.array(fs), np.array(conf)


def duty_cycle(x, sr, floor_db=-40.0, win=4096, hop=2048):
    """Fraction of each output period that actually carries energy.

    THIS IS THE SCALAR FORM OF "the grain does not fill the period". A continuous voiced
    tone has energy everywhere and scores near 1.0. A pulse train — one short grain then
    silence until the next — scores near grain_length / period, and is heard as a buzz or
    a square-wave quality rather than as a lower voice.

    Method: per frame, find the period by autocorrelation, then measure the fraction of
    samples whose local envelope exceeds `floor_db` below the frame peak. Envelope rather
    than raw samples, because a sine crosses zero constantly and would score terribly for
    reasons that have nothing to do with gaps.

    CAVEAT: this measures ONE cause of a low score. Quiet passages and unvoiced frames also
    score low. Read it next to the waveform view, never alone.
    """
    out_t, out_d, out_p = [], [], []
    for i in range(0, len(x) - win, hop):
        s = x[i:i + win]
        peak = np.abs(s).max()
        if peak < 1e-4:
            continue
        env = np.abs(np.convolve(np.abs(s), np.ones(31) / 31, mode="same"))
        thresh = env.max() * (10 ** (floor_db / 20.0))
        ac = np.correlate(s - s.mean(), s - s.mean(), "full")[len(s) - 1:]
        lo, hi = int(sr / 1000), min(int(sr / 40), len(ac) - 1)
        period = (lo + int(np.argmax(ac[lo:hi]))) if hi > lo else np.nan
        out_t.append((i + win / 2) / sr)
        out_d.append(float(np.mean(env > thresh)))
        out_p.append(sr / period if period == period else np.nan)
    return np.array(out_t), np.array(out_d), np.array(out_p)


# --- text views -------------------------------------------------------------

_RAMP = [(-6, "#"), (-18, "+"), (-30, "-"), (-45, "."), (-1e9, " ")]


def print_spectrogram(x, sr, t_off, cols=100, rows=24, fmin=50.0, fmax=8000.0):
    S, t, f = stft(x, sr)
    if S.shape[1] == 0:
        print("  (too short for an STFT frame)")
        return
    B, edges = log_bands(S, f, fmin, fmax, rows)
    # Resample time axis to the terminal width by taking the MAX per column, not the mean:
    # a click is one frame wide and averaging would erase the thing most worth seeing.
    idx = np.linspace(0, B.shape[1], cols + 1).astype(int)
    C = np.array([B[:, a:max(b, a + 1)].max(axis=1) for a, b in zip(idx[:-1], idx[1:])]).T
    db = 20 * np.log10(C + 1e-12)
    db -= db.max()
    print(f"  {'Hz':>6s} |")
    for r in range(rows - 1, -1, -1):
        line = "".join(next(ch for lim, ch in _RAMP if db[r, c] > lim) for c in range(cols))
        print(f"  {edges[r]:6.0f} |{line}|")
    print(f"  {'':6s} +{'-' * cols}+")
    print(f"  {'':6s}  {t_off:<{max(1, cols - 8)}.2f}s{t_off + len(x) / sr:>7.2f}s")


def print_f0(x, sr, t_off):
    t, f, c = f0_track(x, sr)
    ok = c > 0.5
    if not ok.any():
        print("  no confident F0 anywhere in this span")
        return
    print(f"  frames {len(t)}, confident {ok.sum()} ({100*ok.mean():.0f}%)")
    print(f"  F0 median {np.nanmedian(f[ok]):8.1f} Hz   "
          f"10-90 pct {np.nanpercentile(f[ok], 10):.1f} - {np.nanpercentile(f[ok], 90):.1f} Hz")
    step = max(1, len(t) // 20)
    print(f"  {'t (s)':>8s} {'F0 Hz':>8s} {'conf':>6s}")
    for i in range(0, len(t), step):
        v = f"{f[i]:8.1f}" if f[i] == f[i] else "     ---"
        print(f"  {t_off + t[i]:8.2f} {v} {c[i]:6.2f}")


def print_duty(x, sr, t_off):
    t, d, p = duty_cycle(x, sr)
    if not len(t):
        print("  (all frames below the silence floor)")
        return
    print(f"  duty cycle: median {np.median(d):.2f}, 10-90 pct "
          f"{np.percentile(d, 10):.2f} - {np.percentile(d, 90):.2f}")
    print("  1.00 = energy fills every period; 0.30 = a short pulse then silence.")
    step = max(1, len(t) // 16)
    print(f"  {'t (s)':>8s} {'duty':>6s} {'period Hz':>10s}  {'bar':<22s}")
    for i in range(0, len(t), step):
        bar = "#" * int(round(d[i] * 20))
        pv = f"{p[i]:10.1f}" if p[i] == p[i] else "       ---"
        print(f"  {t_off + t[i]:8.2f} {d[i]:6.2f} {pv}  {bar:<22s}")


def print_waveform(x, sr, t_off, cols=100, rows=17):
    """ASCII oscilloscope. Only readable over a short span — a few periods."""
    if len(x) == 0:
        print("  (empty span)")
        return
    idx = np.linspace(0, len(x), cols + 1).astype(int)
    lo = np.array([x[a:max(b, a + 1)].min() for a, b in zip(idx[:-1], idx[1:])])
    hi = np.array([x[a:max(b, a + 1)].max() for a, b in zip(idx[:-1], idx[1:])])
    peak = max(np.abs(x).max(), 1e-9)
    mid = rows // 2
    grid = [[" "] * cols for _ in range(rows)]
    for c in range(cols):
        a = mid - int(round(hi[c] / peak * mid))
        b = mid - int(round(lo[c] / peak * mid))
        for r in range(max(0, min(a, b)), min(rows, max(a, b) + 1)):
            grid[r][c] = "#"
    for r, row in enumerate(grid):
        tick = f"{peak * (mid - r) / mid:+6.3f}" if r in (0, mid, rows - 1) else " " * 6
        print(f"  {tick} |{''.join(row)}|")
    print(f"  {'':6s} +{'-' * cols}+")
    print(f"  {'':6s}  {t_off:<{max(1, cols - 8)}.4f}s{t_off + len(x) / sr:>7.4f}s  "
          f"(peak {peak:.3f})")


# --- png ---------------------------------------------------------------------

def write_pngs(paths_data, outdir, tag):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("\n  matplotlib not installed; skipping --png. Text views above are complete.")
        return []

    os.makedirs(outdir, exist_ok=True)
    written = []
    n = len(paths_data)
    fig, axes = plt.subplots(3, n, figsize=(9 * n, 11), squeeze=False)

    for col, (name, x, sr, t_off) in enumerate(paths_data):
        S, t, f = stft(x, sr)
        if S.shape[1] == 0:
            continue
        db = 20 * np.log10(S + 1e-12)
        db -= db.max()
        ax = axes[0][col]
        ax.pcolormesh(t + t_off, f, db, vmin=-80, vmax=0, shading="auto", cmap="magma")
        ax.set_yscale("log"); ax.set_ylim(50, 8000)
        ax.set_title(f"{name}\nspectrogram (log f, dB rel. peak)")
        ax.set_ylabel("Hz")

        tt, ff, cc = f0_track(x, sr)
        ax = axes[1][col]
        good = cc > 0.5
        ax.plot(tt[good] + t_off, ff[good], ".", ms=3)
        ax.set_yscale("log"); ax.set_ylabel("F0 Hz")
        ax.set_title("F0 track (unconstrained, confident frames only)")
        ax.grid(alpha=0.3)

        # A few periods of waveform from the middle: the view that shows gap SHAPE.
        mid = len(x) // 2
        span = min(len(x) - mid, int(0.03 * sr))
        seg = x[mid:mid + span]
        ax = axes[2][col]
        ax.plot(np.arange(len(seg)) / sr + mid / sr + t_off, seg, lw=0.7)
        ax.set_title("waveform, 30 ms from centre")
        ax.set_xlabel("s")
        ax.grid(alpha=0.3)

    fig.tight_layout()
    p = os.path.join(outdir, f"{tag}.png")
    fig.savefig(p, dpi=110)
    plt.close(fig)
    written.append(p)
    return written


# --- main --------------------------------------------------------------------

def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1

    files, png_dir, t0, t1, views = [], None, None, None, None
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--png":
            png_dir = args[i + 1]; i += 2
        elif a == "--from":
            t0 = float(args[i + 1]); i += 2
        elif a == "--to":
            t1 = float(args[i + 1]); i += 2
        elif a == "--view":
            views = args[i + 1].split(","); i += 2
        else:
            files.append(a); i += 1

    views = views or ["spectrogram", "f0", "duty", "waveform"]
    loaded = []
    for p in files:
        x, sr = read_wav(p)
        seg, off = slice_time(x, sr, t0, t1)
        loaded.append((os.path.basename(p), seg, sr, off))

    for name, seg, sr, off in loaded:
        print(f"\n{'=' * 104}")
        print(f"{name}   {off:.3f} - {off + len(seg)/sr:.3f} s   "
              f"{sr} Hz   peak {np.abs(seg).max() if len(seg) else 0:.3f}")
        print("=" * 104)
        if "spectrogram" in views:
            print("\n[spectrogram]"); print_spectrogram(seg, sr, off)
        if "f0" in views:
            print("\n[F0 track]"); print_f0(seg, sr, off)
        if "duty" in views:
            print("\n[duty cycle]"); print_duty(seg, sr, off)
        if "waveform" in views:
            # Default zoom is 25 ms from the centre; a full segment is unreadable as ASCII.
            mid = len(seg) // 2
            span = min(len(seg) - mid, int(0.025 * sr))
            print("\n[waveform, 25 ms from centre]")
            print_waveform(seg[mid:mid + span], sr, off + mid / sr)

    if png_dir:
        tag = "_vs_".join(os.path.splitext(n)[0] for n, _, _, _ in loaded)[:80]
        for p in write_pngs(loaded, png_dir, tag):
            print(f"\nwrote {p}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
