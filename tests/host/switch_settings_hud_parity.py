#!/usr/bin/env python3
"""Fingerprint parity: the in-engine settings readout must print the hash that
the run-profile script computes for the same effective settings.

usage: switch_settings_hud_parity.py HUD_TEST_BINARY
Compares the key list, then the hash for every profile on two maps.
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "perf"))
import run_profiles as rp  # noqa: E402


def main() -> int:
    exe = sys.argv[1]
    data, inv = rp.load_profiles(), rp.load_inventory()
    keys = subprocess.run([exe, "--keys"], check=True, capture_output=True, text=True).stdout.split()
    want = rp.hash_keys(data)
    if sorted(keys, key=str.lower) != sorted(want, key=str.lower):
        print(f"FAIL: key list differs: engine-only {sorted(set(keys) - set(want))} tooling-only {sorted(set(want) - set(keys))}")
        return 1
    fails = checked = 0
    for profile in data["profiles"]:
        for mp in ("cargoship", "killhouse"):
            ex = rp.expand(data, profile, map_name=mp)
            eff = rp.expected_settings(data, ex["settings"], inv)
            python_hash = rp.settings_hash(eff, mp, want)
            feed = "".join(f"{k}={v}\n" for k, v in eff.items() if k in keys)
            engine = subprocess.run([exe, "--hash", mp], input=feed, check=True, capture_output=True,
                                    text=True).stdout.strip()
            checked += 1
            if engine != python_hash:
                fails += 1
                print(f"FAIL: profile {profile} map {mp}: engine {engine} python {python_hash}")
            else:
                print(f"settings_hud parity: {profile}/{mp} {engine}")
    print(f"settings_hud parity: {checked} fingerprints, {fails} mismatches")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
