#!/usr/bin/env python3
"""WeakNet short-term web dashboard.

This service intentionally uses only the Python standard library. It tails the
WeakNet glog output, stores parsed metrics in SQLite, exposes JSON APIs, serves
the static dashboard, and streams status updates with Server-Sent Events.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import signal
import socket
import sqlite3
import subprocess
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timedelta
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_LOG = ROOT_DIR / "logs" / "server" / "runtime.log"
DEFAULT_DB = ROOT_DIR / "dashboard" / "weaknet_dashboard.sqlite3"
STATIC_DIR = ROOT_DIR / "dashboard" / "static"
DEFAULT_RTT_TARGET = "223.5.5.5"


def detect_default_gateway() -> dict[str, Any]:
    """Read Linux default gateway from /proc/net/route.

    The gateway address in /proc/net/route is stored as a little-endian hex
    value, so 0102A8C0 means 192.168.2.1.
    """
    route_path = Path("/proc/net/route")
    try:
        lines = route_path.read_text(encoding="utf-8").strip().splitlines()
    except OSError as exc:
        return {"ip": "", "interface": "", "reachable": None, "rtt_ms": None, "error": str(exc)}

    for line in lines[1:]:
        fields = line.split()
        if len(fields) < 4:
            continue
        iface, destination, gateway_hex, flags_hex = fields[:4]
        try:
            flags = int(flags_hex, 16)
        except ValueError:
            continue
        if destination != "00000000" or not (flags & 0x2):
            continue
        try:
            gateway_ip = socket.inet_ntoa(bytes.fromhex(gateway_hex)[::-1])
        except OSError:
            gateway_ip = ""
        return {"ip": gateway_ip, "interface": iface, "reachable": None, "rtt_ms": None, "error": ""}

    return {"ip": "", "interface": "", "reachable": None, "rtt_ms": None, "error": "No default gateway found"}


def ping_host_once(host: str, timeout_sec: int = 1) -> tuple[bool, float | None, str]:
    if not host:
        return False, None, "empty host"
    try:
        completed = subprocess.run(
            ["ping", "-c", "1", "-W", str(timeout_sec), host],
            capture_output=True,
            text=True,
            timeout=timeout_sec + 1,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return False, None, str(exc)

    output = f"{completed.stdout}\n{completed.stderr}"
    match = re.search(r"time[=<]([\d.]+)\s*ms", output)
    rtt = float(match.group(1)) if match else None
    return completed.returncode == 0, rtt, output.strip().splitlines()[-1] if output.strip() else ""


def read_iface_counters(interface: str) -> dict[str, int] | None:
    base = Path("/sys/class/net") / interface / "statistics"
    try:
        return {
            "bytes": int((base / "rx_bytes").read_text().strip()) + int((base / "tx_bytes").read_text().strip()),
            "packets": int((base / "rx_packets").read_text().strip()) + int((base / "tx_packets").read_text().strip()),
        }
    except (OSError, ValueError):
        return None


def read_tcp_counters() -> dict[str, int] | None:
    try:
        lines = Path("/proc/net/snmp").read_text(encoding="utf-8").splitlines()
    except OSError:
        return None

    for idx, line in enumerate(lines):
        if not line.startswith("Tcp:") or idx + 1 >= len(lines):
            continue
        header = line.split()[1:]
        values = lines[idx + 1].split()[1:]
        if len(header) != len(values) or not lines[idx + 1].startswith("Tcp:"):
            continue
        data = dict(zip(header, values))
        try:
            return {
                "out_segs": int(data.get("OutSegs", "0")),
                "retrans_segs": int(data.get("RetransSegs", "0")),
            }
        except ValueError:
            return None
    return None


def parse_bool(value: str) -> int | None:
    value = value.upper()
    if value in {"YES", "TRUE", "1"}:
        return 1
    if value in {"NO", "FALSE", "0"}:
        return 0
    return None


def parse_glog_timestamp(line: str) -> str:
    match = re.match(r"^[IWEF](\d{8})\s+(\d{2}:\d{2}:\d{2})(?:\.(\d+))?", line)
    if not match:
        return datetime.now().isoformat(timespec="seconds")

    date_part, time_part, micros = match.groups()
    micros = (micros or "0")[:6].ljust(6, "0")
    parsed = datetime.strptime(f"{date_part} {time_part}.{micros}", "%Y%m%d %H:%M:%S.%f")
    return parsed.isoformat(timespec="microseconds")


def line_hash(line: str) -> str:
    return hashlib.sha1(line.encode("utf-8", errors="ignore")).hexdigest()


@dataclass
class ParsedRecord:
    kind: str
    ts: str
    data: dict[str, Any]
    raw_line: str


class WeakNetLogParser:
    def __init__(self) -> None:
        self.patterns: list[tuple[str, re.Pattern[str]]] = [
            (
                "active",
                re.compile(
                    r"\b(?P<state>ACTIVE|INACTIVE): (?P<iface>\S+) \| RTT: (?P<rtt>-?\d+)ms "
                    r"\| Quality: (?P<quality>\d+) \| RSSI: (?P<rssi>-?\d+)dBm "
                    r"\| TCP Loss: (?P<loss>-?[\d.]+)% \((?P<loss_level>[^)]*)\)"
                    r"(?: \| Traffic: (?P<traffic>-?[\d.]+)MB/s, (?P<flows>\d+) flows, (?P<pps>\d+) pps)?"
                ),
            ),
            (
                "rtt",
                re.compile(
                    r"RTT_MONITOR: (?P<iface>\S+) \| RTT: (?P<rtt>-?\d+)ms "
                    r"\| Quality: (?P<quality>\d+) \| Using: (?P<using>\w+)"
                ),
            ),
            (
                "tcp",
                re.compile(
                    r"TCP_LOSS_MONITOR: interface=(?P<iface>\S+) rate=(?P<loss>[\d.]+)% "
                    r"delta_sent=(?P<sent>\d+) delta_retrans=(?P<retrans>\d+) level=(?P<level>\w+)"
                ),
            ),
            (
                "rssi",
                re.compile(
                    r"RSSI_MONITOR: (?P<iface>\S+) \| RSSI: (?P<rssi>-?\d+)dBm "
                    r"\| Quality: (?P<quality>\d+) \| Using: (?P<using>\w+)"
                ),
            ),
            (
                "traffic",
                re.compile(
                    r"TRAFFIC_MONITOR: Total=(?P<traffic>-?[\d.]+)MB/s, Flows=(?P<flows>\d+), "
                    r"PPS=(?P<pps>\d+), Interface=(?P<iface>\S+)"
                ),
            ),
            (
                "quality",
                re.compile(
                    r"网络质量(?:变化|稳定): (?P<level>[A-Z]+) \(分数: (?P<score>[\d.]+)"
                    r"(?:, 接口: (?P<iface>[^)]+))?\)"
                ),
            ),
            (
                "event_signal",
                re.compile(
                    r"emitted signal: (?P<event>\w+), message='(?P<message>[^']*)', counter=(?P<counter>-?\d+)"
                ),
            ),
            (
                "event_emit",
                re.compile(
                    r"emitting event: type=(?P<type>\d+), message='(?P<message>[^']*)', source='(?P<source>[^']*)'"
                ),
            ),
            (
                "anomaly",
                re.compile(r"Anomaly: (?P<atype>\w+) .* severity=(?P<severity>[\d.]+)"),
            ),
            (
                "emoji_rtt",
                re.compile(
                    r"RTT监控: (?P<iface>\w+) = (?P<rtt>\d+)ms \(质量:(?P<quality>\d+), 使用:(?P<using>\w+)"
                ),
            ),
            (
                "emoji_tcp",
                re.compile(
                    r"TCP详细: (?P<iface>\w+) = (?P<loss>[\d.]+)% \(发送:(?P<sent>\d+), 重传:(?P<retrans>\d+), 等级:(?P<level>\w+)\)"
                ),
            ),
            (
                "emoji_traffic",
                re.compile(
                    r"流量监控: (?P<iface>\w+) = (?P<traffic>[\d.]+)MB/s \(连接:(?P<flows>\d+), 包/秒:(?P<pps>\d+)\)"
                ),
            ),
            (
                "emoji_rssi",
                re.compile(
                    r"RSSI监控: (?P<iface>\w+) = (?P<rssi>-?\d+)dBm \(质量:(?P<quality>\d+), 使用:(?P<using>\w+)\)"
                ),
            ),
            (
                "emoji_summary",
                re.compile(
                    r"接口汇总: (?P<iface>\w+) = RTT:(?P<rtt>-?\d+)ms, 质量:(?P<quality>\d+), "
                    r"RSSI:(?P<rssi>-?\d+)dBm, TCP丢包:(?P<loss>-?[\d.]+)%, 流量:(?P<traffic>[\d.]+)MB/s"
                ),
            ),
            (
                "emoji_quality",
                re.compile(r"网络质量: (?P<iface>\w+) = (?P<level>\w+) \(分数:(?P<score>[\d.]+)\)"),
            ),
        ]

    def parse_line(self, line: str) -> list[ParsedRecord]:
        ts = parse_glog_timestamp(line)
        records: list[ParsedRecord] = []
        for name, pattern in self.patterns:
            match = pattern.search(line)
            if not match:
                continue
            groups = match.groupdict()
            if name in {"active", "emoji_summary"}:
                records.append(self._metric(ts, line, groups, source=name))
            elif name in {"rtt", "emoji_rtt"}:
                records.append(self._metric(ts, line, groups, source=name))
            elif name in {"tcp", "emoji_tcp"}:
                records.append(
                    self._metric(
                        ts,
                        line,
                        {
                            "iface": groups["iface"],
                            "loss": groups["loss"],
                            "loss_level": groups.get("level", ""),
                        },
                        source=name,
                    )
                )
            elif name in {"rssi", "emoji_rssi"}:
                records.append(self._metric(ts, line, groups, source=name))
            elif name in {"traffic", "emoji_traffic"}:
                records.append(self._metric(ts, line, groups, source=name))
            elif name in {"quality", "emoji_quality"}:
                iface = groups.get("iface") or ""
                records.append(
                    self._metric(
                        ts,
                        line,
                        {
                            "iface": iface.strip(),
                            "quality_level": groups["level"].upper(),
                            "quality_score": groups["score"],
                        },
                        source=name,
                    )
                )
                records.append(
                    ParsedRecord(
                        "event",
                        ts,
                        {
                            "type": "NetworkQualityChanged",
                            "severity": self._quality_severity(groups["level"]),
                            "interface": iface.strip(),
                            "message": f"Network quality {groups['level'].upper()} ({groups['score']})",
                            "details": {"score": float(groups["score"])},
                        },
                        line,
                    )
                )
            elif name == "event_signal":
                records.append(
                    ParsedRecord(
                        "event",
                        ts,
                        {
                            "type": groups["event"],
                            "severity": self._event_severity(groups["event"], groups["message"]),
                            "interface": self._extract_interface(groups["message"]),
                            "message": groups["message"],
                            "details": {"counter": int(groups["counter"])},
                        },
                        line,
                    )
                )
            elif name == "event_emit":
                event_type = {"0": "InterfaceChanged", "1": "ConnectionModeChanged", "2": "NetworkQualityChanged"}.get(
                    groups["type"], f"EventType{groups['type']}"
                )
                records.append(
                    ParsedRecord(
                        "event",
                        ts,
                        {
                            "type": event_type,
                            "severity": self._event_severity(event_type, groups["message"]),
                            "interface": groups["source"],
                            "message": groups["message"],
                            "details": {"source": groups["source"]},
                        },
                        line,
                    )
                )
            elif name == "anomaly":
                records.append(
                    ParsedRecord(
                        "event",
                        ts,
                        {
                            "type": "TrafficAnomaly",
                            "severity": "WARN",
                            "interface": "",
                            "message": f"Traffic anomaly: {groups['atype']}",
                            "details": {"severity": float(groups["severity"])},
                        },
                        line,
                    )
                )
        return records

    def _metric(self, ts: str, raw_line: str, groups: dict[str, str | None], source: str) -> ParsedRecord:
        data: dict[str, Any] = {"source": source, "interface": groups.get("iface") or ""}
        if groups.get("rtt") is not None:
            data["rtt_ms"] = int(groups["rtt"] or 0)
        if groups.get("quality") is not None:
            data["quality_code"] = int(groups["quality"] or 0)
        if groups.get("using") is not None:
            data["using_now"] = parse_bool(groups["using"] or "")
        if groups.get("state") is not None:
            data["using_now"] = 1 if groups["state"] == "ACTIVE" else 0
        if groups.get("rssi") is not None:
            data["rssi_dbm"] = int(groups["rssi"] or 0)
        if groups.get("loss") is not None:
            data["tcp_loss_rate"] = float(groups["loss"] or 0)
        if groups.get("loss_level") is not None:
            data["tcp_loss_level"] = groups.get("loss_level") or ""
        if groups.get("traffic") is not None:
            data["traffic_mbps"] = float(groups["traffic"] or 0)
        if groups.get("flows") is not None:
            data["active_flows"] = int(groups["flows"] or 0)
        if groups.get("pps") is not None:
            data["pps"] = int(groups["pps"] or 0)
        if groups.get("quality_level") is not None:
            data["quality_level"] = groups["quality_level"]
        if groups.get("quality_score") is not None:
            data["quality_score"] = float(groups["quality_score"] or 0)
        return ParsedRecord("metric", ts, data, raw_line)

    @staticmethod
    def _extract_interface(message: str) -> str:
        bracket = re.match(r"\[(?P<iface>[^\]]+)\]", message)
        if bracket:
            return bracket.group("iface")
        for pattern in (r"for (?P<iface>\w+)", r"updated: (?P<iface>\w+)"):
            match = re.search(pattern, message)
            if match:
                return match.group("iface")
        return ""

    @staticmethod
    def _event_severity(event_type: str, message: str) -> str:
        text = f"{event_type} {message}".upper()
        if any(word in text for word in ("POOR", "ERROR", "FAILED", "ANOMALY")):
            return "CRITICAL"
        if any(word in text for word in ("FAIR", "LOSS", "UPDATED", "CHANGED")):
            return "WARN"
        return "INFO"

    @staticmethod
    def _quality_severity(level: str) -> str:
        level = level.upper()
        if level == "POOR":
            return "CRITICAL"
        if level == "FAIR":
            return "WARN"
        return "INFO"


class DashboardStore:
    def __init__(self, db_path: Path) -> None:
        self.db_path = db_path
        self.lock = threading.Lock()
        self.gateway_status: dict[str, Any] = detect_default_gateway()
        self.probe_status: dict[str, Any] = {
            "target": DEFAULT_RTT_TARGET,
            "reachable": None,
            "rtt_ms": None,
            "checked_at": None,
            "error": "",
        }
        self.selected_interface: str | None = None
        self.last_iface_counters: dict[str, tuple[float, dict[str, int]]] = {}
        self.last_tcp_counters: tuple[float, dict[str, int]] | None = None
        self.db_path.parent.mkdir(parents=True, exist_ok=True)
        self._init_db()

    def _connect(self) -> sqlite3.Connection:
        conn = sqlite3.connect(self.db_path)
        conn.row_factory = sqlite3.Row
        return conn

    def _init_db(self) -> None:
        with self._connect() as conn:
            conn.executescript(
                """
                CREATE TABLE IF NOT EXISTS metrics (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    line_hash TEXT UNIQUE,
                    ts TEXT NOT NULL,
                    interface TEXT,
                    rtt_ms REAL,
                    quality_code INTEGER,
                    using_now INTEGER,
                    rssi_dbm INTEGER,
                    tcp_loss_rate REAL,
                    tcp_loss_level TEXT,
                    traffic_mbps REAL,
                    active_flows INTEGER,
                    pps INTEGER,
                    quality_level TEXT,
                    quality_score REAL,
                    source TEXT,
                    raw_line TEXT
                );

                CREATE TABLE IF NOT EXISTS events (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    line_hash TEXT UNIQUE,
                    ts TEXT NOT NULL,
                    type TEXT NOT NULL,
                    severity TEXT,
                    interface TEXT,
                    message TEXT,
                    details TEXT,
                    raw_line TEXT
                );

                CREATE INDEX IF NOT EXISTS idx_metrics_ts ON metrics(ts);
                CREATE INDEX IF NOT EXISTS idx_metrics_iface ON metrics(interface);
                CREATE INDEX IF NOT EXISTS idx_events_ts ON events(ts);
                """
            )

    def insert(self, record: ParsedRecord) -> bool:
        h = line_hash(f"{record.kind}:{record.raw_line}:{record.data}")
        with self.lock, self._connect() as conn:
            if record.kind == "metric":
                fields = {
                    "line_hash": h,
                    "ts": record.ts,
                    "interface": record.data.get("interface"),
                    "rtt_ms": record.data.get("rtt_ms"),
                    "quality_code": record.data.get("quality_code"),
                    "using_now": record.data.get("using_now"),
                    "rssi_dbm": record.data.get("rssi_dbm"),
                    "tcp_loss_rate": record.data.get("tcp_loss_rate"),
                    "tcp_loss_level": record.data.get("tcp_loss_level"),
                    "traffic_mbps": record.data.get("traffic_mbps"),
                    "active_flows": record.data.get("active_flows"),
                    "pps": record.data.get("pps"),
                    "quality_level": record.data.get("quality_level"),
                    "quality_score": record.data.get("quality_score"),
                    "source": record.data.get("source"),
                    "raw_line": record.raw_line.rstrip(),
                }
                return self._insert_row(conn, "metrics", fields)

            fields = {
                "line_hash": h,
                "ts": record.ts,
                "type": record.data.get("type"),
                "severity": record.data.get("severity"),
                "interface": record.data.get("interface"),
                "message": record.data.get("message"),
                "details": json.dumps(record.data.get("details", {}), ensure_ascii=False),
                "raw_line": record.raw_line.rstrip(),
            }
            return self._insert_row(conn, "events", fields)

    def insert_metric_sample(self, ts: str, sample: dict[str, Any]) -> bool:
        fields = {
            "line_hash": line_hash(f"sample:{ts}:{sample.get('interface', '')}"),
            "ts": ts,
            "interface": sample.get("interface"),
            "rtt_ms": sample.get("rtt_ms"),
            "quality_code": sample.get("quality_code"),
            "using_now": sample.get("using_now"),
            "rssi_dbm": sample.get("rssi_dbm"),
            "tcp_loss_rate": sample.get("tcp_loss_rate"),
            "tcp_loss_level": sample.get("tcp_loss_level"),
            "traffic_mbps": sample.get("traffic_mbps"),
            "active_flows": sample.get("active_flows"),
            "pps": sample.get("pps"),
            "quality_level": sample.get("quality_level"),
            "quality_score": sample.get("quality_score"),
            "source": "dashboard_sampler",
            "raw_line": "dashboard current-status sample",
        }
        with self.lock, self._connect() as conn:
            return self._insert_row(conn, "metrics", fields)

    @staticmethod
    def _clean_metric_value(field: str, value: Any) -> Any:
        if value is None:
            return None
        try:
            number = float(value)
        except (TypeError, ValueError):
            return value
        if field in {"rtt_ms", "tcp_loss_rate"} and number < 0:
            return None
        if field == "rssi_dbm" and number <= -999:
            return None
        return value

    @staticmethod
    def _insert_row(conn: sqlite3.Connection, table: str, fields: dict[str, Any]) -> bool:
        columns = ", ".join(fields.keys())
        placeholders = ", ".join("?" for _ in fields)
        cursor = conn.execute(
            f"INSERT OR IGNORE INTO {table} ({columns}) VALUES ({placeholders})",
            list(fields.values()),
        )
        return cursor.rowcount > 0

    def status(self) -> dict[str, Any]:
        with self.lock, self._connect() as conn:
            interfaces = self._interfaces(conn)
            quality = conn.execute(
                """
                SELECT ts, interface, quality_level, quality_score
                FROM metrics
                WHERE quality_level IS NOT NULL OR quality_score IS NOT NULL
                ORDER BY ts DESC, id DESC
                LIMIT 1
                """
            ).fetchone()
            latest_event = conn.execute("SELECT * FROM events ORDER BY ts DESC, id DESC LIMIT 1").fetchone()
            active = next((i for i in interfaces if i.get("using_now") == 1), None)
            if not active and interfaces:
                active = interfaces[0]
            now = datetime.now()
            recent_cutoff = (now - timedelta(minutes=5)).isoformat(timespec="seconds")
            points_5m = conn.execute("SELECT COUNT(*) AS n FROM metrics WHERE ts >= ?", (recent_cutoff,)).fetchone()["n"]

        active = active or {}
        rtt_ms = self._clean_metric_value("rtt_ms", active.get("rtt_ms"))
        tcp_loss_rate = self._clean_metric_value("tcp_loss_rate", active.get("tcp_loss_rate"))
        rssi_dbm = self._clean_metric_value("rssi_dbm", active.get("rssi_dbm"))
        if rtt_ms is None and self.probe_status.get("reachable") and self.probe_status.get("rtt_ms") is not None:
            rtt_ms = self.probe_status["rtt_ms"]
        return {
            "generated_at": now.isoformat(timespec="seconds"),
            "active_interface": active.get("interface", ""),
            "quality_level": (quality["quality_level"] if quality and quality["quality_level"] else self._score_level(active)),
            "quality_score": quality["quality_score"] if quality and quality["quality_score"] is not None else None,
            "rtt_ms": rtt_ms,
            "tcp_loss_rate": tcp_loss_rate,
            "tcp_loss_level": active.get("tcp_loss_level"),
            "rssi_dbm": rssi_dbm,
            "traffic_mbps": active.get("traffic_mbps"),
            "active_flows": active.get("active_flows"),
            "pps": active.get("pps"),
            "interfaces": interfaces,
            "gateway": dict(self.gateway_status),
            "probe": dict(self.probe_status),
            "latest_event": dict(latest_event) if latest_event else None,
            "recent_metric_points": points_5m,
            "selected_interface": self.selected_interface,
        }

    def update_gateway_status(self) -> dict[str, Any]:
        gateway = detect_default_gateway()
        if gateway.get("ip"):
            reachable, rtt_ms, error = ping_host_once(gateway["ip"])
            gateway["reachable"] = reachable
            gateway["rtt_ms"] = rtt_ms
            gateway["error"] = "" if reachable else error
            gateway["checked_at"] = datetime.now().isoformat(timespec="seconds")
        with self.lock:
            self.gateway_status = gateway
        return gateway

    def update_probe_status(self, target: str = DEFAULT_RTT_TARGET) -> dict[str, Any]:
        reachable, rtt_ms, error = ping_host_once(target)
        probe = {
            "target": target,
            "reachable": reachable,
            "rtt_ms": rtt_ms,
            "checked_at": datetime.now().isoformat(timespec="seconds"),
            "error": "" if reachable else error,
        }
        with self.lock:
            self.probe_status = probe
        return probe

    def sample_current_status(self) -> bool:
        status = self.status()
        active_name = status.get("active_interface")
        if not active_name:
            return False
        active = next(
            (iface for iface in status.get("interfaces", []) if iface.get("interface") == active_name),
            None,
        )
        if not active:
            return False
        sample = dict(active)
        if self._clean_metric_value("rtt_ms", sample.get("rtt_ms")) is None and status.get("rtt_ms") is not None:
            sample["rtt_ms"] = status.get("rtt_ms")
        if self._clean_metric_value("tcp_loss_rate", sample.get("tcp_loss_rate")) is None and status.get("tcp_loss_rate") is not None:
            sample["tcp_loss_rate"] = status.get("tcp_loss_rate")
        system_sample = self.sample_system_counters(active_name)
        sample.update({k: v for k, v in system_sample.items() if v is not None})
        sample["quality_level"] = status.get("quality_level")
        sample["quality_score"] = status.get("quality_score")
        ts = datetime.now().isoformat(timespec="seconds")
        return self.insert_metric_sample(ts, sample)

    def sample_system_counters(self, interface: str) -> dict[str, Any]:
        now = time.monotonic()
        result: dict[str, Any] = {}

        iface_counters = read_iface_counters(interface)
        if iface_counters:
            previous = self.last_iface_counters.get(interface)
            self.last_iface_counters[interface] = (now, iface_counters)
            if previous:
                prev_ts, prev = previous
                elapsed = max(now - prev_ts, 0.001)
                byte_delta = max(iface_counters["bytes"] - prev["bytes"], 0)
                packet_delta = max(iface_counters["packets"] - prev["packets"], 0)
                result["traffic_mbps"] = byte_delta / elapsed / (1024 * 1024)
                result["pps"] = int(packet_delta / elapsed)
                result["active_flows"] = 1 if byte_delta > 0 else 0

        tcp_counters = read_tcp_counters()
        if tcp_counters:
            previous_tcp = self.last_tcp_counters
            self.last_tcp_counters = (now, tcp_counters)
            if previous_tcp:
                _prev_ts, prev = previous_tcp
                sent_delta = max(tcp_counters["out_segs"] - prev["out_segs"], 0)
                retrans_delta = max(tcp_counters["retrans_segs"] - prev["retrans_segs"], 0)
                if sent_delta >= 20:
                    rate = retrans_delta * 100.0 / sent_delta
                    result["tcp_loss_rate"] = min(rate, 100.0)
                    result["tcp_loss_level"] = "good" if rate <= 0.5 else "fair" if rate <= 1.0 else "poor"

        return result

    def set_selected_interface(self, interface: str | None) -> None:
        with self.lock:
            self.selected_interface = interface or None

    def _interfaces(self, conn: sqlite3.Connection) -> list[dict[str, Any]]:
        names = [
            row["interface"]
            for row in conn.execute(
                "SELECT DISTINCT interface FROM metrics WHERE interface IS NOT NULL AND interface != '' ORDER BY interface"
            )
        ]
        results: list[dict[str, Any]] = []
        fields = [
            "rtt_ms",
            "quality_code",
            "using_now",
            "rssi_dbm",
            "tcp_loss_rate",
            "tcp_loss_level",
            "traffic_mbps",
            "active_flows",
            "pps",
            "quality_level",
            "quality_score",
        ]
        for name in names:
            merged: dict[str, Any] = {"interface": name, "last_seen": None}
            for field in fields:
                invalid_filter = ""
                if field in {"rtt_ms", "tcp_loss_rate"}:
                    invalid_filter = f" AND {field} >= 0"
                elif field == "rssi_dbm":
                    invalid_filter = f" AND {field} > -999"
                row = conn.execute(
                    f"""
                    SELECT ts, {field}
                    FROM metrics
                    WHERE interface = ? AND {field} IS NOT NULL
                      {invalid_filter}
                    ORDER BY ts DESC, id DESC
                    LIMIT 1
                    """,
                    (name,),
                ).fetchone()
                if row:
                    merged[field] = row[field]
                    if not merged["last_seen"] or row["ts"] > merged["last_seen"]:
                        merged["last_seen"] = row["ts"]
                else:
                    merged[field] = None
            results.append(merged)
        results.sort(key=lambda item: (item.get("using_now") != 1, item.get("interface") or ""))
        return results

    @staticmethod
    def _score_level(active: dict[str, Any]) -> str:
        score = active.get("quality_score")
        if score is None:
            return "UNKNOWN"
        if score >= 90:
            return "EXCELLENT"
        if score >= 75:
            return "GOOD"
        if score >= 50:
            return "FAIR"
        return "POOR"

    def metrics(self, minutes: int, interface: str | None = None) -> list[dict[str, Any]]:
        if interface is None:
            interface = self.selected_interface
        cutoff = (datetime.now() - timedelta(minutes=minutes)).isoformat(timespec="seconds")
        params: list[Any] = [cutoff]
        where = "ts >= ?"
        if interface:
            where += " AND interface = ?"
            params.append(interface)
        with self.lock, self._connect() as conn:
            rows = conn.execute(
                f"""
                SELECT ts, interface, rtt_ms, tcp_loss_rate, rssi_dbm, traffic_mbps,
                       active_flows, pps, quality_level, quality_score, using_now, source
                FROM metrics
                WHERE {where}
                ORDER BY ts ASC, id ASC
                """,
                params,
            ).fetchall()
        return [dict(row) for row in rows]

    def events(self, limit: int, interface: str | None = None) -> list[dict[str, Any]]:
        if interface is None:
            interface = self.selected_interface
        with self.lock, self._connect() as conn:
            if interface:
                rows = conn.execute(
                    """
                    SELECT ts, type, severity, interface, message, details
                    FROM events
                    WHERE interface = ?
                    ORDER BY ts DESC, id DESC
                    LIMIT ?
                    """,
                    (interface, limit),
                ).fetchall()
            else:
                rows = conn.execute(
                    "SELECT ts, type, severity, interface, message, details FROM events ORDER BY ts DESC, id DESC LIMIT ?",
                    (limit,),
                ).fetchall()
        events = []
        for row in rows:
            item = dict(row)
            try:
                item["details"] = json.loads(item["details"] or "{}")
            except json.JSONDecodeError:
                item["details"] = {}
            events.append(item)
        return events

    def report(self, minutes: int, interface: str | None = None) -> dict[str, Any]:
        rows = self.metrics(minutes, interface)
        events = self.events(20, interface)
        if not rows:
            return {
                "window_minutes": minutes,
                "summary": "所选时间窗口内没有可用于诊断的网络指标数据。",
                "issues": [],
                "suggestions": ["请确认 weaknet-dbus-server 正在运行，并且 logs/server/runtime.log 正在持续写入。"],
            }

        def values(field: str) -> list[float]:
            cleaned: list[float] = []
            for row in rows:
                value = row.get(field)
                if value is None:
                    continue
                try:
                    number = float(value)
                except (TypeError, ValueError):
                    continue
                if field in {"rtt_ms", "tcp_loss_rate"} and number < 0:
                    continue
                if field == "tcp_loss_rate" and number > 20:
                    continue
                if field == "rssi_dbm" and number <= -999:
                    continue
                cleaned.append(number)
            return cleaned

        def avg(numbers: list[float]) -> float:
            return sum(numbers) / len(numbers)

        def percentile(numbers: list[float], pct: float) -> float:
            if not numbers:
                return 0.0
            ordered = sorted(numbers)
            index = min(len(ordered) - 1, max(0, int(round((pct / 100.0) * (len(ordered) - 1)))))
            return ordered[index]

        rtt = values("rtt_ms")
        loss = values("tcp_loss_rate")
        traffic = values("traffic_mbps")
        rssi = values("rssi_dbm")

        issues: list[str] = []
        suggestions: list[str] = []
        if rtt and percentile(rtt, 95) > 80:
            issues.append(f"检测到延迟偏高：RTT P95 为 {percentile(rtt, 95):.0f} ms，平均值为 {avg(rtt):.0f} ms。")
            suggestions.append("建议检查上行链路是否拥塞，并分别对默认网关和公网 DNS 做 ping 对比。")
        if loss and len(loss) >= 5 and percentile(loss, 95) > 1 and avg(loss) > 0.5:
            issues.append(f"检测到 TCP 重传率偏高：P95 为 {percentile(loss, 95):.2f}%，平均值为 {avg(loss):.2f}%。")
            suggestions.append("建议检查链路质量、双工/速率协商、Wi-Fi 干扰或上游链路丢包。")
        if rssi and min(rssi) < -70:
            issues.append(f"检测到 Wi-Fi 信号较弱：最低 RSSI 为 {min(rssi):.0f} dBm。")
            suggestions.append("建议靠近 AP、调整天线/摆放位置，或切换到有线/更稳定的回传链路。")
        if traffic and max(traffic) > 50:
            issues.append(f"检测到流量突增：最高流量为 {max(traffic):.1f} MB/s。")
            suggestions.append("建议查看 Top Flow 日志，定位是否存在大流量进程或异常连接。")
        if not issues:
            suggestions.append("所选时间窗口内未发现明显弱网症状。")

        return {
            "window_minutes": minutes,
            "summary": f"已分析最近 {minutes} 分钟内的 {len(rows)} 条指标记录和 {len(events)} 条事件记录。",
            "issues": issues,
            "suggestions": suggestions,
        }


class LogTailer(threading.Thread):
    def __init__(self, log_path: Path, store: DashboardStore, stop_event: threading.Event) -> None:
        super().__init__(daemon=True)
        self.log_path = log_path
        self.store = store
        self.stop_event = stop_event
        self.parser = WeakNetLogParser()

    def run(self) -> None:
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self.log_path.touch(exist_ok=True)
        with self.log_path.open("r", encoding="utf-8", errors="ignore") as handle:
            self._backfill(handle)
            while not self.stop_event.is_set():
                line = handle.readline()
                if not line:
                    time.sleep(0.5)
                    continue
                self._process(line)

    def _backfill(self, handle: Any) -> None:
        handle.seek(0, os.SEEK_END)
        size = handle.tell()
        handle.seek(max(0, size - 512 * 1024), os.SEEK_SET)
        if size > 512 * 1024:
            handle.readline()
        for line in handle:
            self._process(line)

    def _process(self, line: str) -> None:
        for record in self.parser.parse_line(line):
            self.store.insert(record)


class StatusSampler(threading.Thread):
    def __init__(self, store: DashboardStore, stop_event: threading.Event, interval_sec: int = 1) -> None:
        super().__init__(daemon=True)
        self.store = store
        self.stop_event = stop_event
        self.interval_sec = interval_sec

    def run(self) -> None:
        while not self.stop_event.is_set():
            try:
                self.store.sample_current_status()
            except Exception as exc:  # Keep the dashboard alive if one sample fails.
                print(f"[dashboard] sampler error: {exc}")
            self.stop_event.wait(self.interval_sec)


class GatewayMonitor(threading.Thread):
    def __init__(self, store: DashboardStore, stop_event: threading.Event, interval_sec: int = 5) -> None:
        super().__init__(daemon=True)
        self.store = store
        self.stop_event = stop_event
        self.interval_sec = interval_sec

    def run(self) -> None:
        while not self.stop_event.is_set():
            try:
                self.store.update_gateway_status()
            except Exception as exc:
                print(f"[dashboard] gateway monitor error: {exc}")
            self.stop_event.wait(self.interval_sec)


class ProbeMonitor(threading.Thread):
    def __init__(
        self,
        store: DashboardStore,
        stop_event: threading.Event,
        target: str = DEFAULT_RTT_TARGET,
        interval_sec: int = 5,
    ) -> None:
        super().__init__(daemon=True)
        self.store = store
        self.stop_event = stop_event
        self.target = target
        self.interval_sec = interval_sec

    def run(self) -> None:
        while not self.stop_event.is_set():
            try:
                self.store.update_probe_status(self.target)
            except Exception as exc:
                print(f"[dashboard] probe monitor error: {exc}")
            self.stop_event.wait(self.interval_sec)


class DashboardHandler(SimpleHTTPRequestHandler):
    store: DashboardStore
    stop_event: threading.Event

    def __init__(self, *args: Any, **kwargs: Any) -> None:
        super().__init__(*args, directory=str(STATIC_DIR), **kwargs)

    def log_message(self, fmt: str, *args: Any) -> None:
        print(f"[dashboard] {self.address_string()} - {fmt % args}")

    def end_headers(self) -> None:
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/":
            self.path = "/index.html"
            return super().do_GET()
        if parsed.path == "/api/status":
            return self._json(self.store.status())
        if parsed.path == "/api/select-interface":
            qs = parse_qs(parsed.query)
            interface = qs.get("interface", [None])[0]
            self.store.set_selected_interface(interface)
            return self._json({"ok": True, "selected_interface": interface})
        if parsed.path == "/api/metrics":
            qs = parse_qs(parsed.query)
            minutes = self._bounded_int(qs.get("minutes", ["60"])[0], 1, 24 * 60, 60)
            interface = qs.get("interface", [None])[0]
            return self._json(self.store.metrics(minutes, interface))
        if parsed.path == "/api/events":
            qs = parse_qs(parsed.query)
            limit = self._bounded_int(qs.get("limit", ["50"])[0], 1, 500, 50)
            interface = qs.get("interface", [None])[0]
            return self._json(self.store.events(limit, interface))
        if parsed.path == "/api/report":
            qs = parse_qs(parsed.query)
            minutes = self._bounded_int(qs.get("minutes", ["10"])[0], 1, 24 * 60, 10)
            interface = qs.get("interface", [None])[0]
            return self._json(self.store.report(minutes, interface))
        if parsed.path == "/events":
            return self._sse()
        return super().do_GET()

    def _json(self, payload: Any, status: HTTPStatus = HTTPStatus.OK) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _sse(self) -> None:
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        while not self.stop_event.is_set():
            payload = json.dumps(self.store.status(), ensure_ascii=False)
            try:
                self.wfile.write(f"event: status\ndata: {payload}\n\n".encode("utf-8"))
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                return
            time.sleep(2)

    @staticmethod
    def _bounded_int(value: str, minimum: int, maximum: int, default: int) -> int:
        try:
            parsed = int(value)
        except (TypeError, ValueError):
            return default
        return max(minimum, min(maximum, parsed))


def build_server(host: str, port: int, store: DashboardStore, stop_event: threading.Event) -> ThreadingHTTPServer:
    DashboardHandler.store = store
    DashboardHandler.stop_event = stop_event
    return ThreadingHTTPServer((host, port), DashboardHandler)


def main() -> int:
    parser = argparse.ArgumentParser(description="WeakNet web dashboard")
    parser.add_argument("--host", default=os.environ.get("WEAKNET_DASHBOARD_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("WEAKNET_DASHBOARD_PORT", "8080")))
    parser.add_argument("--log", type=Path, default=Path(os.environ.get("WEAKNET_LOG_PATH", DEFAULT_LOG)))
    parser.add_argument("--db", type=Path, default=Path(os.environ.get("WEAKNET_DASHBOARD_DB", DEFAULT_DB)))
    args = parser.parse_args()

    stop_event = threading.Event()
    store = DashboardStore(args.db)
    tailer = LogTailer(args.log, store, stop_event)
    tailer.start()
    sampler = StatusSampler(store, stop_event)
    sampler.start()
    gateway_monitor = GatewayMonitor(store, stop_event)
    gateway_monitor.start()
    probe_monitor = ProbeMonitor(store, stop_event, os.environ.get("WEAKNET_RTT_TARGET", DEFAULT_RTT_TARGET))
    probe_monitor.start()

    httpd = build_server(args.host, args.port, store, stop_event)

    def shutdown(_signum: int, _frame: Any) -> None:
        stop_event.set()
        httpd.shutdown()

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    print(f"WeakNet Dashboard listening on http://{args.host}:{args.port}")
    print(f"Tailing log: {args.log}")
    print(f"SQLite cache: {args.db}")
    try:
        httpd.serve_forever()
    finally:
        stop_event.set()
        httpd.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
