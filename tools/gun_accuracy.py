"""Gun accuracy report from the mod's shot log (development; standard library only).

    .venv\\Scripts\\python -I tools/gun_accuracy.py C:\\Users\\Virtual\\hotd-vr-game\\hovr\\probe.log [MORE.log ...] [--summary] [--out REPORT.txt]
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
(docs/DEVELOPMENT.md, "Gun accuracy test"): still (0-30 deg/s) systematic bias
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
MAX_LAG = 3  # frames between the shot and the last frame the gun was drawn; more: not measured
MIN_DIST_M = 0.3  # bullets hitting closer than this to the drawn muzzle are not measured

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
    if int(s["frame"]) - int(s["drawn_frame"]) > MAX_LAG:
        return None  # the gun was not drawn just before this shot (a flat screen, a level transition)
    um = s["unit_m"] * 100  # cm per game unit
    lines = {"drawn": (vec(s["drawn_o"]), unit(vec(s["drawn_d"]))), "fire": (vec(s["fire_o"]), unit(vec(s["fire_d"])))}
    bullets = [p for p in s["pellets_list"] if p.get("depth") == "0"]
    # A hit closer than MIN_DIST_M to the drawn muzzle (a zombie at the gun) has no usable angle.
    dm0 = vec(s["drawn_o"])
    if dm0:
        bullets = [p for p in bullets if p.get("hit") != "1" or norm(sub(vec(p["p"]), dm0)) * s["unit_m"] >= MIN_DIST_M]
    out = {"turn": float(s.get("turn", 0)), "lag": int(s["frame"]) - int(s["drawn_frame"]), "n": len(bullets),
           "hits": sum(1 for p in bullets if p.get("hit") == "1"), "through": len(s["pellets_list"]) - len(bullets),
           "pellets_per_shot": int(s.get("pellets", "1") or 1),
           # The hand's and the drawn barrel's angular speed in the drawn frame (newer logs only).
           "hand_turn": float(s.get("hand_turn", 0) or 0), "barrel_turn": float(s.get("barrel_turn", 0) or 0),
           "shootable": sum(1 for p in bullets if p.get("hit") == "1" and p.get("shoot") == "1")}
    if not bullets:
        return None
    impacts = []
    for p in bullets:
        o, d = vec(p["o"]), unit(vec(p["d"]))
        impacts.append(vec(p["p"]) if p.get("hit") == "1" else add(o, scale(d, float(p["len"]))))
    # How far the hand's pointing axis (camera space) turned between the frame drawn and the shot.
    dh, fh = vec(s.get("drawn_hand", "") or ""), vec(s.get("fire_hand", "") or "")
    hd, hf = (vec(",".join(s["drawn_hand"].split(",")[3:])), vec(",".join(s["fire_hand"].split(",")[3:]))) if dh and fh else (None, None)
    out["hand_df"] = angle_deg(hd, hf) if hd and hf else 0.0
    dm = vec(s["drawn_o"])
    out["dist_m"] = [norm(sub(q, dm)) * s["unit_m"] for q in impacts] if dm else []
    out["shoot_dist_m"] = [x for x, p in zip(out["dist_m"], bullets) if p.get("hit") == "1" and p.get("shoot") == "1"]
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
    # A multi-pellet weapon's first pellet gets no spread (FireBullets spreads every pellet but the
    # first): the shot's aim without the game's spread, against the drawn barrel.
    if out["pellets_per_shot"] > 1 and lines["drawn"][0] is not None:
        first = [q for q, p in zip(impacts, bullets) if p.get("pellet") == "0"]
        if first:
            fa, fy, fp, _ = errors(first[0], *lines["drawn"])
            out["first"] = {"centre": fa, "yaw": fy, "pitch": fp}
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
    w("Gun accuracy report: %d shots logged (%d measured, %d skipped: mouse aim, no drawn frame within 3 frames, or no bullets), %d projectile launches, "
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
            if lo == 0 and not tag.startswith("sweep"):  # a sweeping hand is not the still test
                verdicts.append("%s [%s] still bias %.3f +- %.3f deg (<= %.2f; the mod's set ray %.3f): %s" % (
                    weapon, tag, bias, se2, STILL_MAX_DEG, sb, still_verdict(bias, se2)))
        if any(r["hand_turn"] > 0 for r in rs):
            w("   by the hand's angular speed in the drawn frame (deg/s, camera space; rail under 30 deg/s only):")
            w("   %-9s %5s %8s %7s %8s %8s %18s %18s" % ("bin", "shots", "bias", "+-2se", "yaw", "pitch", "drawn centre", "fire centre"))
            for lo, hi in TURN_BINS:
                b = [r for r in rs if lo <= r["hand_turn"] < hi and r["turn"] < 30 and "drawn" in r]
                if not b:
                    continue
                my, mp, bias, se2 = mean_bias([r["drawn"] for r in b])
                label = "%g-%g" % (lo, hi) if hi != float("inf") else "%g+" % lo
                w("   %-9s %5d %8.3f %7.3f %+8.3f %+8.3f %18s %18s" % (label, len(b), bias, se2, my + 0.0, mp + 0.0,
                                                               fmt3(stats([r["drawn"]["centre"] for r in b]), 3),
                                                               fmt3(stats([r["fire"]["centre"] for r in b if "fire" in r]), 3)))
        dist = [x for r in rs for x in r.get("dist_m", [])]
        sd = [x for r in rs for x in r.get("shoot_dist_m", [])]
        w("   impact distance from the drawn muzzle (m) mean/p95/max %s; %d bullets hit a shootable entity, at %s m" % (
            fmt3(stats(dist), 1), len(sd), fmt3(stats(sd), 1)))
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
                if p.get("aim") != "hand" or p.get("drawn") != "1" or int(p["frame"]) - int(p["drawn_frame"]) > MAX_LAG:
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


# ---------------------------------------------------------------- summary (--summary)

WEAPON_ORDER = ["magnum", "gold_magnum", "handcannon", "pump", "autoshotgun", "smg", "rifle", "minigun", "crossbow"]


def scenario_of(tag, turn, hand_turn):
    """The summary's scenario for a shot: still (a still* tag, rail and hand under 30 deg/s), sweep60 /
    sweep180 (a sweep* tag, rail under 30 deg/s), turn (any other tag but sweep*, rail at 30+ deg/s; the
    controller is held still there, though the comfort smoothing turns the hand in camera space), else
    None."""
    if tag.startswith("sweep"):
        return tag if turn < 30 else None
    if turn >= 30:
        return "turn"
    return "still" if tag.startswith("still") and hand_turn < 30 else None


def median(xs):
    xs = sorted(xs)
    if not xs:
        return float("nan")
    n = len(xs)
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2


def summary(shots, projs, meshes):
    out = []
    w = out.append
    rows = {}
    for s in shots:
        r = measure_shot(s)
        if r is None or "drawn" not in r:
            continue
        sc = scenario_of(s["tag"], r["turn"], r["hand_turn"])
        if sc:
            rows.setdefault((weapon_name(s), sc), []).append(r)
    weapons = sorted({k[0] for k in rows} | {p.get("key", "?") for p in projs} | {m.get("key", "?") for m in meshes},
                     key=lambda k: (WEAPON_ORDER.index(k) if k in WEAPON_ORDER else 99, k))
    w("Summary: bullets against the barrel as drawn in the frame on screen (deg; cm = the bias at the median impact distance)")
    w("still: still* tags, rail and hand < 30 deg/s; turn: rail >= 30 deg/s (any tag but sweep*); sweepN: hand sweeping, "
      "rail < 30 deg/s; ctr = each shot's bullets' mean direction against the drawn barrel, mean/p95; first pellet = a "
      "multi-pellet weapon's unspread first pellet against the drawn barrel (bias +- 2se, max); mod's ray = the spread-free "
      "ray the mod set, carried to each impact distance, against the drawn barrel")
    w("%-12s %-9s %5s %7s %7s %6s %7s %11s %7s %13s %11s %8s  %s" % (
        "weapon", "scenario", "shots", "bullets", "bias", "+-2se", "bias_cm", "ctr m/p95", "dist_m", "spread m/p95", "rail/hand", "hits", "verdict"))
    for wp in weapons:
        for sc in ("still", "turn", "sweep60", "sweep180"):
            rs = rows.get((wp, sc))
            if not rs:
                continue
            my, mp, b, se2 = mean_bias([r["drawn"] for r in rs])
            dist = [x for r in rs for x in r["dist_m"]]
            md = median(dist)
            gs = stats([a for r in rs for a in r["game_spread"]])
            verdict = still_verdict(b, se2) if sc == "still" else ""
            st = [r["set"] for r in rs if "set" in r]
            if st and (gs and gs[1] > 0.01):  # the game spreads this weapon's bullets: the mod's own ray decides
                sb = mean_bias(st)
                verdict = ("%s; " % verdict if verdict else "") + "mod's ray %.3f +- %.3f, ctr max %.3f" % (
                    sb[2], sb[3] if sb[3] != float("inf") else 0, max(x["centre"] for x in st))
                if sc == "still":
                    verdict += " (%s)" % still_verdict(sb[2], sb[3])
            if sc.startswith("sweep") and sc[5:].isdigit():
                hd = [r["hand_df"] for r in rs if r["hand_df"] > 0.1]
                if hd:
                    verdict = "hand turned %.2f deg (median) from the frame drawn to the shot = %.0f ms at %s deg/s" % (
                        median(hd), median(hd) / float(sc[5:]) * 1000, sc[5:])
            fr = [r["first"] for r in rs if "first" in r]
            if fr:  # multi-pellet: the unspread first pellet decides
                fb = mean_bias(fr)
                verdict = ("%s; " % verdict if verdict else "") + "first pellet %.3f +- %.3f, ctr max %.3f" % (
                    fb[2], fb[3] if fb[3] != float("inf") else 0, max(f["centre"] for f in fr))
                if sc == "still":
                    verdict += " (%s)" % still_verdict(fb[2], fb[3])
            cs = stats([r["drawn"]["centre"] for r in rs])
            w("%-12s %-9s %5d %7d %7.3f %6.3f %7.2f %11s %7.1f %13s %11s %8s  %s" % (
                wp, sc, len(rs), sum(r["n"] for r in rs), b, se2 if se2 != float("inf") else 0, math.tan(math.radians(b)) * md * 100,
                "%.2f/%.2f" % cs[:2], md, ("%.2f/%.2f" % gs[:2]) if gs else "-",
                "%.0f/%.0f" % (median([r["turn"] for r in rs]), median([r["hand_turn"] for r in rs])),
                "%d/%d" % (sum(r["shootable"] for r in rs), sum(r["n"] for r in rs)), verdict))
        rs = rows.get((wp, "turn"))
        if rs:
            parts = []
            for lo, hi in TURN_BINS[1:]:
                b = [r for r in rs if lo <= r["turn"] < hi]
                if b:
                    parts.append("%s: %d shots %.3f deg" % ("%g-%g" % (lo, hi) if hi != float("inf") else "%g+" % lo, len(b),
                                                            mean_bias([r["drawn"] for r in b])[2]))
            w("%-12s   turn by rail speed: %s" % ("", "; ".join(parts)))
    if projs:
        w("")
        w("Projectiles (launch only, landing not logged): launch direction vs the drawn barrel (deg), launch point off the drawn barrel line (cm)")
        pg = {}
        for p in projs:
            if p.get("aim") != "hand" or p.get("drawn") != "1" or p.get("launched") != "1":
                continue
            if int(p["frame"]) - int(p["drawn_frame"]) > MAX_LAG:
                continue
            sc = scenario_of(p["tag"], float(p.get("turn", 0)), float(p.get("hand_turn", 0) or 0))
            if sc:
                pg.setdefault((weapon_name(p), sc), []).append(p)
        for (wp, sc), ps in sorted(pg.items(), key=lambda kv: (kv[0][0], ["still", "turn", "sweep60", "sweep180"].index(kv[0][1]))):
            ang, perp = [], []
            for p in ps:
                mo, md = vec(p["drawn_o"]), unit(vec(p["drawn_d"]))
                o, d = vec(p["o"]), unit(vec(p["d"]))
                ang.append(angle_deg(d, md))
                perp.append(norm(cross(sub(o, mo), md)) * p["unit_m"] * 100)
            w("%-12s %-9s %5d launches: direction %s deg, launch point off the line %s cm (mean/p95/max)" % (
                wp, sc, len(ps), fmt3(stats(ang), 3), fmt3(stats(perp), 2)))
    if meshes:
        w("")
        w("Mesh check (last per weapon; thresholds %.1f deg, %.1f cm across the axis)" % (MESH_MAX_DEG, MESH_MAX_CM))
        last = {}
        for m in meshes:
            if m.get("source") == "builtin":
                last[m.get("key")] = m
        for wp in weapons:
            m = last.get(wp)
            if not m:
                continue
            perp_ok = float(m.get("perp_cm", "inf")) <= MESH_MAX_CM
            if m.get("axis") == "1":
                v = "PASS" if float(m["angle_deg"]) <= MESH_MAX_DEG and perp_ok else "FAIL"
            elif m.get("muzzle") == "1":
                v = "muzzle only: %s" % ("PASS" if perp_ok else "FAIL")
            else:
                v = "NO FIT (%s)" % m.get("why", "")
            w("%-12s axis %s, angle %s deg, muzzle across %s cm, along %s cm, by %s: %s" % (
                wp, "yes" if m.get("axis") == "1" else "no", m.get("angle_deg"), m.get("perp_cm"), m.get("along_cm"), m.get("method"), v))
    return "\n".join(out)


# ---------------------------------------------------------------- self-test

def rotate(d, yaw_deg, pitch_deg):
    """d turned by yaw (+ right) and pitch (+ up) in its barrel frame."""
    right, up = barrel_frame(d)
    y, p = math.radians(yaw_deg), math.radians(pitch_deg)
    v = add(add(scale(d, math.cos(p) * math.cos(y)), scale(right, math.cos(p) * math.sin(y))), scale(up, math.sin(p)))
    return unit(v)


def synthetic_log():
    """Magnum: still shots 0.1 deg right of the drawn barrel; turning (150 deg/s) shots 2 deg low; sweeping
    (hand at 120 deg/s, rail still) shots 0.5 deg left.
    Pump: 6 pellets, 3 deg spread about a centre on the barrel. A crossbow launch and mesh lines."""
    rnd = random.Random(7)
    lines = ["[ 0.000] shotlog on frame=1 unit_m=0.01000"]
    sid = 0

    def shot(key, pellets, tag_turn, yaw, pitch, spread, hand=0.0):
        nonlocal sid
        sid += 1
        mo = [rnd.uniform(-50, 50), 150, rnd.uniform(-50, 50)]
        md = unit([rnd.uniform(-0.3, 0.3), rnd.uniform(-0.2, 0.1), 1])
        centre = rotate(md, yaw, pitch)
        frame = 100 + sid * 3
        lines.append("[ 1.000] shotlog shot id=%d frame=%d weapon=GunArms_X key=%s data=0x1 type=1 kind=2 pellets=%d range=20000 aim=hand "
                     "o=%.3f,%.3f,%.3f d=%.6f,%.6f,%.6f fire_o=%.3f,%.3f,%.3f fire_d=%.6f,%.6f,%.6f fire_turn=%.1f drawn=1 drawn_frame=%d "
                     "drawn_key=%s drawn_o=%.3f,%.3f,%.3f drawn_d=%.6f,%.6f,%.6f turn=%.1f turn_frame=%.1f hand_turn=%.1f barrel_turn=%.1f" % (
                         sid, frame, key, pellets, *mo, *md, *mo, *md, tag_turn, frame - 1, key, *mo, *md, tag_turn, tag_turn, hand, hand + tag_turn))
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
    lines.append("[ 2.500] shotlog tag name=sweep60 frame=550")
    for _ in range(10):
        shot("magnum", 1, 0.0, -0.5, 0.0, 0, hand=120.0)
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

    check(len(shots) == 36 and len(projs) == 1 and len(meshes) == 2, "parsed counts")
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
    check(all(abs(r["dist_m"][0] - 10.0) < 1e-3 for r in mag_still), "impact distance 10 m")
    # The summary: tag "still" (rail 0) -> still, "turning" at 150 deg/s -> turn; the sweep shots (hand
    # at 120 deg/s, 0.5 deg left) -> sweep60.
    sm = summary(shots, projs, meshes)
    still_row = [l for l in sm.splitlines() if l.startswith("magnum") and " still " in l]
    turn_row = [l for l in sm.splitlines() if l.startswith("magnum") and " turn " in l]
    sweep_row = [l for l in sm.splitlines() if l.startswith("magnum") and " sweep60 " in l]
    check(len(still_row) == 1 and " 0.100 " in still_row[0] and still_row[0].rstrip().endswith(("PASS", "(PASS)")), "summary still row")
    check(len(turn_row) == 1 and " 2.000 " in turn_row[0], "summary turn row")
    check(len(sweep_row) == 1 and " 0.500 " in sweep_row[0] and " 0/120 " in sweep_row[0], "summary sweep row")
    check("100-300: 10 shots 2.000 deg" in sm, "summary turn bins")
    check("hand's angular speed" in text, "hand speed table")
    check(all("first" in r and abs(r["first"]["centre"] - 3.0) < 0.01 for r in pump), "pump first pellet (pellet 0, 3 deg out here)")
    print(sm)
    print()
    print(text)
    print()
    if fails:
        print("SELFTEST FAILED: " + "; ".join(fails))
        return 1
    print("selftest passed")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log", nargs="*", help="one or more logs (each its own game run: shot ids restart)")
    ap.add_argument("--out")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--summary", action="store_true", help="one table per weapon and scenario (still, turn, sweep) first")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.log:
        ap.error("a log file or --selftest")
    shots, projs, meshes, overrides = [], [], [], []
    for path in a.log:
        with open(path, encoding="utf-8", errors="replace") as f:
            for acc, part in zip((shots, projs, meshes, overrides), parse(f)):
                acc.extend(part)
    text = report(shots, projs, meshes, overrides)
    if a.summary:
        text = summary(shots, projs, meshes) + "\n\n" + text
    print(text)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
