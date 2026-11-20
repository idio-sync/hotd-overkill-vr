"""Gun accuracy report from the mod's shot log (development; standard library only).

    .venv\\Scripts\\python -I tools/gun_accuracy.py C:\\Users\\Virtual\\hotd-vr-game\\hovr\\probe.log [--out REPORT.txt]
    .venv\\Scripts\\python -I tools/gun_accuracy.py --selftest

The log comes from `vr shotlog 1` (src/mod/gun.cpp; `vr shotlog tag NAME` starts a scenario) and
`vr gunmeshcheck` (src/mod/gunmodel.cpp). The development harness logs to hovr\\probe.log, a
release install to hovr\\hovr.log; the format is the same. Lines read:

  shotlog shot id= frame= weapon= key= ... aim=hand|mouse o= d= fire_o= fire_d= drawn_frame= drawn_o= drawn_d= turn= ...
  shotlog pellet id= frame= pellet= of= depth= o= d= len= hit= p= ent= shoot=
  shotlog proj id= frame= weapon= key= launched= speed= o= d= fire_o= fire_d= drawn_o= drawn_d= turn= ...
  shotlog mesh weapon= key= source=builtin|override axis= angle_deg= perp_cm= along_cm= ...
  shotlog mesh_override weapon= key= fields= ...
  shotlog tag name=    shotlog on|off unit_m=

For every hand-aimed shot, each of its bullets (depth 0: FireBullets' own ray casts; the
pass-through bullets of its recursive calls, depth 1+, are counted but kept out) is measured
against two barrel lines:
  - drawn: the barrel as drawn in the last frame on screen before the shot (muzzle drawn_o,
    direction drawn_d, from the world matrix the gun was drawn with);
  - fire:  the barrel line the mod computed at fire time (fire_o, fire_d: the shot ray it set).
The bullet's impact is its hit point (or, for a miss, the end of its ray). Its bias: the angle
between impact - muzzle and the barrel direction (degrees), split into yaw (+ right) and pitch
(+ up) about the world's up, and its perpendicular distance from the barrel line at the impact
(cm: game units * unit_m * 100). Per shot, the bullets' mean direction is the shot's centre;
multi-pellet weapons' spread is the bullets' angles about that centre, apart from the bias.

Per weapon and scenario tag: the systematic bias (the mean yaw and pitch error over the shots;
the length of that mean), the per-shot centre bias (mean, p95, max), the perpendicular distance,
the spread, the frame lag between the shot and the drawn frame, and the bias against the rail
camera's turn speed in the drawn frame (bins 0-30, 30-100, 100-300, 300+ deg/s). Thresholds
(docs/superpowers/specs/2026-11-19-gun-accuracy-design.md): still (0-30 deg/s) systematic bias
<= 0.25 degrees; mesh check within 0.5 degrees and 0.5 cm (across the axis; along it is reported).
Projectiles (no impact logged): the launch direction's angle to the drawn barrel and the launch
point's distance from the drawn barrel line.
"""
import argparse
import math
import random
import re
import sys

STILL_MAX_DEG = 0.25
MESH_MAX_DEG = 0.5
MESH_MAX_CM = 0.5
TURN_BINS = [(0, 30), (30, 100), (100, 300), (300, float("inf"))]

LINE = re.compile(r"shotlog (\w+)\s*(.*)$")
TOKEN = re.compile(r'(\w+)=("[^"]*"|\S*)')


# ---------------------------------------------------------------- vectors

def sub(a, b):
    return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]


def add(a, b):
    return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]


def scale(a, s):
    return [a[0] * s, a[1] * s, a[2] * s]


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def norm(a):
    return math.sqrt(dot(a, a))


def unit(a):
    n = norm(a)
    return scale(a, 1 / n) if n > 0 else a


def angle_deg(a, b):
    return math.degrees(math.atan2(norm(cross(a, b)), dot(a, b)))


def barrel_frame(d):
    """Right and up axes across a (unit) barrel direction, about the world's up (+y)."""
    right = unit(cross([0, 1, 0], d))
    if norm(right) < 1e-6:
        right = [1, 0, 0]
    return right, cross(d, right)


def errors(point, origin, d):
    """A point against a line (origin, unit d): (angle deg, yaw deg + right, pitch deg + up,
    perpendicular distance in game units)."""
    v = sub(point, origin)
    right, up = barrel_frame(d)
    along = dot(v, d)
    return (angle_deg(v, d), math.degrees(math.atan2(dot(v, right), along)), math.degrees(math.atan2(dot(v, up), along)),
            norm(cross(v, d)))


# ---------------------------------------------------------------- parsing

def vec(s):
    try:
        p = [float(x) for x in s.split(",")]
        return p if len(p) >= 3 else None
    except ValueError:
        return None


def parse(lines):
    shots, order, projs, meshes, overrides = {}, [], [], [], []
    tag, unit_m = "untagged", 0.01
    for line in lines:
        m = LINE.search(line)
        if not m:
            continue
        kind, rest = m.group(1), m.group(2)
        f = {k: v.strip('"') for k, v in TOKEN.findall(rest)}
        if kind == "tag":
            tag = f.get("name", "untagged")
        elif kind in ("on", "off"):
            unit_m = float(f.get("unit_m", unit_m))
        elif kind == "shot":
            f["tag"], f["unit_m"], f["pellets_list"] = tag, unit_m, []
            shots[f["id"]] = f
            order.append(f["id"])
        elif kind == "pellet":
            s = shots.get(f.get("id"))
            if s is not None:
                s["pellets_list"].append(f)
        elif kind == "proj":
            f["tag"], f["unit_m"] = tag, unit_m
            projs.append(f)
        elif kind == "mesh":
            meshes.append(f)
        elif kind == "mesh_override":
            overrides.append(f)
    return [shots[i] for i in order], projs, meshes, overrides


def weapon_name(f):
    k = f.get("key", "?")
    return k if k not in ("?", "") else "type%s_kind%s" % (f.get("type", "?"), f.get("kind", "?"))


# ---------------------------------------------------------------- measuring

def measure_shot(s):
    """Per-shot measurements, or None (mouse aim, no drawn frame, no bullets)."""
    if s.get("aim") != "hand" or s.get("drawn") != "1":
        return None
    um = s["unit_m"] * 100  # cm per game unit
    lines = {"drawn": (vec(s["drawn_o"]), unit(vec(s["drawn_d"]))), "fire": (vec(s["fire_o"]), unit(vec(s["fire_d"])))}
    bullets = [p for p in s["pellets_list"] if p.get("depth") == "0"]
    out = {"turn": float(s.get("turn", 0)), "lag": int(s["frame"]) - int(s["drawn_frame"]), "n": len(bullets),
           "hits": sum(1 for p in bullets if p.get("hit") == "1"), "through": len(s["pellets_list"]) - len(bullets),
           "pellets_per_shot": int(s.get("pellets", "1") or 1)}
    if not bullets:
        return None
    impacts = []
    for p in bullets:
        o, d = vec(p["o"]), unit(vec(p["d"]))
        impacts.append(vec(p["p"]) if p.get("hit") == "1" else add(o, scale(d, float(p["len"]))))
    # The spread-free ray the mod set (o, d), carried to each bullet's distance: the mod's own part
    # of the error, without the game's spread ("set" against the drawn barrel).
    so, sd = vec(s["o"]), unit(vec(s["d"]))
    set_points = [add(so, scale(sd, norm(sub(q, so)))) for q in impacts]
    for name, (mo, md) in lines.items():
        if mo is None or md is None:
            continue
        for label, pts in ((name, impacts), ("set" if name == "drawn" else None, set_points)):
            if label is None:
                continue
            per = [errors(q, mo, md) for q in pts]
            centre = unit([sum(c) for c in zip(*[unit(sub(q, mo)) for q in pts])])
            ca, cy, cp, _ = errors(add(mo, centre), mo, md)
            out[label] = {"centre": ca, "yaw": cy, "pitch": cp, "perp_cm": [e[3] * um for e in per], "angles": [e[0] for e in per]}
    dirs = [unit(vec(p["d"])) for p in bullets]
    mean = unit([sum(c) for c in zip(*dirs)])
    out["spread"] = [angle_deg(d, mean) for d in dirs] if len(dirs) > 1 else []
    out["game_spread"] = [angle_deg(d, sd) for d in dirs]  # each bullet against the spread-free ray
    return out


def mean_bias(have):
    """The systematic bias of a set of shots: mean yaw and pitch, their length, and two standard
    errors of that length (inf for one shot)."""
    n = len(have)
    my, mp = sum(h["yaw"] for h in have) / n, sum(h["pitch"] for h in have) / n
    if n < 2:
        return my, mp, math.hypot(my, mp), float("inf")
    vy = sum((h["yaw"] - my) ** 2 for h in have) / (n - 1)
    vp = sum((h["pitch"] - mp) ** 2 for h in have) / (n - 1)
    return my, mp, math.hypot(my, mp), 2 * math.sqrt((vy + vp) / n)


def still_verdict(bias, se2):
    if bias <= STILL_MAX_DEG:
        return "PASS"
    if bias - se2 > STILL_MAX_DEG:
        return "FAIL"
    return "NOT SURE (the game's spread: more shots)"


def stats(xs):
    if not xs:
        return None
    xs = sorted(xs)
    p95 = xs[min(len(xs) - 1, int(round(0.95 * (len(xs) - 1))))]
    return sum(xs) / len(xs), p95, xs[-1]


def fmt3(st, digits=2):
    if st is None:
        return "-"
    return "/".join(("%." + str(digits) + "f") % x for x in st)


def report(shots, projs, meshes, overrides):
    out = []
    w = out.append
    groups = {}
    mouse = 0
    for s in shots:
        r = measure_shot(s)
        if r is None:
            mouse += 1
            continue
        groups.setdefault((weapon_name(s), s["tag"]), []).append(r)
    w("Gun accuracy report: %d shots logged (%d measured, %d skipped: mouse aim, no drawn frame or no bullets), %d projectile launches, "
      "%d mesh checks" % (len(shots), len(shots) - mouse, mouse, len(projs), len(meshes)))
    w("Angles in degrees, distances in cm; mean/p95/max. bias = systematic (length of the mean yaw/pitch error); centre = each shot's "
      "bullets' mean direction against the barrel line; perp = each bullet's distance from the barrel line at its impact.")
    verdicts = []
    for (weapon, tag), rs in sorted(groups.items()):
        w("")
        bullets = sum(r["n"] for r in rs)
        w("== %s [%s]: %d shots, %d bullets (%d hits), %d pass-through casts excluded; shot-to-drawn frame lag %s" % (
            weapon, tag, len(rs), bullets, sum(r["hits"] for r in rs), sum(r["through"] for r in rs),
            fmt3(stats([float(r["lag"]) for r in rs]), 1)))
        w("   %-6s %8s %7s %8s %8s %18s %18s" % ("line", "bias", "+-2se", "yaw", "pitch", "centre m/p95/max", "perp cm m/p95/max"))
        for line in ("drawn", "fire", "set"):
            have = [r[line] for r in rs if line in r]
            if not have:
                continue
            my, mp, b, se2 = mean_bias(have)
            w("   %-6s %8.3f %7.3f %+8.3f %+8.3f %18s %18s" % (line, b, se2, my + 0.0, mp + 0.0, fmt3(stats([h["centre"] for h in have]), 3),
                                                            fmt3(stats([c for h in have for c in h["perp_cm"]]), 2)))
        w("   (drawn: bullets against the barrel drawn in the frame on screen; fire: against the barrel line at fire time; set: the "
          "spread-free ray the mod set, against the drawn barrel)")
        gs = [a for r in rs for a in r["game_spread"]]
        w("   game spread (each bullet against the spread-free ray): mean/p95/max %s deg" % fmt3(stats(gs), 2))
        spread = [a for r in rs for a in r["spread"]]
        if spread:
            rms = math.sqrt(sum(a * a for a in spread) / len(spread))
            w("   spread (multi-pellet, about each shot's mean): rms %.2f deg, mean/p95/max %s" % (rms, fmt3(stats(spread), 2)))
        w("   by the rail camera's turn speed in the drawn frame (deg/s):")
        w("   %-9s %5s %8s %7s %8s %8s %18s %18s %8s" % ("bin", "shots", "bias", "+-2se", "yaw", "pitch", "drawn centre", "fire centre",
                                                        "set bias"))
        for lo, hi in TURN_BINS:
            b = [r for r in rs if lo <= r["turn"] < hi and "drawn" in r]
            if not b:
                continue
            my, mp, bias, se2 = mean_bias([r["drawn"] for r in b])
            sb = mean_bias([r["set"] for r in b if "set" in r])[2]
            label = "%g-%g" % (lo, hi) if hi != float("inf") else "%g+" % lo
            w("   %-9s %5d %8.3f %7.3f %+8.3f %+8.3f %18s %18s %8.3f" % (label, len(b), bias, se2, my + 0.0, mp + 0.0,
                                                                   fmt3(stats([r["drawn"]["centre"] for r in b]), 3),
                                                                   fmt3(stats([r["fire"]["centre"] for r in b if "fire" in r]), 3), sb))
            if lo == 0:
                verdicts.append("%s [%s] still bias %.3f +- %.3f deg (<= %.2f; the mod's set ray %.3f): %s" % (
                    weapon, tag, bias, se2, STILL_MAX_DEG, sb, still_verdict(bias, se2)))
    if projs:
        w("")
        w("== projectiles (launch only; landing not logged)")
        pg = {}
        for p in projs:
            pg.setdefault((weapon_name(p), p["tag"]), []).append(p)
        for (weapon, tag), ps in sorted(pg.items()):
            ang, perp, dist, launched = [], [], [], 0
            for p in ps:
                launched += p.get("launched") == "1"
                if p.get("aim") != "hand" or p.get("drawn") != "1":
                    continue
                mo, md = vec(p["drawn_o"]), unit(vec(p["drawn_d"]))
                o, d = vec(p["o"]), unit(vec(p["d"]))
                um = p["unit_m"] * 100
                ang.append(angle_deg(d, md))
                perp.append(norm(cross(sub(o, mo), md)) * um)
                dist.append(norm(sub(o, mo)) * um)
            w("   %s [%s]: %d launches (%d launched): direction vs drawn barrel %s deg; launch point off the drawn line %s cm, from the drawn "
              "muzzle %s cm" % (weapon, tag, len(ps), launched, fmt3(stats(ang), 3), fmt3(stats(perp), 2), fmt3(stats(dist), 2)))
    if meshes or overrides:
        w("")
        w("== mesh check (live vertex buffer against the description; thresholds %.1f deg, %.1f cm across the axis)" % (MESH_MAX_DEG, MESH_MAX_CM))
        for m in meshes:
            perp_ok = float(m.get("perp_cm", "inf")) <= MESH_MAX_CM
            if m.get("axis") == "1":
                ok = float(m["angle_deg"]) <= MESH_MAX_DEG and perp_ok
                v = "PASS" if ok else "FAIL"
            elif m.get("muzzle") == "1":
                v = "muzzle only %s (no axis: %s)" % ("PASS" if perp_ok else "FAIL", m.get("why", ""))
            else:
                v = "NO FIT (%s)" % m.get("why", "")
            if m.get("source") == "override":
                v += " (override in effect: not a failure)"
            elif m.get("axis") == "1" or m.get("muzzle") == "1":
                verdicts.append("mesh %s %s" % (m.get("key"), v))
            how = m.get("method", "?")
            if how == "edges":
                how += " (%s pairs)" % m.get("edge_support")
            w("   %-12s %-9s angle %s deg, muzzle across %s cm, along %s cm; axis by %s, %s rings on it (%s round parts), radius %s cm, "
              "span %s cm: %s" % (m.get("key"), m.get("source"), m.get("angle_deg"), m.get("perp_cm"), m.get("along_cm"), how, m.get("rings"),
                                  m.get("round_rings"), m.get("radius_cm"), m.get("span_cm"), v))
        for o in overrides:
            w("   override %s: %s (barrel %s, muzzle %s, grip %s, hold %s) - logged, not a failure" % (o.get("key"), o.get("fields"),
                                                                                                  o.get("barrel"), o.get("muzzle"),
                                                                                                  o.get("grip"), o.get("hold")))
    if verdicts:
        w("")
        w("== verdicts")
        for v in verdicts:
            w("   " + v)
    return "\n".join(out)


# ---------------------------------------------------------------- self-test

def rotate(d, yaw_deg, pitch_deg):
    """d turned by yaw (+ right) and pitch (+ up) in its barrel frame."""
    right, up = barrel_frame(d)
    y, p = math.radians(yaw_deg), math.radians(pitch_deg)
    v = add(add(scale(d, math.cos(p) * math.cos(y)), scale(right, math.cos(p) * math.sin(y))), scale(up, math.sin(p)))
    return unit(v)


def synthetic_log():
    """Magnum: still shots 0.1 deg right of the drawn barrel; turning (150 deg/s) shots 2 deg low.
    Pump: 6 pellets, 3 deg spread about a centre on the barrel. A crossbow launch and mesh lines."""
    rnd = random.Random(7)
    lines = ["[ 0.000] shotlog on frame=1 unit_m=0.01000"]
    sid = 0

    def shot(key, pellets, tag_turn, yaw, pitch, spread):
        nonlocal sid
        sid += 1
        mo = [rnd.uniform(-50, 50), 150, rnd.uniform(-50, 50)]
        md = unit([rnd.uniform(-0.3, 0.3), rnd.uniform(-0.2, 0.1), 1])
        centre = rotate(md, yaw, pitch)
        frame = 100 + sid * 3
        lines.append("[ 1.000] shotlog shot id=%d frame=%d weapon=GunArms_X key=%s data=0x1 type=1 kind=2 pellets=%d range=20000 aim=hand "
                     "o=%.3f,%.3f,%.3f d=%.6f,%.6f,%.6f fire_o=%.3f,%.3f,%.3f fire_d=%.6f,%.6f,%.6f fire_turn=%.1f drawn=1 drawn_frame=%d "
                     "drawn_key=%s drawn_o=%.3f,%.3f,%.3f drawn_d=%.6f,%.6f,%.6f turn=%.1f turn_frame=%.1f" % (
                         sid, frame, key, pellets, *mo, *md, *mo, *md, tag_turn, frame - 1, key, *mo, *md, tag_turn, tag_turn))
        phase = rnd.uniform(0, 2 * math.pi)
        for k in range(pellets):
            a = phase + 2 * math.pi * k / pellets  # evenly round the centre: their mean is the centre
            d = rotate(centre, spread * math.cos(a), spread * math.sin(a)) if spread else centre
            p = add(mo, scale(d, 1000))
            lines.append("[ 1.000] shotlog pellet id=%d frame=%d pellet=%d of=%d depth=0 o=%.3f,%.3f,%.3f d=%.6f,%.6f,%.6f len=20000.0 hit=1 "
                         "p=%.3f,%.3f,%.3f ent=0x1 shoot=1" % (sid, frame, k, pellets, *mo, *d, *p))
            if k == 0:  # a pass-through cast (depth 1), wildly off: must be excluded
                lines.append("[ 1.000] shotlog pellet id=%d frame=%d pellet=0 of=1 depth=1 o=%.3f,%.3f,%.3f d=0,1,0 len=500.0 hit=1 "
                             "p=0,0,0 ent=0x1 shoot=1" % (sid, frame, *p))

    lines.append("[ 0.500] shotlog tag name=still frame=50")
    for _ in range(10):
        shot("magnum", 1, 0.0, 0.1, 0.0, 0)
    for _ in range(5):
        shot("pump", 6, 5.0, 0.0, 0.0, 3.0)
    lines.append("[ 2.000] shotlog tag name=turning frame=500")
    for _ in range(10):
        shot("magnum", 1, 150.0, 0.0, -2.0, 0)
    lines.append("[ 3.000] shotlog shot id=999 frame=600 weapon=GunArms_Magnum key=magnum aim=mouse o=0,0,0 d=0,0,1 fire_o=- fire_d=- drawn=0")
    lines.append("[ 3.100] shotlog proj id=1000 frame=601 weapon=GunArms_CrossBow key=crossbow aim=hand launched=1 speed=3000.0 o=0.5,0,0 "
                 "d=0,0,1 fire_o=0,0,0 fire_d=0,0,1 drawn=1 drawn_frame=600 drawn_key=crossbow drawn_o=0,0,0 drawn_d=0,0,1 turn=0 turn_frame=0")
    lines.append('[ 4.000] shotlog mesh weapon=GunArms_Magnum key=magnum source=builtin axis=1 muzzle=1 method=rings rings=3 round_rings=4 slab_points=90 '
                 'radius_cm=0.9 rms_cm=0.001 span_cm=5 fit_dir=1,0,0 fit_muzzle=23.4,8.4,-0.87 def_dir=1,0,0 def_muzzle=23.4,8.4,-0.87 '
                 'angle_deg=0.120 perp_cm=0.050 along_cm=-0.2 why=""')
    lines.append('[ 4.100] shotlog mesh weapon=GunArms_SMG key=smg source=builtin axis=1 muzzle=1 method=rings rings=2 round_rings=2 slab_points=60 radius_cm=0.8 '
                 'rms_cm=0.001 span_cm=3 fit_dir=1,0,0 fit_muzzle=0,0,0 def_dir=1,0,0 def_muzzle=0,0,0 angle_deg=0.900 perp_cm=0.100 '
                 'along_cm=0 why=""')
    return lines


def selftest():
    shots, projs, meshes, overrides = parse(synthetic_log())
    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)

    check(len(shots) == 26 and len(projs) == 1 and len(meshes) == 2, "parsed counts")
    rs = [measure_shot(s) for s in shots]
    mag_still = [r for r, s in zip(rs, shots) if r and s["key"] == "magnum" and s["tag"] == "still"]
    mag_turn = [r for r, s in zip(rs, shots) if r and s["key"] == "magnum" and s["tag"] == "turning"]
    pump = [r for r, s in zip(rs, shots) if r and s["key"] == "pump"]
    check(rs[-1] is None, "mouse shot skipped")
    check(all(abs(r["drawn"]["yaw"] - 0.1) < 1e-3 and abs(r["drawn"]["pitch"]) < 1e-3 for r in mag_still), "still magnum 0.1 deg right")
    check(all(abs(r["drawn"]["pitch"] + 2.0) < 1e-3 for r in mag_turn), "turning magnum 2 deg low")
    check(all(r["through"] == 1 and r["n"] == 1 for r in mag_still), "pass-through excluded")
    # The perpendicular distance at 10 m for 0.1 deg: 1000 units * sin(0.1 deg) = 1.745 units = 1.745 cm.
    check(all(abs(r["drawn"]["perp_cm"][0] - 1000 * math.sin(math.radians(0.1))) < 0.01 for r in mag_still), "perp cm")
    sp = [a for r in pump for a in r["spread"]]
    check(len(sp) == 30 and all(abs(a - 3.0) < 0.01 for a in sp), "pump spread 3 deg")
    check(all(r["drawn"]["centre"] < 0.01 for r in pump), "pump centre on the barrel")
    text = report(shots, projs, meshes, overrides)
    check("magnum [still] still bias 0.100 +- 0.000 deg (<= 0.25; the mod's set ray 0.000): PASS" in text, "still verdict PASS")
    check(all(abs(r["game_spread"][0] - 0.1) < 1e-3 for r in mag_still), "game spread against the set ray")
    check(all(r["set"]["centre"] < 1e-3 for r in mag_still), "set ray on the barrel")
    check("mesh smg FAIL" in text and "mesh magnum PASS" in text, "mesh verdicts")
    check("launch point off the drawn line 0.50/0.50/0.50 cm" in text, "projectile offset")
    print(text)
    print()
    if fails:
        print("SELFTEST FAILED: " + "; ".join(fails))
        return 1
    print("selftest passed")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log", nargs="?")
    ap.add_argument("--out")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.log:
        ap.error("a log file or --selftest")
    with open(a.log, encoding="utf-8", errors="replace") as f:
        shots, projs, meshes, overrides = parse(f)
    text = report(shots, projs, meshes, overrides)
    print(text)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
