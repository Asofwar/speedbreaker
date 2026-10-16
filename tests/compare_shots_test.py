#!/usr/bin/env python3
# scripts/compare_shots.py on made-up timedemo shots: two run folders of
# shot-<step>.ppm pictures (and summary.json "shots" lists, as
# debug/bench.cpp writes them) in a scratch folder, for each case its exit
# code, its --json results and the words its report must hold: identical
# pictures pass, one pixel off fails by default and passes within a
# tolerance or a threshold, the differences' place is found, and pictures
# that can't be compared (missing, another size, another swap, not a PPM)
# stop it; --shown compares the pictures as shown (shot-<step>-shown.ppm)
# where the front images can't be (another internal resolution).
#   python3 tests/compare_shots_test.py
import json
import math
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "scripts", "compare_shots.py")
W, H = 64, 32

failures = 0


def fail(message):
    global failures
    failures += 1
    print("  FAIL", message)


def expect(condition, message):
    if not condition:
        fail(message)


def picture(seed=1, width=W, height=H):
    """A deterministic RGB picture with some structure (gradients)."""
    out = bytearray(width * height * 3)
    for y in range(height):
        for x in range(width):
            i = (y * width + x) * 3
            out[i] = (x * 4 + seed) & 255
            out[i + 1] = (y * 8 + seed * 3) & 255
            out[i + 2] = (x * y + seed * 7) & 255
    return out


def write_ppm(path, pixels, width=W, height=H, header=None):
    with open(path, "wb") as f:
        f.write(header if header is not None else b"P6\n# made by compare_shots_test\n%d %d\n255\n" % (width, height))
        f.write(bytes(pixels))


def make_run(root, name, shots, swaps=None, statuses=None):
    """A run folder with shot-<step>.ppm for {step: pixels} and, with
    `swaps`, a summary.json naming each shot's swap."""
    folder = os.path.join(root, name)
    os.makedirs(folder)
    for step, pixels in shots.items():
        write_ppm(os.path.join(folder, f"shot-{step}.ppm"), pixels)
    if swaps is not None:
        taken = [{"step": step, "status": (statuses or {}).get(step, "ok"), "loop": 4000 + step, "frame_step": step,
                  "swap": swaps[step], "file": f"shot-{step}.ppm", "width": W, "height": H} for step in sorted(shots)]
        with open(os.path.join(folder, "summary.json"), "w", encoding="utf-8") as f:
            json.dump({"status": "ok", "shots": {"verified": True, "taken": taken}}, f)
    return folder


def run(a, b, *options, json_path=None):
    args = [sys.executable, SCRIPT, a, b, *options]
    if json_path:
        args += ["--json", json_path]
    p = subprocess.run(args, capture_output=True, text=True)
    result = None
    if json_path and os.path.exists(json_path):
        with open(json_path, encoding="utf-8") as f:
            result = json.load(f)
    return p.returncode, p.stdout + p.stderr, result


def case(name, body):
    print(name)
    with tempfile.TemporaryDirectory(prefix="compare_shots_test.") as root:
        try:
            body(root)
        except Exception as e:  # a crash is a failure, with the case's name
            fail(f"{name}: {type(e).__name__}: {e}")


def identical(root):
    a = make_run(root, "A", {1600: picture(), 2200: picture(2)}, swaps={1600: 4490, 2200: 5090})
    b = make_run(root, "B", {1600: picture(), 2200: picture(2)}, swaps={1600: 4490, 2200: 5090})
    code, text, result = run(a, b, json_path=os.path.join(root, "r.json"))
    expect(code == 0, f"exit {code}, wanted 0:\n{text}")
    expect("shot 1600: identical (64x32)" in text and "shot 2200: identical" in text, f"no identical lines:\n{text}")
    expect(result and result["verdict"] == "identical", f"verdict {result and result['verdict']}")
    expect(result and result["shots"][0]["psnr"] is None and result["shots"][0]["max_diff"] == 0, "identical: PSNR null, max 0")
    # summary.json itself names the run too.
    code, text, _ = run(os.path.join(a, "summary.json"), os.path.join(b, "summary.json"))
    expect(code == 0, f"summary.json paths: exit {code}")


def one_pixel(root):
    pa, pb = picture(), picture()
    i = (20 * W + 50) * 3  # x 50, y 20
    pb[i + 1] = (pb[i + 1] + 10) & 255
    a = make_run(root, "A", {1600: pa})
    b = make_run(root, "B", {1600: pb})
    # The defaults: off by 10 is within the threshold (64), PSNR 66 dB.
    code, text, result = run(a, b, json_path=os.path.join(root, "r0.json"))
    expect(code == 0 and result and result["verdict"] == "within_tolerance" and result["preset"] == "default",
           f"defaults: exit {code}:\n{text}")
    expect("1 pixels changed" in text and "0 off by more than 64" in text, f"defaults' report:\n{text}")
    # --strict (capped runs): off by 10 is beyond its threshold (8), but 1 pixel
    # of 2048 is 0.05%, beyond its 0.01%.
    code, text, result = run(a, b, "--strict", json_path=os.path.join(root, "r1.json"))
    expect(code == 1 and result and result["preset"] == "strict" and result["shots"][0]["differing_pixels"] == 1,
           f"--strict: exit {code}:\n{text}")
    # A value after a preset wins.
    code, text, _ = run(a, b, "--strict", "--threshold", "10")
    expect(code == 0, f"--strict --threshold 10: exit {code}:\n{text}")
    # --exact: identical only.
    code, text, result = run(a, b, "--exact", json_path=os.path.join(root, "r.json"))
    expect(code == 1, f"--exact: exit {code}, wanted 1:\n{text}")
    expect("max difference 10" in text and "1 off by more than 0" in text and "BEYOND TOLERANCE" in text, f"report:\n{text}")
    expect("within x 50..50 y 20..20" in text, f"where:\n{text}")
    shot = (result or {"shots": [{}]})["shots"][0]
    expect(shot.get("differing_pixels") == 1 and shot.get("changed_pixels") == 1 and shot.get("bbox") == [50, 20, 50, 20],
           f"json: {shot}")
    # PSNR of one value off by 10 in 64x32x3: 10 log10(255^2 / (100 / 6144)).
    psnr = 10 * math.log10(255.0 ** 2 / (100.0 / (W * H * 3)))
    expect(shot.get("psnr") and abs(shot["psnr"] - psnr) < 1e-6, f"PSNR {shot.get('psnr')}, wanted {psnr}")
    # Its tile of the 8x8 grid: 8x4 pixels each, x 48..55, y 20..23.
    expect(shot.get("tiles") and shot["tiles"][0] == {"x": 48, "y": 20, "width": 8, "height": 4, "pixels": 1},
           f"tile {shot.get('tiles')}")
    # Within a tolerance of 1% of the pixels (20 of 2048), any difference counted: passes.
    code, text, result = run(a, b, "--threshold", "0", "--tolerance", "1", json_path=os.path.join(root, "r2.json"))
    expect(code == 0 and result and result["verdict"] == "within_tolerance", f"--tolerance 1: exit {code}:\n{text}")
    # A threshold of 10: a difference of 10 isn't one.
    code, text, result = run(a, b, "--threshold", "10", "--tolerance", "0", json_path=os.path.join(root, "r3.json"))
    expect(code == 0 and result and result["shots"][0]["differing_pixels"] == 0, f"--threshold 10: exit {code}:\n{text}")
    # But --min-psnr above its PSNR fails it whatever the tolerance.
    code, text, _ = run(a, b, "--tolerance", "100", "--min-psnr", "70")
    expect(code == 1, f"--min-psnr 70: exit {code}, wanted 1")
    # The diff picture: magenta beyond the threshold, green within it, run A
    # darkened elsewhere.
    for options, colour in ((["--exact"], b"\xff\x00\xff"), ([], b"\x00\xff\x00")):
        diff_dir = os.path.join(root, "diff" + str(len(options)))
        run(a, b, "--diff", diff_dir, *options)
        path = os.path.join(diff_dir, "diff-1600.ppm")
        expect(os.path.exists(path), f"no diff-1600.ppm with {options}")
        if os.path.exists(path):
            with open(path, "rb") as f:
                data = f.read()
            header = b"P6\n%d %d\n255\n" % (W, H)
            px = data[len(header):]
            expect(data.startswith(header) and len(px) == W * H * 3, "diff picture's size")
            expect(px[i:i + 3] == colour, f"{options}: changed pixel {px[i:i + 3]!r}, wanted {colour!r}")
            expect(px[0:3] == bytes(v >> 2 for v in pa[0:3]), f"background {px[0:3]!r} vs {pa[0:3]!r}")


def block(root):
    # A 10x6 block in the bottom-right corner, its red inverted (off by 179
    # to 251): a broken draw.
    pa, pb = picture(), picture()
    for y in range(26, 32):
        for x in range(54, 64):
            i = (y * W + x) * 3
            pb[i] = 255 - pa[i]
    a = make_run(root, "A", {3400: pa})
    b = make_run(root, "B", {3400: pb})
    code, text, result = run(a, b, json_path=os.path.join(root, "r.json"))
    shot = (result or {"shots": [{}]})["shots"][0]
    expect(code == 1, f"defaults: 60 pixels (2.9%) beyond 0.05%: exit {code}")
    expect(shot.get("differing_pixels") == 60 and shot.get("bbox") == [54, 26, 63, 31], f"json: {shot}")
    tiles = shot.get("tiles") or []
    expect(tiles and all(t["y"] >= 24 and t["x"] >= 48 for t in tiles), f"tiles {tiles}")
    code, _, _ = run(a, b, "--tolerance", "2", "--min-psnr", "0")
    expect(code == 1, f"beyond 2%: exit {code}")
    code, _, _ = run(a, b, "--tolerance", "3", "--min-psnr", "0")
    expect(code == 0, f"within 3%: exit {code}")
    # Within 3% of the pixels, but its PSNR is under the default 30 dB.
    code, text, _ = run(a, b, "--tolerance", "3")
    expect(code == 1, f"within 3% but PSNR under 30 dB: exit {code}:\n{text}")


def noise(root):
    # Uncapped runs' noise as the Deck's showed: most of the picture off by 1
    # to 4, a patch by up to 45: passes the defaults, not --strict or --exact.
    pa, pb = picture(), picture()
    for k in range(0, len(pb), 3):
        pb[k] = min(255, pb[k] + 1 + (k // 3) % 4)
    for y in range(4, 8):
        for x in range(8, 16):
            i = (y * W + x) * 3
            pb[i + 2] = min(255, pa[i + 2] + 45)
    a = make_run(root, "A", {4600: pa})
    b = make_run(root, "B", {4600: pb})
    code, text, _ = run(a, b)
    expect(code == 0 and "within_tolerance" in text, f"uncapped noise, defaults: exit {code}:\n{text}")
    code, text, _ = run(a, b, "--strict")
    expect(code == 1, f"uncapped noise, --strict: exit {code}")
    code, text, _ = run(a, b, "--exact")
    expect(code == 1, f"uncapped noise, --exact: exit {code}")
    # Capped runs' noise: two fifths of the picture off by 1 or 2: passes --strict.
    pc = picture()
    for k in range(0, len(pc), 15):
        pc[k] = min(255, pc[k] + 1 + (k // 15) % 2)
        pc[k + 3] = min(255, pc[k + 3] + 1)
    c = make_run(root, "C", {4600: pc})
    code, text, _ = run(a, c, "--strict")
    expect(code == 0, f"capped noise, --strict: exit {code}:\n{text}")


def not_comparable(root):
    a = make_run(root, "A", {1600: picture(), 2200: picture()}, swaps={1600: 4490, 2200: 5090})
    # B: one shot missing.
    b = make_run(root, "B", {1600: picture()}, swaps={1600: 4490})
    code, text, _ = run(a, b)
    expect(code == 2 and "shot 2200: only in run A" in text, f"missing shot: exit {code}:\n{text}")
    # C: another swap for 2200 (another frame).
    c = make_run(root, "C", {1600: picture(), 2200: picture()}, swaps={1600: 4490, 2200: 5091})
    code, text, _ = run(a, c)
    expect(code == 2 and "swap 5090" in text and "swap 5091" in text, f"other swap: exit {code}:\n{text}")
    # D: another size.
    d = os.path.join(root, "D")
    os.makedirs(d)
    write_ppm(os.path.join(d, "shot-1600.ppm"), picture(width=32), width=32)
    write_ppm(os.path.join(d, "shot-2200.ppm"), picture())
    code, text, _ = run(a, d)
    expect(code == 2 and "64x32 in run A, 32x32 in run B" in text, f"other size: exit {code}:\n{text}")
    # E: an ASCII PPM.
    e = os.path.join(root, "E")
    os.makedirs(e)
    write_ppm(os.path.join(e, "shot-1600.ppm"), b"", header=b"P3\n1 1\n255\n0 0 0\n")
    write_ppm(os.path.join(e, "shot-2200.ppm"), picture())
    code, text, _ = run(a, e)
    expect(code == 2 and "not a binary PPM" in text, f"P3: exit {code}:\n{text}")
    # F: no shots at all.
    f = os.path.join(root, "F")
    os.makedirs(f)
    code, text, _ = run(f, f)
    expect(code == 2 and "no shot-<step>.ppm" in text, f"no shots: exit {code}:\n{text}")


def unverified(root):
    a = make_run(root, "A", {1600: picture()}, swaps={1600: 4490}, statuses={1600: "unverified"})
    b = make_run(root, "B", {1600: picture()}, swaps={1600: 4490})
    code, text, _ = run(a, b)
    expect(code == 0 and "warning: shot 1600: run A says unverified" in text, f"unverified: exit {code}:\n{text}")


def shown(root):
    # NFSMW_BENCH_SHOTS_SHOWN: the front images at two internal resolutions
    # (64x32 against 48x24: not comparable), the pictures as shown at the
    # window's size (--shown: compared, the front images left alone).
    a = make_run(root, "A", {1600: picture()}, swaps={1600: 4490})
    b = make_run(root, "B", {}, swaps={})
    write_ppm(os.path.join(b, "shot-1600.ppm"), picture(1, 48, 24), 48, 24)
    with open(os.path.join(b, "summary.json"), "w", encoding="utf-8") as f:
        json.dump({"status": "ok", "shots": {"verified": True, "taken": [
            {"step": 1600, "status": "ok", "swap": 4490, "shown_status": "failed", "shown_why": "dropped"}]}}, f)
    pa, pb = picture(3), picture(3)
    pb[(5 * W + 7) * 3] ^= 0x10
    write_ppm(os.path.join(a, "shot-1600-shown.ppm"), pa)
    write_ppm(os.path.join(b, "shot-1600-shown.ppm"), pb)
    code, text, _ = run(a, b)
    expect(code == 2 and "48x24 in run B" in text and "-shown" not in text.split("error:")[0], f"front images: exit {code}:\n{text}")
    code, text, result = run(a, b, "--shown", json_path=os.path.join(root, "s.json"))
    expect(code == 0 and result and result["shots"][0]["step"] == 1600 and not result["shots"][0]["identical"],
           f"--shown: exit {code}:\n{text}")
    expect("warning: shot 1600: run B's picture as shown says failed (dropped)" in text, f"--shown status:\n{text}")
    f = os.path.join(root, "F")
    os.makedirs(f)
    code, text, _ = run(f, f, "--shown")
    expect(code == 2 and "no shot-<step>-shown.ppm" in text, f"--shown, no shots: exit {code}:\n{text}")


case("identical", identical)
case("one pixel off", one_pixel)
case("a block off, and where", block)
case("run-to-run noise", noise)
case("not comparable", not_comparable)
case("unverified shots", unverified)
case("pictures as shown", shown)

print("FAILED" if failures else "ok")
sys.exit(1 if failures else 0)
