#!/usr/bin/env python3
"""
Qt dashboard 到 Python RAG 分析器的桥接脚本。
用于从 Qt 进程调用本地 RAG 分析，并返回纯文本结果。
"""

from __future__ import annotations

import argparse
import contextlib
import io
import os
import sys


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

os.chdir(SCRIPT_DIR)

try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="WeakNet Qt RAG bridge")
    subparsers = parser.add_subparsers(dest="command", required=True)

    analyze_parser = subparsers.add_parser("analyze", help="Analyze one time point from a log file")
    analyze_parser.add_argument("--log-file", required=True, help="Path to the log text file")
    analyze_parser.add_argument("--time-point", required=True, help="Target time point, format HH:MM:SS")
    analyze_parser.add_argument("--api-key", default="", help="Optional DashScope API key")

    return parser.parse_args()


def read_log_text(log_file: str) -> str:
    with open(log_file, "r", encoding="utf-8") as handle:
        return handle.read()


def normalize_api_key(value: str) -> str:
    if value.strip():
        return value.strip()
    return os.getenv("DASHSCOPE_API_KEY", "").strip()


def analyze(log_text: str, time_point: str, api_key: str) -> str:
    runtime_output = io.StringIO()

    with contextlib.redirect_stdout(runtime_output), contextlib.redirect_stderr(runtime_output):
        from local_vector_rag_analyzer import LocalVectorRAGAnalyzer

        analyzer = LocalVectorRAGAnalyzer(api_key)
        if not api_key:
            analyzer.client = None
        result = analyzer.analyze_time_point(log_text, time_point)

    runtime_notes = runtime_output.getvalue().strip()
    if runtime_notes:
        print(runtime_notes, file=sys.stderr)

    return result


def main() -> int:
    args = parse_args()

    if args.command != "analyze":
        print("Unsupported command", file=sys.stderr)
        return 2

    try:
        log_text = read_log_text(args.log_file)
        api_key = normalize_api_key(args.api_key)
        result = analyze(log_text, args.time_point, api_key)
        print(result)
        return 0
    except FileNotFoundError as exc:
        print(f"Log file not found: {exc}", file=sys.stderr)
        return 1
    except Exception as exc:
        print(f"RAG bridge failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
