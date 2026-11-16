#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build

CXX=x86_64-w64-mingw32-g++
STRIP=x86_64-w64-mingw32-strip
OPENVR_HEADER_URL="https://raw.githubusercontent.com/ValveSoftware/openvr/master/headers/openvr_driver.h"

if [ ! -f build/openvr_driver.h ]; then
    echo "==> Downloading openvr_driver.h"
    curl -fsSL "$OPENVR_HEADER_URL" -o build/openvr_driver.h
fi

echo "==> Building driver_fakehmd.dll"
$CXX -O2 -std=c++17 -shared -static -static-libgcc -static-libstdc++ \
    -Ibuild src/driver.cpp -o build/driver_fakehmd.dll
$STRIP build/driver_fakehmd.dll

echo "==> Embedding the driver"
python3 - <<'EOF'
d = open('build/driver_fakehmd.dll', 'rb').read()
with open('build/driver_dll.h', 'w') as f:
    f.write('static const unsigned char driver_fakehmd_dll[] = {\n')
    for i in range(0, len(d), 16):
        f.write(','.join(str(b) for b in d[i:i + 16]) + ',\n')
    f.write('};\nstatic const unsigned int driver_fakehmd_dll_len = %d;\n' % len(d))
EOF

echo "==> Building FakeVR.exe"
$CXX -O2 -std=c++17 -municode -mwindows -static -static-libgcc -static-libstdc++ \
    -Ibuild src/fakevr.cpp -o build/FakeVR.exe \
    -lole32 -luuid -lshell32 -lpsapi -ladvapi32
$STRIP build/FakeVR.exe

echo "==> Done: build/FakeVR.exe"
sha256sum build/FakeVR.exe
