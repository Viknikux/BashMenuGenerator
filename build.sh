#!/usr/bin/env bash

echo "Compiling BashMenuGenerator..."

if g++ -O2 -static-libgcc -static-libstdc++ BashMenuGenerator.cpp -o BashMenuGenerator; then
    echo "========================================"
    echo "  [SUCCESS] BashMenuGenerator built!"
    echo "========================================"
else
    echo "========================================"
    echo "  [ERROR] Build failed! Check code errors."
    echo "========================================"
fi