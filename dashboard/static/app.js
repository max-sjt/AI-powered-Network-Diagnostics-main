const state = {
  status: null,
  metrics: [],
  events: [],
  windowMinutes: 15,
  selectedInterface: null,
};

const $ = (id) => document.getElementById(id);

function fmt(value, digits = 0) {
  if (value === null || value === undefined || Number.isNaN(Number(value))) return "-";
  return Number(value).toFixed(digits);
}

function metricFmt(value, digits = 0, invalidValues = []) {
  if (value === null || value === undefined || Number.isNaN(Number(value))) return "N/A";
  if (invalidValues.includes(Number(value))) return "N/A";
  return Number(value).toFixed(digits);
}

function setText(id, value) {
  const node = $(id);
  if (node) node.textContent = value;
}

function setClassName(id, value) {
  const node = $(id);
  if (node) node.className = value;
}

function qualityClass(level) {
  const value = String(level || "UNKNOWN").toLowerCase();
  if (value === "excellent" || value === "good") return "good";
  if (value === "fair") return "fair";
  if (value === "poor") return "poor";
  return "unknown";
}

async function getJson(url) {
  const response = await fetch(url, { cache: "no-store" });
  if (!response.ok) throw new Error(`${url} returned ${response.status}`);
  return response.json();
}

async function refreshAll() {
  state.status = await getJson("/api/status");
  if (!state.selectedInterface && state.status && state.status.selected_interface) {
    state.selectedInterface = state.status.selected_interface;
  }
  await refreshMetricsAndEvents();
  render();
}

async function refreshMetricsAndEvents() {
  const ifaceParam = state.selectedInterface ? `&interface=${encodeURIComponent(state.selectedInterface)}` : "";
  state.metrics = await getJson(`/api/metrics?minutes=${state.windowMinutes}${ifaceParam}`);
  state.events = await getJson(`/api/events?limit=50${ifaceParam}`);
}

function render() {
  renderStatus();
  renderInterfaces();
  renderEvents();
  renderChart();
}

function renderStatus() {
  const status = state.status || {};
  const selected = selectedInterfaceData();
  const view = selected || status;
  setText("subtitle", status.generated_at
    ? `Last updated ${new Date(status.generated_at).toLocaleString()}${state.selectedInterface ? ` · viewing ${state.selectedInterface}` : ""}`
    : "Waiting for parsed metrics...");

  const quality = String(status.quality_level || "UNKNOWN").toUpperCase();
  setText("qualityLevel", quality);
  setText("qualityScore",
    status.quality_score === null || status.quality_score === undefined
      ? "No score"
      : `${fmt(status.quality_score, 1)} / 100`);

  setClassName("qualityCard", `quality ${qualityClass(quality)}`);

  const gateway = status.gateway || {};
  const probe = status.probe || {};
  setText("activeInterface", state.selectedInterface || status.active_interface || "-");
  setText("gatewayIp", gateway.ip ? `${gateway.ip}` : "N/A");
  setText("gatewayRtt", metricFmt(gateway.rtt_ms, 1));
  setText("rttValue", metricFmt(view.rtt_ms, 0, [-1]));
  setText("rttUnit", probe.target && probe.rtt_ms === status.rtt_ms ? `ms to ${probe.target}` : "ms");
  setText("lossValue", metricFmt(view.tcp_loss_rate, 2, [-1]));
  setText("rssiValue", metricFmt(view.rssi_dbm, 0, [-1000]));
  setText("trafficValue", metricFmt(view.traffic_mbps, 1));
  setText("pointCount", `${state.metrics.length} points`);
}

function selectedInterfaceData() {
  if (!state.selectedInterface || !state.status || !state.status.interfaces) return null;
  return state.status.interfaces.find((iface) => iface.interface === state.selectedInterface) || null;
}

function renderInterfaces() {
  const container = $("interfaces");
  const interfaces = (state.status && state.status.interfaces) || [];
  const gateway = (state.status && state.status.gateway) || {};
  if (!interfaces.length) {
    container.innerHTML = `<div class="iface-row"><span class="event-message">No interfaces parsed yet.</span></div>`;
    return;
  }

  container.innerHTML = interfaces
    .map((iface) => {
      const active = iface.using_now === 1;
      const selected = state.selectedInterface === iface.interface || (!state.selectedInterface && active);
      const loss = iface.tcp_loss_rate === null || iface.tcp_loss_rate === undefined || Number(iface.tcp_loss_rate) < 0
        ? "N/A"
        : `${fmt(iface.tcp_loss_rate, 2)}%`;
      return `
        <button class="iface-row iface-button ${active ? "active" : ""} ${selected ? "selected" : ""}" data-interface="${escapeHtml(iface.interface || "")}" type="button">
          <div class="row-top">
            <span class="row-title">${escapeHtml(iface.interface || "-")}</span>
            <span class="badge ${active ? "good" : ""}">${selected ? "selected" : active ? "active" : "standby"}</span>
          </div>
          <div class="row-grid">
            <span>Gateway ${gateway.interface === iface.interface && gateway.ip ? escapeHtml(gateway.ip) : "-"}</span>
            <span>RTT ${metricFmt(iface.rtt_ms, 0, [-1])} ms</span>
            <span>Loss ${loss}</span>
            <span>RSSI ${metricFmt(iface.rssi_dbm, 0, [-1000])} dBm</span>
            <span>Traffic ${metricFmt(iface.traffic_mbps, 1)} MB/s</span>
            <span>Flows ${fmt(iface.active_flows, 0)}</span>
            <span>PPS ${fmt(iface.pps, 0)}</span>
          </div>
        </button>
      `;
    })
    .join("");
  container.querySelectorAll(".iface-button").forEach((button) => {
    button.addEventListener("click", async () => {
      const iface = button.getAttribute("data-interface");
      state.selectedInterface = iface || null;
      await getJson(`/api/select-interface?interface=${encodeURIComponent(state.selectedInterface || "")}`);
      await refreshAll();
    });
  });
}

function renderEvents() {
  const container = $("events");
  setText("eventCount", `${state.events.length} events`);
  if (!state.events.length) {
    container.innerHTML = `<div class="event-row"><span class="event-message">No events parsed yet.</span></div>`;
    return;
  }

  container.innerHTML = state.events
    .map((event) => {
      const severity = String(event.severity || "INFO").toLowerCase();
      const badgeClass = severity === "critical" ? "bad" : severity === "warn" ? "warn" : "";
      const ts = event.ts ? new Date(event.ts).toLocaleString() : "-";
      return `
        <div class="event-row ${severity}">
          <div class="row-top">
            <span class="row-title">${escapeHtml(event.type || "Event")}</span>
            <span class="badge ${badgeClass}">${escapeHtml(event.severity || "INFO")}</span>
          </div>
          <span class="event-message">${escapeHtml(event.message || "")}</span>
          <span class="event-time">${ts}${event.interface ? ` · ${escapeHtml(event.interface)}` : ""}</span>
        </div>
      `;
    })
    .join("");
}

function renderChart() {
  const canvas = $("trendCanvas");
  if (!canvas) return;
  const ctx = canvas.getContext("2d");
  const width = canvas.width;
  const height = canvas.height;
  ctx.clearRect(0, 0, width, height);

  drawGrid(ctx, width, height);
  if (!state.metrics.length) {
    ctx.fillStyle = "#627089";
    ctx.font = "18px system-ui";
    ctx.fillText("No trend data yet", 32, 56);
    return;
  }

  const rows = state.metrics
    .filter((row) => row.ts)
    .map((row) => ({ ...row, t: new Date(row.ts).getTime() }))
    .filter((row) => Number.isFinite(row.t));

  if (!rows.length) return;
  const minT = Math.min(...rows.map((row) => row.t));
  const maxT = Math.max(...rows.map((row) => row.t));
  const pad = { left: 52, right: 24, top: 24, bottom: 34 };
  const plotW = width - pad.left - pad.right;
  const plotH = height - pad.top - pad.bottom;

  const drawn = [
    drawSeries(ctx, rows, "rtt_ms", "#2563eb", minT, maxT, pad, plotW, plotH),
    drawSeries(ctx, rows, "tcp_loss_rate", "#b42318", minT, maxT, pad, plotW, plotH),
    drawSeries(ctx, rows, "traffic_mbps", "#0f766e", minT, maxT, pad, plotW, plotH),
  ].filter(Boolean).length;

  ctx.fillStyle = "#627089";
  ctx.font = "16px system-ui";
  ctx.fillText("0", 28, pad.top + plotH);
  if (!drawn) {
    ctx.fillStyle = "#627089";
    ctx.font = "18px system-ui";
    ctx.fillText("Metrics are present, but values are invalid or flat at zero.", 32, 56);
  }
}

function drawGrid(ctx, width, height) {
  ctx.fillStyle = "#fbfcfe";
  ctx.fillRect(0, 0, width, height);
  ctx.strokeStyle = "#e6eaf0";
  ctx.lineWidth = 1;
  for (let i = 0; i <= 5; i += 1) {
    const y = 24 + i * ((height - 58) / 5);
    ctx.beginPath();
    ctx.moveTo(52, y);
    ctx.lineTo(width - 24, y);
    ctx.stroke();
  }
}

function drawSeries(ctx, rows, field, color, minT, maxT, pad, plotW, plotH) {
  const points = rows
    .filter((row) => row[field] !== null && row[field] !== undefined && Number(row[field]) >= 0)
    .map((row) => ({ t: row.t, v: Number(row[field]) }));
  if (points.length < 2) return false;

  const maxV = Math.max(...points.map((point) => point.v), 1);
  const rangeT = Math.max(maxT - minT, 1);

  ctx.strokeStyle = color;
  ctx.lineWidth = 2;
  ctx.beginPath();
  points.forEach((point, index) => {
    const x = pad.left + ((point.t - minT) / rangeT) * plotW;
    const y = pad.top + plotH - (point.v / maxV) * plotH;
    if (index === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });
  ctx.stroke();
  return true;
}

async function generateReport() {
  const report = $("report");
  report.innerHTML = "<p>正在生成诊断结果...</p>";
  try {
    const ifaceParam = state.selectedInterface ? `&interface=${encodeURIComponent(state.selectedInterface)}` : "";
    const data = await getJson(`/api/report?minutes=${state.windowMinutes}${ifaceParam}`);
    const issues = data.issues && data.issues.length ? data.issues : ["未发现明确异常。"];
    const suggestions = data.suggestions || [];
    report.innerHTML = `
      <p><strong>分析摘要：</strong>${escapeHtml(data.summary || "")}</p>
      <p><strong>问题发现：</strong></p>
      <ul>${issues.map((item) => `<li>${escapeHtml(item)}</li>`).join("")}</ul>
      <p><strong>处理建议：</strong></p>
      <ul>${suggestions.map((item) => `<li>${escapeHtml(item)}</li>`).join("")}</ul>
    `;
  } catch (error) {
    report.innerHTML = `<p>${escapeHtml(error.message)}</p>`;
  }
}

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#039;");
}

function startSse() {
  if (!window.EventSource) return;
  const source = new EventSource("/events");
  source.addEventListener("status", (event) => {
    state.status = JSON.parse(event.data);
    renderStatus();
    renderInterfaces();
  });
  source.onerror = () => {
    source.close();
    setTimeout(startSse, 5000);
  };
}

function startPolling() {
  setInterval(async () => {
    try {
      state.status = await getJson("/api/status");
      await refreshMetricsAndEvents();
      render();
    } catch (error) {
      setText("subtitle", error.message);
    }
  }, 1000);
}

if ($("windowSelect")) {
  $("windowSelect").addEventListener("change", async (event) => {
    state.windowMinutes = Number(event.target.value);
    await refreshAll();
  });
}

if ($("refreshBtn")) $("refreshBtn").addEventListener("click", refreshAll);
if ($("reportBtn")) $("reportBtn").addEventListener("click", generateReport);

refreshAll().catch((error) => {
  setText("subtitle", error.message);
});
startSse();
startPolling();
