
function fmtTs(iso) {
    const d = new Date(iso);
    const dd = String(d.getDate()).padStart(2, '0');
    const mm = String(d.getMonth() + 1).padStart(2, '0');
    const hh = String(d.getHours()).padStart(2, '0');
    const min = String(d.getMinutes()).padStart(2, '0');
    return `${dd}/${mm} ${hh}:${min}`;
}

function rssiColor(rssi) {
    if (rssi == null) return '#94a3b8';
    if (rssi >= -60) return '#22c55e';
    if (rssi >= -75) return '#eab308';
    return '#ef4444';
}

const LAYER_PALETTE = [
    '#16a34a', // 1 - root
    '#2563eb', // 2
    '#d97706', // 3
    '#ec4899', // 4
    '#06b6d4', // 5
    '#110235', // 6
];

function layerColor(layer) {
    if (layer == null) return '#475569';
    return LAYER_PALETTE[(layer - 1) % LAYER_PALETTE.length];
}

function updateUI(s) {
    const devTbody = document.getElementById("devices");
    const total = s.devices.length;
    const online = s.devices.filter(d => d.online).length;
    const root = s.devices.find(d => d.layer === 1 && d.online);

    document.getElementById("devCount").textContent = total;
    document.getElementById("devOnline").textContent = online;
    document.getElementById("statTotal").textContent = online;
    document.getElementById("statChildren").textContent = total;
    document.getElementById("statOffline").textContent = total - online;

    const rootMac  = document.getElementById("rootMac");
    const rootPill = document.getElementById("rootPill");
    if (root) {
        rootMac.textContent  = root.mac;
        rootPill.textContent = "Online";
        rootPill.className   = "root-pill root-pill-on";
    } else {
        rootMac.textContent  = "--";
        rootPill.textContent = "Offline";
        rootPill.className   = "root-pill root-pill-off";
    }

    const sortedDevices = [...s.devices].sort((a, b) => (a.layer ?? 999) - (b.layer ?? 999));
    if (total === 0) {
        devTbody.innerHTML = '<tr><td colspan="6" class="empty">Aguardando dispositivos&hellip;</td></tr>';
    } else {
        devTbody.innerHTML = sortedDevices.map(d => `
                    <tr style="opacity:${d.online ? 1 : 0.45}">
                        <td class="mono">
                            <span class="dot ${d.online ? 'dot-on' : 'dot-off'}"></span>${d.mac}${d.layer === 1 ? '<span class="role-root">ROOT</span>' : ''}
                        </td>
                        <td data-label="Layer"><span style="color:${layerColor(d.layer)};font-weight:600">${d.layer ?? "?"}</span></td>
                        <td data-label="Versão" class="mono" style="font-size:11px;color:#94a3b8">${d.version ?? "?"}</td>
                        <td data-label="RSSI" class="mono" style="color:${rssiColor(d.rssi)}">${d.rssi != null ? d.rssi + ' dBm' : '—'}</td>
                        <td data-label="Hits">${d.hits}</td>
                        <td data-label="Visto"><span class="mono">${fmtTs(d.last_seen)}</span></td>
                    </tr>
                `).join("");
    }

    document.getElementById("updated").textContent = new Date().toLocaleTimeString();
}

async function refresh() {
    try {
        const s = await (await fetch("/api/state")).json();
        console.log("Estado atualizado: %o", s);
        updateUI(s);
    } catch (e) {''
        document.getElementById("updated").textContent = "erro: " + e.message;
    }
}

const socket = io();
socket.on('state_update', updateUI);
socket.on('reading_update', updateReading);
refresh();

function dispararLeitura() {
    const btn = document.getElementById("readBtn");
    const status = document.getElementById("readStatus");
    btn.disabled = true;
    status.textContent = "Aguardando respostas...";
    status.style.color = "#94a3b8";
    socket.emit('read_request');
    setTimeout(() => { btn.disabled = false; }, 3000);
}

socket.on('read_error', function(data) {
    const status = document.getElementById("readStatus");
    status.textContent = data.msg;
    status.style.color = "#ef4444";
    document.getElementById("readBtn").disabled = false;
});

function updateReading(r) {
    const tbody = document.getElementById("readings");
    const existingRow = document.getElementById("row-" + r.mac.replace(/:/g, ""));
    const ts = r.ts ? fmtTs(r.ts) : "--";
    const html = `<tr id="row-${r.mac.replace(/:/g, "")}">
        <td class="mono">${r.mac}</td>
        <td>${r.CH1 ?? "—"}</td>
        <td>${r.CH2 ?? "—"}</td>
        <td>${r.CH3 ?? "—"}</td>
        <td class="mono">${ts}</td>
    </tr>`;
    if (existingRow) {
        existingRow.outerHTML = html;
    } else {
        if (tbody.querySelector(".empty")) tbody.innerHTML = "";
        tbody.insertAdjacentHTML("beforeend", html);
    }
    document.getElementById("readStatus").textContent = "Última leitura: " + new Date().toLocaleTimeString();
    document.getElementById("readStatus").style.color = "#22c55e";
}

document.getElementById("fwFile").addEventListener("change", function () {
    const label = document.getElementById("fwFileName");
    if (this.files.length) {
        label.textContent = this.files[0].name;
        label.classList.add("has-file");
    } else {
        label.textContent = "Escolher .bin…";
        label.classList.remove("has-file");
    }
});

async function sendOta() {
    const fileInput = document.getElementById("fwFile");
    const status = document.getElementById("otaStatus");
    if (!fileInput.files.length) {
        status.textContent = "Selecione um arquivo .bin.";
        status.style.color = "#f59e0b"; return;
    }
    const fname = fileInput.files[0].name;
    status.textContent = `Enviando ${fname}...`;
    status.style.color = "#94a3b8";
    const formData = new FormData();
    formData.append("file", fileInput.files[0]);
    try {
        const r = await fetch("/api/ota/upload", { method: "POST", body: formData });
        const d = await r.json();
        if (d.ok) {
            const kb = d.size ? ` (${(d.size / 1024).toFixed(1)} KB)` : "";
            const rota = /gateway/i.test(fname) ? "self-update do ROOT"
                       : /driver/i.test(fname)  ? "repasse aos nos via mesh"
                       : "nome desconhecido (sera ignorado pelo ROOT)";
            const aviso = d.pushed ? "ROOT notificado (push WS)" : "ROOT offline - envio ignorado";
            status.textContent = `${fname}${kb} enviado! ${aviso} - ${rota}. Veja o Serial [OTA].`;
            status.style.color = d.pushed ? "#22c55e" : "#f59e0b";
        } else {
            status.textContent = "Erro: " + (d.error || "desconhecido");
            status.style.color = "#ef4444";
        }
    } catch (e) {
        status.textContent = "Erro: " + e.message;
        status.style.color = "#ef4444";
    }
}