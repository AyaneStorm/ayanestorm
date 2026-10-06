#!/usr/bin/env python3
"""
@file as_build_star_catalog.py
@author chanayane@firestorm
@brief Builds the AyaneStorm real-sky star catalogue binary from the
Celestial Data star set (Frohn & Hernangomez 2023, BSD 3-Clause,
https://doi.org/10.5281/zenodo.7561601), itself from d3-celestial.

Usage:
    python as_build_star_catalog.py <stars.14.min.geojson> <out.bin> [count]

Output (little endian), sorted by increasing magnitude (brightest first):
    char[8]  magic "ASSTAR01"
    uint32   star count
    uint32   reserved (0)
    count x {
        int16 x, y, z   unit direction in the J2000 equatorial frame * 32767
                        (x toward RA 0h, z toward the north celestial pole)
        int16 mag       apparent magnitude * 1000
        int16 bv        B-V color index * 1000, -32768 when unknown
    }
"""

import json
import math
import struct
import sys

MAGIC = b"ASSTAR01"
BV_UNKNOWN = -32768


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    src, dst = sys.argv[1], sys.argv[2]
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 20000

    with open(src, encoding="utf-8") as f:
        features = json.load(f)["features"]

    stars = []
    for feat in features:
        props = feat["properties"]
        mag = props.get("mag")
        if mag is None or mag >= 999:
            continue
        # GeoJSON longitude is RA in degrees (wrapped to [-180, 180]),
        # latitude is declination.
        ra_deg, dec_deg = feat["geometry"]["coordinates"][:2]
        stars.append((mag, ra_deg, dec_deg, props.get("bv")))

    stars.sort(key=lambda s: s[0])
    stars = stars[:count]

    with open(dst, "wb") as out:
        out.write(MAGIC)
        out.write(struct.pack("<II", len(stars), 0))
        for mag, ra_deg, dec_deg, bv in stars:
            ra = math.radians(ra_deg)
            dec = math.radians(dec_deg)
            x = math.cos(dec) * math.cos(ra)
            y = math.cos(dec) * math.sin(ra)
            z = math.sin(dec)
            bv_i = BV_UNKNOWN if bv is None or abs(bv) > 30 else round(bv * 1000)
            out.write(struct.pack("<hhhhh",
                                  round(x * 32767), round(y * 32767), round(z * 32767),
                                  round(mag * 1000), bv_i))

    print("wrote %d stars to %s (faintest mag %.2f)" % (len(stars), dst, stars[-1][0]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
