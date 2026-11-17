# Copies the include closure of a few Boost headers out of an extracted Boost release.
# Usage: python -I boost_subset.py <boost_root> <dest> <root headers, comma separated> [whole dirs, comma separated]
# Every "#include" is followed whether or not it sits inside an #if, so the result is a superset.
# Libraries that include through macros (mpl, preprocessor, config) are copied whole when touched.
import os
import re
import shutil
import sys

src, dst = sys.argv[1], sys.argv[2]
roots = sys.argv[3].split(',')
whole = set(sys.argv[4].split(',')) if len(sys.argv) > 4 and sys.argv[4] else set()
inc = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)
BS = chr(92)
seen, todo, done_whole = set(), list(roots), set()


def add_dir(d):
    for dp, _, fs in os.walk(os.path.join(src, d)):
        for f in fs:
            todo.append(os.path.relpath(os.path.join(dp, f), src).replace(BS, '/'))


while todo:
    rel = todo.pop()
    if rel in seen or not os.path.isfile(os.path.join(src, rel)):
        continue
    seen.add(rel)
    parts = rel.split('/')
    if len(parts) > 2 and parts[0] == 'boost' and parts[1] in whole and parts[1] not in done_whole:
        done_whole.add(parts[1])
        add_dir('boost/' + parts[1])
    text = open(os.path.join(src, rel), encoding='utf-8', errors='replace').read()
    for kind, name in inc.findall(text):
        if name.startswith('boost/'):
            todo.append(name)
        if kind == '"':
            todo.append(os.path.normpath(os.path.join(os.path.dirname(rel), name)).replace(BS, '/'))

total = 0
for rel in sorted(seen):
    out = os.path.join(dst, rel)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    shutil.copyfile(os.path.join(src, rel), out)
    total += os.path.getsize(out)
print(len(seen), 'files,', total, 'bytes')
