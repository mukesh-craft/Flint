#!/usr/bin/env python3
"""A3: render flintc_prof JSON (profile_report.json) as folded stacks + SVG.

The Timer now records each phase's stack parent, so the tree rebuilds
exactly (flat wall times alone cannot nest phases). Old JSON without
"parent" degrades to a flat root list instead of failing.

Usage:
  ./flintc_prof prog.fl -o prog        # writes profile_report.json
  python3 tools/flamegraph.py profile_report.json flame.svg
  python3 tools/flamegraph.py profile_report.json --folded

Only stdlib; no new dependencies.
"""
import json
import sys


def load_tree(path):
    doc = json.load(open(path))
    phases = doc.get("phases", [])
    total = doc.get("total_ns", 0)
    nodes = [{"name": p.get("phase", "?"), "wall": p.get("wall_ns", 0),
              "parent": p.get("parent", -1), "children": []}
             for p in phases]
    roots = []
    for i, n in enumerate(nodes):
        par = n["parent"]
        if isinstance(par, int) and 0 <= par < len(nodes) and par != i:
            nodes[par]["children"].append(i)
        else:
            roots.append(i)
    return total, nodes, roots


def folded(total, nodes, roots):
    lines = []

    def walk(idx, stack):
        n = nodes[idx]
        stack = stack + [n["name"]]
        lines.append((";".join(stack), n["wall"]))
        for c in n["children"]:
            walk(c, stack)

    for r in roots:
        walk(r, [])
    return lines


def color(name):
    h = sum(ord(c) * (i + 1) for i, c in enumerate(name))
    r = 200 + h % 55
    g = 60 + (h // 7) % 120
    b = 40 + (h // 13) % 60
    return "#%02x%02x%02x" % (r, g, b)


def svg(total, nodes, roots, path):
    W, ROW_H, PAD = 1200, 22, 10
    rows = []

    def layout(idx, depth):
        while len(rows) <= depth:
            rows.append([])
        rows[depth].append(idx)
        for c in nodes[idx]["children"]:
            layout(c, depth + 1)

    for r in roots:
        layout(r, 0)
    H = PAD * 2 + ROW_H * max(1, len(rows)) + 30
    scale = (W - PAD * 2) / total if total > 0 else 0
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" '
             'height="%d" font-family="monospace" font-size="12">' % (W, H)]
    parts.append('<text x="%d" y="20">flintc profile (total %d ns)</text>'
                 % (PAD, total))

    def draw(idx, depth, x):
        n = nodes[idx]
        w = n["wall"] * scale
        y = 30 + depth * ROW_H
        label = "%s (%d ns)" % (n["name"], n["wall"])
        parts.append('<g><title>%s</title>' % label)
        parts.append('<rect x="%.1f" y="%d" width="%.1f" height="%d" '
                     'fill="%s" stroke="white"/>' % (x, y, w, ROW_H - 2,
                                                     color(n["name"])))
        if w > 60:
            parts.append('<text x="%.1f" y="%d">%s</text>'
                         % (x + 4, y + 15, n["name"][: int(w // 7)]))
        parts.append('</g>')
        cx = x
        for c in n["children"]:
            cw = nodes[c]["wall"] * scale
            draw(c, depth + 1, cx)
            cx += cw

    # Roots share row 0 proportionally (children tile beneath, may exceed
    # the parent when phases overlap — wall times are not exclusive).
    x = PAD
    for r in roots:
        w = nodes[r]["wall"] * scale
        draw(r, 0, x)
        if not nodes[r]["children"]:
            x += w
    parts.append('</svg>')
    open(path, "w").write("\n".join(parts) + "\n")


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print("usage: flamegraph.py profile_report.json [out.svg | --folded]")
        return 1
    total, nodes, roots = load_tree(argv[1])
    if len(argv) >= 3 and argv[2] == "--folded":
        for stack, wall in folded(total, nodes, roots):
            print("%d %s" % (wall, stack))
    else:
        out = argv[2] if len(argv) >= 3 else "flame.svg"
        svg(total, nodes, roots, out)
        print("wrote %s (%d phases, total %d ns)" % (out, len(nodes), total))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
