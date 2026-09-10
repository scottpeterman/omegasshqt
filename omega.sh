#!/bin/sh
# Run from the repo root.
echo "=== 1. does main.cpp.o actually define ::main? ==="
O=build/app/CMakeFiles/omega.dir/main.cpp.o
if [ -f "$O" ]; then
  nm "$O" | grep -i main || echo "  (no main symbol at all)"
else
  echo "  $O not found"
fi

echo
echo "=== 2. is 'main' inside a namespace? (brace balance) ==="
for f in app/main.cpp app/mainwindow.h app/settings.h theme/theme.h; do
  [ -f "$f" ] || { echo "  MISSING $f"; continue; }
  python3 - "$f" <<'PY'
import sys,re
p=sys.argv[1]; s=open(p).read()
s=re.sub(r'//[^\n]*','',s); s=re.sub(r'/\*.*?\*/','',s,flags=re.S)
s=re.sub(r'"(\\.|[^"\\])*"','""',s); s=re.sub(r"'(\\.|[^'\\])*'","''",s)
print(f"  {p:24} open={s.count('{')} close={s.count('}')} "
      f"{'OK' if s.count('{')==s.count('}') else '<-- UNBALANCED'}")
PY
done

echo
echo "=== 3. the closing brace before main() in app/main.cpp ==="
grep -n "namespace\|^int main" app/main.cpp

echo
echo "=== 4. what is locally modified ==="
git status --short