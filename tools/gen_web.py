#!/usr/bin/env python3
"""Erzeugt firmware/DruckerDashboardRP2350-MK1/web_index.h aus web/index.html (gzip-komprimiert)."""
import gzip, os, sys
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = os.path.join(root, "web", "index.html")
dst = os.path.join(root, "firmware", "DruckerDashboardRP2350-MK1", "web_index.h")
data = open(src, "rb").read()
gz = gzip.compress(data, compresslevel=9, mtime=0)
lines = []
for i in range(0, len(gz), 20):
    lines.append("  " + ",".join(str(b) for b in gz[i:i+20]) + ",")
with open(dst, "w") as f:
    f.write("// AUTOMATISCH ERZEUGT aus web/index.html durch tools/gen_web.py - nicht von Hand bearbeiten\n")
    f.write("#pragma once\n#include <stdint.h>\n#include <stddef.h>\n")
    f.write("static const uint8_t INDEX_HTML_GZ[] = {\n" + "\n".join(lines) + "\n};\n")
    f.write("static const size_t INDEX_HTML_GZ_LEN = %d;\n" % len(gz))
print("web_index.h: %d Bytes HTML -> %d Bytes gzip" % (len(data), len(gz)))
