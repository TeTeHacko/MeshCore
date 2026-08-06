#!/usr/bin/env python3
"""A/B current measurement on a bench node, via the UC96 meter's own exporter.

  tools/power_ab.py --serial 85D35458CC870126 --minutes 30 \
      --cell "baseline=" --cell "ps=powersaving on" --cell "duty=gps duty"

Why the exporter and not the meter: `uc96-exporter.service` owns the BLE link to
the UC96. A second central connecting to it takes the link away and the service
reconnect-loops, so this reads http://localhost:9877/metrics instead. Same data,
no contention. (It also lands in Mimir via alloy, so a run stays inspectable
afterwards -- but the exporter is scraped live, which is tighter for timing.)

**The instantaneous current field is useless here.** The UC96 reports current in
10 mA steps (`meter_current_amps` 0.02 for a node drawing anywhere from 15 to
24 mA), and the deltas worth chasing on a repeater are 1-4 mA. The accumulating
`meter_capacity_ah` counter has 1 mAh resolution, so this integrates that over a
window instead: avg mA = 60 * delta_mAh / minutes. The +-1 mAh quantisation is
printed with every cell as an explicit error bar -- at 20 minutes that is
+-3 mA, which would swallow the whole effect, so short windows are a trap and
the tool says so rather than letting you read noise as a result.

Two estimators are reported per cell, because the endpoint one alone wastes the
window: `fit` is a least-squares slope through the whole sampled staircase, and
it beats the endpoint difference by using WHEN each 1 mAh step landed instead of
only how many landed. Its residual spread is printed too -- a cell that drifted
(traffic burst, charging, a hand near the antenna) shows up there rather than
quietly biasing the number. When the two estimators disagree by more than the
error bar, do not average them: the cell was not stationary, so re-run it.

Airtime counters are recorded per cell as CONTEXT, not as the claim: a cell that
happened to relay a flood burns more than an idle one, and without that column a
polluted cell looks like a real regression.
"""

import argparse
import glob
import json
import re
import sys
import time
import urllib.request

import serial

EXPORTER = "http://localhost:9877/metrics"
SETTLE_DEFAULT = 60


def meter_sample(url, meter=None):
    """One reading of the exporter: (capacity_mAh, volts, amps, unix_ts)."""
    with urllib.request.urlopen(url, timeout=10) as r:
        body = r.read().decode()
    out = {}
    for name in ("meter_capacity_ah", "meter_voltage_volts", "meter_current_amps",
                 "meter_last_seen_timestamp_seconds", "meter_up"):
        for line in body.splitlines():
            if not line.startswith(name + "{"):
                continue
            if meter and f'meter="{meter}"' not in line:
                continue
            out[name] = float(line.rsplit(" ", 1)[1])
            break
    missing = [k for k in ("meter_capacity_ah", "meter_up") if k not in out]
    if missing:
        raise SystemExit(f"exporter neposkytl {missing} (jiny meter? --meter <mac bez dvojtecek>)")
    if out["meter_up"] != 1.0:
        raise SystemExit("meter_up=0 -- exporter nema spojeni s meraakem, meritko by lhalo")
    # Frames must be arriving, not just the link be up: a stale series is a
    # straight line, and a straight line reads like a perfectly stable load.
    age = time.time() - out.get("meter_last_seen_timestamp_seconds", 0.0)
    if age > 30:
        raise SystemExit(f"posledni ramec pred {age:.0f} s -- serie je zamrzla, ne stabilni")
    return (out["meter_capacity_ah"] * 1000.0, out.get("meter_voltage_volts", 0.0),
            out.get("meter_current_amps", 0.0), out.get("meter_last_seen_timestamp_seconds", 0.0))


def fit_slope_ma(series):
    """Least-squares mA through (seconds, mAh) samples: (mA, residual, std err).

    The counter is a 1 mAh staircase, so the slope carries the timing of every
    step, not just the count at the ends. Residuals are returned in mAh: on a
    stationary cell they stay under ~0.6 (half a step); much more than that means
    the load moved during the window and the average is hiding something.

    CAVEAT on the standard error: it assumes independent residuals, and a
    quantised staircase violates that (consecutive samples sit on the same step),
    so it reads optimistic. It is still the right order of magnitude, and it is
    the honest one to compare deltas against -- unlike the endpoint quantisation,
    which the fit is specifically there to beat. Run one cell twice to get the
    empirical repeatability; that number outranks this estimate.
    """
    n = len(series)
    if n < 4:
        return None, None, None
    tx = sum(t for t, _ in series) / n
    ty = sum(c for _, c in series) / n
    sxx = sum((t - tx) ** 2 for t, _ in series)
    if sxx == 0:
        return None, None, None
    sxy = sum((t - tx) * (c - ty) for t, c in series)
    slope = sxy / sxx                      # mAh per second
    sse = sum((c - (ty + slope * (t - tx))) ** 2 for t, c in series)
    resid = (sse / n) ** 0.5
    se = ((sse / (n - 2)) / sxx) ** 0.5    # mAh/s
    return slope * 3600.0, resid, se * 3600.0


class Console:
    """Text console of a repeater/analyzer build over USB CDC.

    dtr/rts + the settle delay are not optional: nRF52 TinyUSB keeps the CDC
    mute until DTR is asserted, and a board opened without it looks dead.
    """

    LOST = "<console lost>"

    def __init__(self, port_glob):
        self.port_glob = port_glob
        self.s = None
        self._open()

    def _open(self):
        path = glob.glob(self.port_glob)
        if not path:
            raise SystemExit(f"port {self.port_glob} nenalezen")
        self.s = serial.Serial(path[0], 115200, timeout=0)
        self.s.dtr = True
        self.s.rts = True
        time.sleep(1.3)
        self.s.reset_input_buffer()

    def reopen(self, wait=90.0):
        """Wait for the board to come back on USB and re-open it.

        These boards drop off USB on their own (documented flakiness) and return
        seconds to minutes later. Losing the console mid-run must not force a
        rerun of an hour-long matrix.
        """
        try:
            if self.s:
                self.s.close()
        except Exception:
            pass
        self.s = None
        t0 = time.time()
        while time.time() - t0 < wait:
            if glob.glob(self.port_glob):
                try:
                    self._open()
                    return True
                except (OSError, serial.SerialException, SystemExit):
                    pass
            time.sleep(3)
        return False

    def cmd(self, c, timeout=4.0, quiet=0.3):
        """Send a command. On a console drop, re-open once and retry.

        Returns LOST if the console could not be recovered. Callers that are
        APPLYING a cell setting must treat that as a failed cell — measuring a
        treatment that was never applied is worse than no measurement, and it
        happened: `gps off` was lost on the wire, the run carried on, and two
        cells silently measured `gps on` again.
        """
        try:
            return self._cmd(c, timeout, quiet)
        except (OSError, serial.SerialException):
            pass
        if self.reopen():
            try:
                return self._cmd(c, timeout, quiet)
            except (OSError, serial.SerialException):
                pass
        return self.LOST

    def _cmd(self, c, timeout, quiet):
        if self.s is None:
            raise serial.SerialException("console not open")
        self.s.write((c + "\r").encode())
        buf, last, t0 = b"", time.time(), time.time()
        while time.time() - t0 < timeout:
            d = self.s.read(256)
            if d:
                buf += d
                last = time.time()
            elif buf and time.time() - last > quiet:
                break
            else:
                time.sleep(0.02)
        txt = buf.decode(errors="replace")
        # echo of the command comes back first; keep the answer only
        lines = [l.strip() for l in txt.replace("\r", "\n").split("\n") if l.strip()]
        lines = [l for l in lines if l != c]
        return " | ".join(l.lstrip("> ").strip() for l in lines)

    def air(self):
        m = re.search(r'"tx_air_secs":(\d+).*?"rx_air_secs":(\d+)', self.cmd("stats-radio"))
        return (int(m.group(1)), int(m.group(2))) if m else (None, None)

    def close(self):
        try:
            if self.s:
                self.s.close()
        except Exception:
            pass


# What a cell's setting command must end up reporting, so "applied" is verified
# on the node rather than assumed from a write that returned no error. Key is the
# command prefix, value is (query, expected substring in the answer).
VERIFY = {
    "gps off": ("gps", "off"),
    "gps on": ("gps", "on,"),
    "gps duty": ("gps", "duty"),
    "powersaving on": ("powersaving", "on"),
    "powersaving off": ("powersaving", "off"),
    "ble on": ("ble", "on"),
    "ble off": ("ble", "off"),
}

# Queried at the start of every cell: the record of what the node actually was,
# and the raw material for VERIFY above.
STATE_QUERIES = ("powersaving", "gps", "ble", "get repeat")


def port_glob_for(serial_no):
    g = glob.glob(f"/dev/serial/by-id/*{serial_no}*")
    if not g:
        raise SystemExit(f"port pro seriak {serial_no} nenalezen v /dev/serial/by-id/")
    return f"/dev/serial/by-id/*{serial_no}*"


def main():
    ap = argparse.ArgumentParser(description="A/B mereni proudu pres UC96 exporter")
    ap.add_argument("--serial", help="seriove cislo desky (matchuje se v /dev/serial/by-id/)")
    ap.add_argument("--port", help="misto --serial: pevna cesta k portu")
    ap.add_argument("--cell", action="append", default=[], metavar="NAME=CMD[;CMD]",
                    help="jedna bunka: jmeno a prikazy, ktere ji nastavi (prazdne = jen mer)")
    ap.add_argument("--minutes", type=float, default=30.0, help="delka okna bunky (vychozi 30)")
    ap.add_argument("--settle", type=float, default=SETTLE_DEFAULT,
                    help="prodleva po aplikaci prikazu pred zacatkem okna (s, vychozi 60)")
    ap.add_argument("--poll", type=float, default=10.0,
                    help="jak casto vzorkovat citac kapacity v okne (s, vychozi 10)")
    ap.add_argument("--meter", help="MAC merace bez dvojtecek, kdyz exporter vidi vic meraku")
    ap.add_argument("--url", default=EXPORTER)
    ap.add_argument("--json", help="ulozit vysledek jako JSON")
    args = ap.parse_args()

    if not args.cell:
        raise SystemExit("aspon jedna --cell")

    err_ma = 60.0 / args.minutes  # +-1 mAh quantisation over the window
    print(f"okno {args.minutes:g} min => kvantizace merice je +-{err_ma:.2f} mA na bunku")
    if err_ma > 1.5:
        print("  POZOR: to je vic, nez kolik ceka vetsina uspor. Delsi --minutes, nebo "
              "cti vysledek jako 'nezmereno'.")

    con = None
    if args.serial or args.port:
        con = Console(args.port or port_glob_for(args.serial))
        print(f"deska: {con.cmd('ver')} / {con.cmd('get name')}")

    results = []
    try:
        for spec in args.cell:
            name, _, cmds = spec.partition("=")
            name = name.strip() or "cell"
            cmds = [c.strip() for c in cmds.split(";") if c.strip()]
            print(f"\n=== bunka {name}")

            applied = True
            for c in cmds:
                if not con:
                    raise SystemExit("bunka ma prikazy, ale neni zadana deska (--serial)")
                reply = con.cmd(c)
                print(f"    {c} -> {reply}")
                if reply == Console.LOST or "Unknown command" in reply or "rror" in reply:
                    applied = False
            if cmds:
                print(f"    usazovani {args.settle:g} s")
                time.sleep(args.settle)

            state = {}
            tx0 = rx0 = None
            if con:
                for q in STATE_QUERIES:
                    state[q] = con.cmd(q)
                tx0, rx0 = con.air()

            # Verify on the node that the setting took, then refuse to measure a
            # treatment that is not actually in place. The alternative -- what
            # this tool did before -- is a plausible-looking number for a state
            # the node was never in.
            for c in cmds:
                for prefix, (query, expect) in VERIFY.items():
                    if c.startswith(prefix):
                        answer = state.get(query, "")
                        if expect not in answer:
                            print(f"    NEOVERENO: po '{c}' hlasi '{query}' -> "
                                  f"'{answer}' (cekano '{expect}')")
                            applied = False
            if not applied:
                print("    CHYBA: zasah se neaplikoval/neoveril -- bunka se NEMERI "
                      "(merit neaplikovany zasah je horsi nez nemerit)")
                results.append({"cell": name, "cmds": cmds, "avg_ma": None, "fit_ma": None,
                                "resid_mah": None, "se_ma": None, "delta_mah": None,
                                "minutes": 0.0, "err_ma": err_ma, "volts": None,
                                "state": state, "tx_air_delta": None, "rx_air_delta": None,
                                "samples": 0, "invalid": "neaplikovano"})
                continue
            cap0, v0, _, _ = meter_sample(args.url, args.meter)
            t0 = time.time()
            print(f"    start: cap={cap0:.0f} mAh, U={v0:.2f} V, stav={state}")

            series = [(0.0, cap0)]
            deadline = t0 + args.minutes * 60
            # Heartbeat once a minute. Without it a healthy cell and a hung
            # process look identical for ten minutes at a stretch -- which is
            # exactly how a good run got killed two minutes before it finished.
            next_beat = t0 + 60
            while time.time() < deadline:
                time.sleep(min(args.poll, max(0.0, deadline - time.time())))
                cap, v1, i1, _ = meter_sample(args.url, args.meter)
                now = time.time()
                series.append((now - t0, cap))
                if now >= next_beat:
                    next_beat += 60
                    el = (now - t0) / 60.0
                    print(f"    ... {el:.0f}/{args.minutes:g} min, +{cap-cap0:.0f} mAh, "
                          f"zatim {(cap-cap0)/((now-t0)/3600.0):.1f} mA", flush=True)

            cap1, v1, i1, _ = meter_sample(args.url, args.meter)
            series.append((time.time() - t0, cap1))
            dt_h = (time.time() - t0) / 3600.0
            tx1, rx1 = con.air() if con else (None, None)
            dcap = cap1 - cap0
            fit_ma = resid = se_ma = None
            if dcap < 0:
                print("    CHYBA: citac kapacity klesl (reset merice) -- bunka je neplatna")
                avg = None
            elif dcap == 0:
                # An instrument reporting a perfectly flat counter is not
                # measuring a small load, it is not measuring. Reporting
                # "0.00 +-0.00 mA" as a result once sent a whole session chasing
                # the wrong thing: the same board read 22 mA on another meter
                # while this one sat at zero for 15 minutes. Refuse the cell.
                print(f"    CHYBA: citac kapacity se za {dt_h*60:.0f} min NEHNUL "
                      f"(instant {i1*1000:.0f} mA). Merak tuhle zatez nemeri -- "
                      f"nula NENI vysledek. Zkus jiny merak nebo vetsi zatez.")
                avg = None
            else:
                avg = dcap / dt_h
                fit_ma, resid, se_ma = fit_slope_ma(series)
                print(f"    konec: cap={cap1:.0f} mAh, delta={dcap:.0f} mAh za {dt_h*60:.1f} min, "
                      f"{len(series)} vzorku")
                print(f"    ==> endpoint {avg:.2f} +-{err_ma:.2f} mA | fit {fit_ma:.2f} "
                      f"+-{se_ma:.2f} mA (rezidua {resid:.2f} mAh) @ {v1:.2f} V "
                      f"(instant {i1*1000:.0f} mA, 10mA krok)")
                if abs(fit_ma - avg) > err_ma:
                    print("    POZOR: estimatory se rozchazi vic nez chyba -- bunka nebyla "
                          "stacionarni, zopakuj ji")
            # stats-radio can fail to parse on either end (busy console); keep the
            # cell valid and just report the airtime column as unknown.
            d_tx = tx1 - tx0 if (tx0 is not None and tx1 is not None) else None
            d_rx = rx1 - rx0 if (rx0 is not None and rx1 is not None) else None
            if d_tx is not None:
                print(f"    airtime v okne: tx +{d_tx} s, rx +{d_rx} s")
            results.append({"cell": name, "cmds": cmds, "avg_ma": avg, "fit_ma": fit_ma,
                            "resid_mah": resid, "se_ma": se_ma, "delta_mah": dcap,
                            "minutes": dt_h * 60, "err_ma": err_ma, "volts": v1,
                            "state": state, "tx_air_delta": d_tx, "rx_air_delta": d_rx,
                            "samples": len(series)})
    finally:
        if con:
            con.close()

    summarize(results)

    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=2)
        print(f"\nJSON -> {args.json}")
    return 0


def summarize(results):
    """Print the summary. Separate from main() so a finished run's JSON can be
    re-analysed (and the analysis tested) without touching hardware."""
    print("\n===== SOUHRN ===== (fit je presnejsi estimator, endpoint v zavorce)")
    valid = [r for r in results if r["fit_ma"] is not None]
    base = valid[0] if valid else None

    # Spread across repeats of the same cell is repeatability ONLY if the rig is
    # stationary. It is not, when something drifts underneath (a charging cell
    # tapering: `gps on` cells went 107 -> 92 -> 88 -> 84 mA across one hour).
    # Using that spread as a noise floor then buries the very effect being
    # measured -- it flagged a real 46 mA GPS delta as "v sumu".
    floor = None
    groups = {}
    for r in valid:
        groups.setdefault(tuple(r["cmds"]), []).append(r["fit_ma"])
    for cmds, vals in groups.items():
        if len(vals) > 1:
            spread = max(vals) - min(vals)
            trend = abs(vals[-1] - vals[0])
            drifting = trend > 0.6 * spread   # monotone-ish: most of the spread is trend
            label = "drift, NE sum" if drifting else "opakovatelnost"
            print(f"  {label} ({'+'.join(cmds) or 'bez prikazu'}): rozptyl {spread:.2f} mA "
                  f"pres {len(vals)} bunky (kraje se lisi o {trend:.2f})")
            if not drifting:
                floor = spread if floor is None else max(floor, spread)

    # Alternating designs (A B A B ...) are the answer to a drifting rig, so
    # analyse them as such: each B against the MEAN OF ITS TWO NEIGHBOURING A
    # cells, which cancels a linear trend. This is the number to quote.
    sigs = [tuple(r["cmds"]) for r in valid]
    distinct = list(dict.fromkeys(sigs))
    if len(distinct) == 2 and len(valid) >= 3:
        a_sig, b_sig = distinct
        deltas = []
        for i, r in enumerate(valid):
            if tuple(r["cmds"]) != b_sig:
                continue
            before = next((v["fit_ma"] for v in reversed(valid[:i])
                           if tuple(v["cmds"]) == a_sig), None)
            after = next((v["fit_ma"] for v in valid[i + 1:]
                          if tuple(v["cmds"]) == a_sig), None)
            flank = [x for x in (before, after) if x is not None]
            if flank:
                deltas.append(sum(flank) / len(flank) - r["fit_ma"])
        if deltas:
            mean = sum(deltas) / len(deltas)
            print(f"\n  PAROVY ODHAD ({'+'.join(a_sig) or 'bez prikazu'} minus "
                  f"{'+'.join(b_sig) or 'bez prikazu'}): {mean:.2f} mA "
                  f"(z {len(deltas)} paru, rozptyl {min(deltas):.2f}-{max(deltas):.2f}) "
                  f"-- drift vykracen, tohle je cislo k citovani")

    for r in valid:
        d = ""
        if base and r is not base:
            diff = r["fit_ma"] - base["fit_ma"]
            # Significance against the measured repeatability when we have it,
            # otherwise against twice the fit's own standard error.
            lim = floor if floor is not None else 2 * (r["se_ma"] or 0)
            sig = "" if abs(diff) > lim else "  (v sumu!)"
            d = f"   delta vs {base['cell']}: {diff:+.2f} mA{sig}"
        print(f"  {r['cell']:12s} {r['fit_ma']:6.2f} +-{r['se_ma']:.2f} mA  "
              f"(endpoint {r['avg_ma']:.2f} +-{r['err_ma']:.2f}){d}")
    for r in results:
        if r["fit_ma"] is None:
            print(f"  {r['cell']:12s} neplatna ({r.get('invalid', 'merak nemeril')})")


if __name__ == "__main__":
    # Re-analyse a finished run: power_ab.py --reanalyse run.json
    if len(sys.argv) == 3 and sys.argv[1] == "--reanalyse":
        with open(sys.argv[2]) as f:
            summarize(json.load(f))
        sys.exit(0)
    sys.exit(main())
