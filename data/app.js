"use strict";

// ── On load: fetch current config and populate form ───────────────────────
window.addEventListener("DOMContentLoaded", () => {
	initTheme();
	loadConfig();
});

// ── Theme ─────────────────────────────────────────────────────────────────
function initTheme() {
	const html = document.documentElement;
	const icon = document.getElementById("theme-icon");

	// Respect system preference on first visit
	if (window.matchMedia("(prefers-color-scheme: dark)").matches) {
		html.setAttribute("data-bs-theme", "dark");
		icon.className = "bi bi-sun-fill";
	}

	document.getElementById("theme-toggle").addEventListener("click", () => {
		const dark = html.getAttribute("data-bs-theme") === "dark";
		html.setAttribute("data-bs-theme", dark ? "light" : "dark");
		icon.className = dark ? "bi bi-moon-fill" : "bi bi-sun-fill";
	});
}

// ── Load config from device and pre-fill form ─────────────────────────────
function loadConfig() {
	fetch("/config")
		.then((r) => r.json())
		.then((cfg) => {
			document.getElementById("ssid").value = cfg.ssid || "";
			document.getElementById("serverUrl").value = cfg.serverUrl || "";
			document.getElementById("apId").value = cfg.apId ?? "";
			document.getElementById("authTimeout").value = cfg.authTimeout ?? "";
			document.getElementById("lockDuration").value = cfg.lockDuration ?? "";

			const badge = document.getElementById("mode-badge");
			if (cfg.mode === "ap") {
				badge.innerHTML =
					"<span class='badge bg-warning text-dark ms-1' title='Access Point — Setup Mode'><i class='bi bi-wifi'></i></span>";
			} else {
				badge.innerHTML =
					"<span class='badge bg-success ms-1' title='Connected to Network'><i class='bi bi-check-circle'></i></span>";
			}
		})
		.catch(() => showToast("Could not load current config.", "danger"));
}

// ── Password visibility ───────────────────────────────────────────────────
function togglePw() {
	const inp = document.getElementById("password");
	const eye = document.getElementById("pw-eye");
	inp.type = inp.type === "password" ? "text" : "password";
	eye.className = inp.type === "text" ? "bi bi-eye-slash" : "bi bi-eye";
}

// ── Signal helpers ────────────────────────────────────────────────────────
function sigClass(rssi) {
	if (rssi >= -55) return "sig-5";
	if (rssi >= -65) return "sig-4";
	if (rssi >= -75) return "sig-3";
	if (rssi >= -85) return "sig-2";
	return "sig-1";
}

function sigIcon(rssi) {
	if (rssi >= -55) return "bi-reception-4";
	if (rssi >= -65) return "bi-reception-3";
	if (rssi >= -75) return "bi-reception-2";
	if (rssi >= -85) return "bi-reception-1";
	return "bi-reception-0";
}

// ── WiFi scan ─────────────────────────────────────────────────────────────
// The server runs WiFi.scanNetworks() asynchronously. If a scan is already
// in progress it responds 202; we poll every 1.5 s until we get 200.
let _selectedSSID = null;
let _scanTimer = null;

function scanNetworks() {
	const btn = document.getElementById("scan-btn");
	const status = document.getElementById("scan-status");
	const list = document.getElementById("network-list");

	// Clear any previous polling timer
	if (_scanTimer) {
		clearTimeout(_scanTimer);
		_scanTimer = null;
	}

	btn.disabled = true;
	btn.innerHTML =
		'<span class="spinner-border spinner-border-sm me-1"></span>Scanning…';
	status.style.display = "block";
	status.textContent = "Scanning for networks…";
	list.innerHTML = `
    <div class="text-center text-muted small py-3">
      <span class="spinner-border spinner-border-sm me-1"></span>Scanning…
    </div>`;

	_pollScan();
}

function _pollScan() {
	fetch("/scan")
		.then((r) => {
			// 202 = scan still running on the ESP32, try again shortly
			if (r.status === 202) {
				_scanTimer = setTimeout(_pollScan, 1500);
				return null;
			}
			return r.json();
		})
		.then((networks) => {
			if (networks === null) return; // still polling

			const btn = document.getElementById("scan-btn");
			const status = document.getElementById("scan-status");
			const list = document.getElementById("network-list");

			btn.disabled = false;
			btn.innerHTML = '<i class="bi bi-arrow-clockwise me-1"></i>Scan';

			if (!networks.length) {
				status.textContent = "No networks found.";
				list.innerHTML = `
          <div class="text-center text-muted small py-3">
            <i class="bi bi-wifi-off me-1"></i>No networks found
          </div>`;
				return;
			}

			status.textContent = `Found ${networks.length} network${networks.length !== 1 ? "s" : ""}.`;
			list.innerHTML = networks
				.map((n) => {
					const safeSsid = n.ssid
						.replace(/&/g, "&amp;")
						.replace(/</g, "&lt;")
						.replace(/"/g, "&quot;");
					const jsSsid = n.ssid.replace(/\\/g, "\\\\").replace(/'/g, "\\'");
					return `
          <div class="network-item d-flex align-items-center gap-2 p-2 border mb-1
                      ${n.ssid === _selectedSSID ? "selected" : ""}"
               onclick="selectNetwork(this,'${jsSsid}')">
            <i class="bi ${sigIcon(n.rssi)} signal-icon ${sigClass(n.rssi)}"></i>
            <div class="flex-grow-1 text-truncate">
              <div class="fw-medium" style="font-size:.875rem">${safeSsid}</div>
              <div class="text-muted" style="font-size:.75rem">
                ${n.rssi} dBm &nbsp;&middot;&nbsp; ch&nbsp;${n.channel}
              </div>
            </div>
            <i class="bi ${n.secure ? "bi-lock-fill" : "bi-unlock"} lock-icon"
               style="${n.secure ? "" : "opacity:.3"}"></i>
          </div>`;
				})
				.join("");
		})
		.catch(() => {
			const btn = document.getElementById("scan-btn");
			const status = document.getElementById("scan-status");
			const list = document.getElementById("network-list");
			btn.disabled = false;
			btn.innerHTML = '<i class="bi bi-arrow-clockwise me-1"></i>Scan';
			status.textContent = "Scan failed — try again.";
			list.innerHTML = `
        <div class="text-center text-danger small py-3">
          <i class="bi bi-exclamation-circle me-1"></i>Scan failed
        </div>`;
		});
}

function selectNetwork(el, ssid) {
	_selectedSSID = ssid;
	document.getElementById("ssid").value = ssid;
	document
		.querySelectorAll(".network-item")
		.forEach((e) => e.classList.remove("selected"));
	el.classList.add("selected");
	document.getElementById("password").focus();
}

// ── Toast ─────────────────────────────────────────────────────────────────
function showToast(msg, type) {
	const el = document.getElementById("main-toast");
	el.className = `toast align-items-center border-0 text-bg-${type}`;
	document.getElementById("toast-body").textContent = msg;
	bootstrap.Toast.getOrCreateInstance(el, { delay: 4000 }).show();
}

// ── Save ──────────────────────────────────────────────────────────────────
function saveConfig() {
	const form = document.getElementById("config-form");

	// Trigger Bootstrap validation
	if (!form.checkValidity()) {
		form.classList.add("was-validated");
		showToast("Please fill in all required fields correctly.", "warning");
		return;
	}

	const ssid = document.getElementById("ssid").value.trim();
	const url = document.getElementById("serverUrl").value.trim();

	fetch("/save", {
		method: "POST",
		headers: { "Content-Type": "application/json" },
		body: JSON.stringify({
			ssid,
			password: document.getElementById("password").value,
			serverUrl: url,
			apId: parseInt(document.getElementById("apId").value),
			authTimeout: parseInt(document.getElementById("authTimeout").value),
			lockDuration: parseInt(document.getElementById("lockDuration").value),
		}),
	})
		.then((r) => r.json())
		.then((d) =>
			d.ok
				? showToast("Saved — device is rebooting…", "success")
				: showToast("Error: " + (d.error || "unknown"), "danger"),
		)
		.catch(() => showToast("Could not reach device.", "danger"));
}

// ── Factory reset ─────────────────────────────────────────────────────────
function factoryReset() {
	if (!confirm("Reset all settings to factory defaults and reboot?")) return;
	fetch("/reset", { method: "POST" })
		.then(() => showToast("Factory reset — device rebooting…", "warning"))
		.catch(() => showToast("Could not reach device.", "danger"));
}
