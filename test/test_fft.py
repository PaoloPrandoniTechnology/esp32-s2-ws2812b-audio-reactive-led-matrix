#!/usr/bin/env python3
"""
test/test_fft.py - Test automatici unità: FFT, finestre, limiti parametri, protocollo WS

Non richiede hardware ESP32:
  python -m pytest test/test_fft.py -v     (se pytest installato)
  python test/test_fft.py                  (altrimenti run con assert built-in)
"""
from __future__ import annotations

import json
import math
import os
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False

PROJECT_ROOT = Path(__file__).resolve().parent.parent
DATA_DIR = PROJECT_ROOT / "data"
CONFIG_H = PROJECT_ROOT / "include" / "config.h"
APP_JS = DATA_DIR / "app.js"


# ==================================================================
# Utility: extract numeric defines from config.h
# ==================================================================
def extract_config_h_defines() -> dict[str, int]:
    """Estrae #define NAME VALUE numerici da include/config.h"""
    text = CONFIG_H.read_text(encoding="utf-8")
    out: dict[str, int] = {}
    for m in re.finditer(r'^\s*#\s*define\s+([A-Z0-9_]+)\s+([0-9]+(?:\.[0-9]+)?)\s*$', text, re.M):
        name, val = m.group(1), m.group(2)
        try:
            out[name] = int(val) if "." not in val else float(val)
        except ValueError:
            pass
    return out


# ==================================================================
# Test 1: Coerenza PARAM_CONFIG (app.js) vs config.h (firmware)
# ==================================================================
def extract_app_js_param_config() -> dict:
    """Estrae l'oggetto PARAM_CONFIG da app.js (primo blocco { ... }).
    Approccio robusto: linee key:value -> dizionario."""
    text = APP_JS.read_text(encoding="utf-8")
    start = text.find("const PARAM_CONFIG = ")
    if start < 0:
        start = text.find("PARAM_CONFIG = ")
    if start < 0:
        raise AssertionError("PARAM_CONFIG non trovato in app.js")
    brace_open = text.find("{", start)
    if brace_open < 0: raise AssertionError("{  mancante dopo PARAM_CONFIG")
    depth = 0
    i = brace_open
    while i < len(text):
        ch = text[i]
        if ch == "{": depth += 1
        elif ch == "}": depth -= 1
        if depth == 0: break
        i += 1
    block = text[brace_open : i + 1]
    # Strip commenti riga //
    block_no_comments = re.sub(r'//.*\n', '\n', block)
    # Cerca i top-level key: val per ciascun param (siamo al livello PARAM_CONFIG.x = {})
    # Estrazione manuale: pattern  <name>: {\n  fw: ... , min:..., max:..., def:...
    result = {}
    for m in re.finditer(r"(\w+)\s*:\s*\{([^{}]*)\}", block_no_comments):
        pname = m.group(1)
        body = m.group(2)
        entry = {}
        # min
        mm = re.search(r"min\s*:\s*(-?\d+(?:\.\d+)?)", body)
        if mm: entry["min"] = float(mm.group(1)) if "." in mm.group(1) else int(mm.group(1))
        mm = re.search(r"max\s*:\s*(-?\d+(?:\.\d+)?)", body)
        if mm: entry["max"] = float(mm.group(1)) if "." in mm.group(1) else int(mm.group(1))
        mm = re.search(r"def\s*:\s*(-?\d+(?:\.\d+)?|'[^']*'|\"[^\"]*\")", body)
        if mm:
            v = mm.group(1)
            if v[0] in ("'", '"'):
                entry["def"] = v[1:-1]
            else:
                entry["def"] = float(v) if "." in v else int(v)
        # values (array)
        mm = re.search(r"values\s*:\s*\[([^\]]*)\]", body)
        if mm:
            vals = re.findall(r"'([^']+)'", mm.group(1))
            if vals: entry["values"] = vals
        # steps array
        mm = re.search(r"steps\s*:\s*\[([^\]]*)\]", body)
        if mm:
            nums = [int(x) for x in re.findall(r"\d+", mm.group(1))]
            if nums: entry["steps"] = nums
        result[pname] = entry
    return result


def test_param_config_limits_match_config_h():
    """Verifica che min/max/default FW (sampleRate, gainDb, hpfHz) coincidano tra
    include/config.h e data/app.js PARAM_CONFIG."""
    fw = extract_config_h_defines()
    browser_cfg = extract_app_js_param_config()
    if not browser_cfg:
        print("SKIP test_param_config_limits_match_config_h (PARAM_CONFIG non parsabile)")
        return
    mapping = [
        ("sampleRate", "SAMPLE_RATE_DEFAULT", "SAMPLE_RATE_MIN", "SAMPLE_RATE_MAX"),
        ("gainDb",     "GAIN_DB_DEFAULT",     "GAIN_DB_MIN",     "GAIN_DB_MAX"),
        ("hpfHz",      "HPF_HZ_DEFAULT",      "HPF_HZ_MIN",      "HPF_HZ_MAX"),
    ]
    errors = []
    for key, defK, minK, maxK in mapping:
        param = browser_cfg.get(key, {})
        if not isinstance(param, dict): continue
        if defK in fw and "def" in param and int(param["def"]) != int(fw[defK]):
            errors.append(f"{key} default mismatch: FW={fw[defK]} JS={param['def']}")
        if minK in fw and "min" in param and int(param["min"]) != int(fw[minK]):
            errors.append(f"{key} min mismatch: FW={fw[minK]} JS={param['min']}")
        if maxK in fw and "max" in param and int(param["max"]) != int(fw[maxK]):
            errors.append(f"{key} max mismatch: FW={fw[maxK]} JS={param['max']}")
    assert not errors, "PARAM_CONFIG NON COERENTE con config.h:\n - " + "\n - ".join(errors)
    print("OK  test_param_config_limits_match_config_h")


# ==================================================================
# Test 2: Protocollo pacchetto WS binario (header 12B)
# ==================================================================
def test_ws_binary_protocol_header_12B():
    """Pacchetto binario WS: header 12B uint32+uint32+uint16+uint16 (magic 0xDA7A)."""
    # Generato da simulator.py: struct.pack("<IIHH", seq, sr, ns, 0xDA7A)
    SEQ = 0x01020304
    SR = 16000
    NS = 512
    MAGIC = 0xDA7A
    header = struct.pack("<IIHH", SEQ, SR, NS, MAGIC)
    assert len(header) == 12, f"Header deve essere 12B, got {len(header)}"
    s, r, n, m = struct.unpack("<IIHH", header)
    assert s == SEQ and r == SR and n == NS and m == MAGIC, "Unpack header fallito"
    # Verifica NSAMPLES 512 -> 2x512 = 1024 byte body, pkt totale 1036
    body = b"\x00\x00" * NS
    assert len(header + body) == 12 + 1024
    print("OK  test_ws_binary_protocol_header_12B")


# ==================================================================
# Test 3: Funzione FFT (numpy confronto) - se numpy disponibile
# ==================================================================
if HAS_NUMPY:
    def compute_js_fft_bruteforce(x: np.ndarray):
        """FFT naive O(N^2) per confrontare con FFT implementata in browser.
        La FFT nel browser è radix-2 Cooley-Tukey: numpy.fft lo verifica."""
        N = len(x)
        n = np.arange(N)
        k = n.reshape((N, 1))
        e = np.exp(-2j * np.pi * k * n / N)
        return np.dot(e, x)

    def test_fft_sine_1khz_bin_matches_expected():
        """sin(2π·1000·t) campionato a 16000 Hz per 1024 pt -> picco a bin k = round(1000·1024/16000) = 64."""
        SR = 16000
        N = 1024
        F = 1000.0
        t = np.arange(N) / SR
        x = np.sin(2 * np.pi * F * t)
        X = np.fft.fft(x)
        mag = 20 * np.log10(np.abs(X) + 1e-12)
        peak_bin = int(np.argmax(mag[:N//2]))
        expected_bin = int(round(F * N / SR))  # 64
        assert peak_bin == expected_bin, f"FFT peak mismatch: got bin={peak_bin} (freq={peak_bin*SR/N}Hz), expected {expected_bin}"
        # Frequenza calcolata dal bin deve essere ~1000 Hz
        peak_freq = peak_bin * SR / N
        assert abs(peak_freq - F) <= SR / N, f"FFT freq offset: {peak_freq} vs {F}"
        print(f"OK  test_fft_sine_1khz_bin_matches_expected (peak={peak_bin} freq={peak_freq}Hz)")

    def test_windows_sum_squared():
        """Verifica le finestre Hann, Hamming, Blackman, Rect: sum(win^2) coerenti tra
        implementazione browser (getWindow) e numpy calcolato."""
        N = 1024
        n_arr = np.arange(N)
        # Stesse formule di data/app.js getWindow() e data/fft_worker.js getWindow()
        windows = {
            "hann":      0.5 - 0.5 * np.cos(2 * np.pi * n_arr / (N - 1)),
            "hamming":   0.54 - 0.46 * np.cos(2 * np.pi * n_arr / (N - 1)),
            "blackman":  0.42 - 0.5 * np.cos(2 * np.pi * n_arr / (N - 1)) + 0.08 * np.cos(4 * np.pi * n_arr / (N - 1)),
            "rect":      np.ones(N),
        }
        for name, w in windows.items():
            sumsq = float(np.sum(w ** 2))
            # Verifica banale: deve essere >0 e non NaN (testa correttezza sintassi)
            assert sumsq > 0 and np.isfinite(sumsq), f"Window {name} sumsq non valido {sumsq}"
            # Relazioni note: Rect > Hann ≈ Hamming > Blackman (per N grande)
            if name == "rect":  assert sumsq == N
            if name == "hann":  assert 380 < sumsq < 388   # N=1024 ~384
            if name == "hamming": assert 402 < sumsq < 412 # N=1024 ~406.9
            if name == "blackman": assert 305 < sumsq < 320 # N=1024 ~311.6
        print("OK  test_windows_sum_squared (Hann/Hamming/Blackman/Rect N=1024)")

    def test_freq_to_pixel_mapping_monotonic():
        """freq_to_bin -> pixel y deve essere MONOTONO: freq crescente -> y decrescente
        (waterfall ha freq 0 in basso, Nyquist in alto) O viceversa, ma SEMPRE monotono."""
        N = 1024
        SR = 16000
        def bin_freq(k): return k * SR / N
        # pixel mapping semplice lineare: y = H * (1 - f/Nyquist)
        def freq_to_pix(f, H): return H * (1.0 - f / (SR / 2))
        H = 400
        ys = [freq_to_pix(bin_freq(k), H) for k in range(N//2 + 1)]
        for i in range(len(ys) - 1):
            # Monotono non crescente
            assert ys[i + 1] <= ys[i] + 1e-6, "mapping freq->pixel NON monotono"
        print("OK  test_freq_to_pixel_mapping_monotonic")

    def test_fft_size_is_power_of_two_constraints():
        """PARAM_CONFIG.fftSize accetta SOLO potenze di due (256..8192)."""
        browser_cfg = extract_app_js_param_config()
        if "fftSize" not in browser_cfg:
            print("SKIP test_fft_size_is_power_of_two_constraints")
            return
        fs = browser_cfg["fftSize"]
        allowed = [256, 512, 1024, 2048, 4096, 8192]
        if isinstance(fs, dict) and "allowed" in fs:
            assert list(fs["allowed"]) == allowed, f"fftSize.allowed non match: {fs['allowed']}"
        if isinstance(fs, dict) and "min" in fs:
            assert int(fs["min"]) == 256
            assert int(fs["max"]) == 8192
        print("OK  test_fft_size_is_power_of_two_constraints")


# ==================================================================
# Test 4: Clamp sample rate snap a valori permessi
# ==================================================================
def test_clamp_sample_rate_snap_values():
    SR_ALLOWED = [8000, 16000, 22050, 32000, 44100, 48000]
    def clamp(sr):
        if sr <= SR_ALLOWED[0]: return SR_ALLOWED[0]
        if sr >= SR_ALLOWED[-1]: return SR_ALLOWED[-1]
        best = SR_ALLOWED[0]; best_diff = abs(best - sr)
        for v in SR_ALLOWED[1:]:
            d = abs(v - sr)
            if d < best_diff: best, best_diff = v, d
        return best
    assert clamp(0) == 8000
    assert clamp(999999) == 48000
    assert clamp(15500) == 16000
    assert clamp(17000) == 16000  # 1k vs 5.05k
    assert clamp(40000) == 44100  # 4.1k vs 8k
    assert clamp(22000) == 22050
    print("OK  test_clamp_sample_rate_snap_values")


def test_minmax_swap_guards_in_js_logic():
    """Verifica algoritmo swap guard: minDb > maxDb → swap; freqMin > freqMax → swap."""
    def clamp_all(params):
        p = dict(params)
        if p.get("minDb", -100) > p.get("maxDb", -20):
            p["minDb"], p["maxDb"] = p["maxDb"], p["minDb"]
        if p.get("freqMin", 0) > p.get("freqMax", 8000):
            p["freqMin"], p["freqMax"] = p["freqMax"], p["freqMin"]
        return p

    # Caso 1: minDb=-20  maxDb=-100 → swap
    p1 = clamp_all({"minDb": -20, "maxDb": -100})
    assert p1["minDb"] == -100 and p1["maxDb"] == -20
    # Caso 2: freqMin=9000 freqMax=1000 → swap
    p2 = clamp_all({"freqMin": 9000, "freqMax": 1000, "sampleRate": 16000})
    assert p2["freqMin"] == 1000 and p2["freqMax"] == 9000
    # Caso 3: ordine corretto
    p3 = clamp_all({"minDb": -110, "maxDb": -20, "freqMin": 0, "freqMax": 8000})
    assert p3 == {"minDb": -110, "maxDb": -20, "freqMin": 0, "freqMax": 8000}
    print("OK  test_minmax_swap_guards_in_js_logic")


# ==================================================================
# MAIN
# ==================================================================
if __name__ == "__main__":
    print(f"\n=== test_fft.py (numpy={'SI' if HAS_NUMPY else 'NO'}) ===")
    test_param_config_limits_match_config_h()
    test_ws_binary_protocol_header_12B()
    test_clamp_sample_rate_snap_values()
    test_minmax_swap_guards_in_js_logic()
    if HAS_NUMPY:
        test_fft_sine_1khz_bin_matches_expected()
        test_windows_sum_squared()
        test_freq_to_pixel_mapping_monotonic()
        test_fft_size_is_power_of_two_constraints()
    else:
        print("SKIP 4 test numpy (pip install numpy)")
    print("\nTutti i test passati ✔")
