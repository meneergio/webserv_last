#!/bin/bash
echo "Content-Type: text/plain"
echo ""
echo "=== ENV ==="
env | sort
echo "=== ARGS ==="
echo "$@"
echo "=== STDIN ==="
cat
