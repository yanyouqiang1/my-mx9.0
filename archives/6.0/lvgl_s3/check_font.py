import re, sys

font = r"E:\ai-project\mx9.0\archives\6.0\lvgl_s3\src\lv_font_simsun_16_cjk.c"
ino  = r"E:\ai-project\mx9.0\archives\6.0\lvgl_s3\src\lvgl_s3.ino"

src = open(font, encoding="utf-8", errors="ignore").read()

# parse cmaps
cmaps = []
for m in re.finditer(r"range_start\s*=\s*(\d+).{0,4}range_length\s*=\s*(\d+).{0,4}glyph_id_start\s*=\s*(\d+)", src, re.S):
    cmaps.append(tuple(int(x) for x in m.groups()))

def find_list(name):
    m = re.search(re.escape(name) + r"\[\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        return None
    body = m.group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    return [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", body)]

lists = {}
for nm in ["unicode_list_1", "unicode_list_5", "glyph_id_ofs_list_4"]:
    v = find_list(nm)
    if v is not None:
        lists[nm] = v

def covered(cu):
    for (rs, rl, gs) in cmaps:
        if rs <= cu < rs + rl:
            if rl <= 128:
                return True  # dense tiny range
            # sparse lists
            for nm in ("unicode_list_1", "unicode_list_5"):
                if nm in lists:
                    if (cu - rs) in lists[nm]:
                        return True
            if "glyph_id_ofs_list_4" in lists and len(lists["glyph_id_ofs_list_4"]) == rl:
                return True
    return False

text = open(ino, encoding="utf-8", errors="ignore").read()
# only string literals
lits = re.findall(r'"((?:[^"\\]|\\.)*)"', text)
chars = set()
for s in lits:
    for ch in s:
        if ord(ch) > 0x2000:
            chars.add(ch)

print("cmaps:", cmaps)
for k, v in lists.items():
    print(k, "len", len(v), v[:8])
missing = sorted([c for c in chars if not covered(ord(c))])
print("CJK chars used:", len(chars))
print("MISSING (%d):" % len(missing), " ".join("%s(U+%04X)" % (c, ord(c)) for c in missing))
