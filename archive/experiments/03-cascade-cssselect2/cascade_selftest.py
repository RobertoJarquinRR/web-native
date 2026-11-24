#!/usr/bin/env python3
"""Extra semantic check: id > class > tag, later rule wins ties, !important wins.

The shared input has no conflicting declarations, so this synthetic case exercises
the cascade decisions that cssselect2's Matcher enables.
"""
import html5lib

from run import cascade, parse_css

HTML = ('<!doctype html><html><body>'
        '<p id="main" class="note">A</p><p class="note">B</p></body></html>')
CSS = """
p { color: #ff0000; background: #111111; }
.note { color: #0000ff; }
#main { color: #00ff00; }
p { background: #222222; }
.note { color: #ffff00; }
#main { color: #00ffff !important; }
"""
EXPECTED = {"main": ("#00ffff", "#222222"), None: ("#ffff00", "#222222")}

root = html5lib.parse(HTML, treebuilder="etree", namespaceHTMLElements=False)
root_wrapper, styles, _ = cascade(root, parse_css([CSS]))
ok = True
for element in root_wrapper.iter_subtree():
    if element.local_name != "p":
        continue
    got = (styles[element].get("color"), styles[element].get("background"))
    want = EXPECTED[element.id]
    passed = got == want
    ok = ok and passed
    print(f"  {'OK  ' if passed else 'FAIL'} p id={element.id!r} "
          f"color={got[0]} background={got[1]} (expected {want})")
print("PASS" if ok else "FAIL")
raise SystemExit(0 if ok else 1)
