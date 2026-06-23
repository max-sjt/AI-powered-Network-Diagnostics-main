#!/bin/bash
set -e

cd "$(dirname "$0")"
export PYTHONPATH="$PWD/.python-packages${PYTHONPATH:+:$PYTHONPATH}"
python3 local_vector_rag_analyzer.py "$@"
