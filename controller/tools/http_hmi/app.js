// HTTP test panel. Talks only to its own Python server (run_http_hmi.py):
//   POST /api/esp32  {endpoint, body, timeout, quiet}  forwards to the ESP32
//   GET  /api/state?since=N                           reports the ESP32 pushed
//   POST /api/config {esp32_url}, POST /api/clear
'use strict';

const MOTION_TIMEOUT_S = 30;
const READ_TIMEOUT_S = 2;
const STATE_POLL_MS = 500;
const JOINT_NAMES = ['1 Base', '2 Shoulder', '3 Elbow', '4 Wrist'];

const $ = (id) => document.getElementById(id);

// ------------------------------------------------------------------ log

function log(text, cls = 'log-rx') {
    const out = $('log');
    const atBottom = out.scrollHeight - out.scrollTop - out.clientHeight < 30;
    const line = document.createElement('div');
    line.className = cls;
    line.textContent = `[${new Date().toLocaleTimeString()}] ${text}`;
    out.appendChild(line);
    while (out.childElementCount > 1000) out.firstChild.remove();
    if (atBottom) out.scrollTop = out.scrollHeight;
}

function setBadge(el, text, kind) {
    el.textContent = text;
    el.className = `badge badge-${kind}`;
}

// ------------------------------------------------------------------ server calls

async function api(path, body) {
    const resp = await fetch(path, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
    });
    const data = await resp.json().catch(() => ({}));
    if (!resp.ok) throw new Error(data.error || `HTTP ${resp.status}`);
    return data;
}

// Sends one /controller call through the server. Returns the server's
// description: {http_status, response, raw, error, elapsed_ms}.
async function esp(endpoint, body = {}, { timeout = READ_TIMEOUT_S, quiet = false } = {}) {
    if (!quiet) log(`-> ${endpoint} ${JSON.stringify(body)}`, 'log-tx');
    let r;
    try {
        r = await api('/api/esp32', { endpoint, body, timeout, quiet });
    } catch (e) {
        r = { error: `panel server: ${e.message}` };
    }
    // Quiet (polling) calls log nothing: a failed read shows as "ESP32 offline".
    if (!quiet) {
        const ms = r.elapsed_ms != null ? ` (${r.elapsed_ms} ms)` : '';
        if (r.error) {
            log(`<- ${endpoint} ERROR ${r.http_status ?? ''} ${r.error}${ms}`, 'log-error');
        } else if (r.http_status !== 200) {
            log(`<- ${endpoint} HTTP ${r.http_status} ${r.raw}${ms}`, 'log-error');
        } else {
            log(`<- ${endpoint} ${r.raw || '{}'}${ms}`, 'log-success');
        }
    }
    return r;
}

// Shows a command's outcome in its badge.
function showResult(badge, r) {
    if (r.error && r.http_status == null) return setBadge(badge, 'comm fault', 'danger');
    if (r.http_status !== 200) return setBadge(badge, `HTTP ${r.http_status}`, 'danger');
    if (r.error) return setBadge(badge, r.error, 'danger');
    const res = r.response || {};
    if ('operation_result' in res) {
        const kind = { success: 'success', invalid: 'warning', rejected: 'danger' }[res.operation_result] || 'secondary';
        return setBadge(badge, `${res.operation_result} (${(r.elapsed_ms / 1000).toFixed(1)} s)`, kind);
    }
    if ('operation_done' in res) return setBadge(badge, res.operation_done ? 'done' : 'failed', res.operation_done ? 'success' : 'danger');
    return setBadge(badge, 'ok', 'success');
}

async function command(badgeId, endpoint, body, motion = false) {
    const badge = $(badgeId);
    setBadge(badge, motion ? 'moving...' : 'sending...', 'info');
    const r = await esp(endpoint, body, { timeout: motion ? MOTION_TIMEOUT_S : READ_TIMEOUT_S });
    showResult(badge, r);
    return r;
}

function num(id) {
    const v = parseFloat($(id).value);
    if (!Number.isFinite(v)) {
        log(`${$(id).getAttribute('aria-label') || id}: not a number`, 'log-error');
        return null;
    }
    return v;
}

// ------------------------------------------------------------------ live values

const live = { pose: null, angles: null, lastOk: 0 };
let readInFlight = false;
let pollTimer = null;
let espOnline = null;

function buildJointTiles() {
    $('joint-tiles').innerHTML = JOINT_NAMES.map((name, i) =>
        `<div class="tile"><div class="tile-label">${name}</div>` +
        `<div class="tile-value"><span id="v-j${i + 1}">--</span><span class="tile-unit">&deg;</span></div></div>`).join('');
}

function ok(r) { return !r.error && r.http_status === 200; }

async function readOnce() {
    if (readInFlight) return;
    readInFlight = true;
    const quiet = !$('chk-show-poll').checked;
    const withDistance = $('chk-distance').checked;
    const t0 = performance.now();
    try {
        const calls = [
            esp('/controller/get-current-arm-position', {}, { quiet }),
            esp('/controller/get-current-joint-position', {}, { quiet }),
        ];
        if (withDistance) calls.push(esp('/controller/get-obstacle-distance', {}, { quiet }));
        const [pos, joints, dist] = await Promise.all(calls);

        let allOk = ok(pos) && ok(joints);
        if (ok(pos)) {
            const p = pos.response;
            live.pose = p;
            $('v-x').textContent = fmt(p.x);
            $('v-y').textContent = fmt(p.y);
            $('v-z').textContent = fmt(p.z);
        }
        if (ok(joints) && Array.isArray(joints.response.angles)) {
            const a = joints.response.angles;
            live.angles = a;
            a.forEach((v, i) => {
                if (!$(`v-j${i + 1}`)) {
                    // A fifth value (gripper on the sorting arm) gets its own tile.
                    $('joint-tiles').insertAdjacentHTML('beforeend',
                        `<div class="tile"><div class="tile-label">${i + 1}</div>` +
                        `<div class="tile-value"><span id="v-j${i + 1}">--</span><span class="tile-unit">&deg;</span></div></div>`);
                }
                $(`v-j${i + 1}`).textContent = fmt(v);
            });
        }
        if (dist) {
            if (ok(dist)) {
                $('v-dist').textContent = dist.response.distance;
            } else if (dist.http_status === 404) {
                // Not this build (sorting): stop asking.
                $('chk-distance').checked = false;
                $('v-dist').textContent = 'n/a';
                log('get-obstacle-distance answered 404, distance reads switched off', 'log-system');
            } else {
                allOk = false;
            }
        }
        $('v-rtt').textContent = Math.round(performance.now() - t0);
        $('card-live').classList.toggle('stale', !allOk);
        if (allOk) live.lastOk = Date.now();
        setOnline(allOk, allOk ? '' : (pos.error || joints.error || dist?.error || `HTTP ${pos.http_status}`));
    } finally {
        readInFlight = false;
    }
}

function setOnline(online, why) {
    if (online === espOnline) return;
    espOnline = online;
    setBadge($('badge-esp'), online ? 'ESP32 online' : 'ESP32 offline', online ? 'success' : 'danger');
    log(online ? 'ESP32 online' : `ESP32 offline: ${why}`, online ? 'log-success' : 'log-error');
}

function fmt(v) { return typeof v === 'number' ? v.toFixed(1) : '--'; }

function restartPolling() {
    clearInterval(pollTimer);
    pollTimer = null;
    if ($('chk-poll').checked) {
        readOnce();
        pollTimer = setInterval(readOnce, parseInt($('sel-interval').value, 10));
    } else {
        setBadge($('badge-esp'), 'ESP32 not polled', 'secondary');
        espOnline = null;
    }
}

setInterval(() => {
    $('v-poll-age').textContent = live.lastOk ? `${((Date.now() - live.lastOk) / 1000).toFixed(0)} s ago` : '';
}, 1000);

// ------------------------------------------------------------------ reports (server state)

let stateSeq = 0;
let serverUp = null;

const EVENT_CLASS = {
    status: 'log-report', 'alarm-on': 'log-error', 'alarm-off': 'log-success',
    result: 'log-report', bad: 'log-error', system: 'log-system',
};

async function pollState() {
    let s;
    try {
        const resp = await fetch(`/api/state?since=${stateSeq}`, { cache: 'no-store' });
        s = await resp.json();
    } catch (e) {
        if (serverUp !== false) {
            serverUp = false;
            setBadge($('badge-server'), 'Server down', 'danger');
            log('panel server not reachable: is run_http_hmi.py still running?', 'log-error');
        }
        return;
    }
    if (serverUp !== true) {
        serverUp = true;
        setBadge($('badge-server'), 'Server ok', 'success');
        if (document.activeElement !== $('in-esp32')) $('in-esp32').value = s.esp32_url;
    }
    for (const e of s.events) log(`ESP32 report: ${e.text}`, EVENT_CLASS[e.kind] || 'log-rx');
    stateSeq = s.seq;

    const st = s.status;
    $('v-status').textContent = st ?? '--';
    $('v-status').style.color = { idle: 'var(--accent-green)', running: 'var(--accent-cyan)', stop: 'var(--accent-red)', manual: 'var(--accent-amber)' }[st] || '';
    $('v-last-report').textContent = s.last_report ? `${s.last_report} from ${s.last_report_from} (#${s.report_count})` : 'none yet';

    const list = $('alarm-list');
    list.replaceChildren(...(s.alarms.length ? s.alarms.map((a) => {
        const li = document.createElement('li');
        li.textContent = a.message;
        const t = document.createElement('span');
        t.textContent = `since ${a.since}`;
        li.appendChild(t);
        return li;
    }) : [Object.assign(document.createElement('div'), { className: 'empty', textContent: 'No active alarm' })]));

    $('result-rows').replaceChildren(...s.results.map((r) => {
        const tr = document.createElement('tr');
        for (const v of [r.time, r.part_id, r.reading_result, r.origin, r.destination, r.repository, r.sorting_result ? 'yes' : 'NO']) {
            const td = document.createElement('td');
            td.textContent = v;
            tr.appendChild(td);
        }
        if (!r.sorting_result) tr.style.color = 'var(--accent-red)';
        return tr;
    }));
}

// ------------------------------------------------------------------ wiring

function wire() {
    buildJointTiles();
    $('v-motion-to').textContent = MOTION_TIMEOUT_S;

    $('btn-esp32').onclick = async () => {
        try {
            const r = await api('/api/config', { esp32_url: $('in-esp32').value.trim() });
            log(`ESP32 address: ${r.esp32_url}`, 'log-system');
            espOnline = null;
            restartPolling();
        } catch (e) {
            log(`Set ESP32: ${e.message}`, 'log-error');
        }
    };
    $('in-esp32').addEventListener('keydown', (e) => { if (e.key === 'Enter') $('btn-esp32').click(); });

    $('btn-permit-on').onclick = () => command('r-permit', '/controller/set-running-permit', { permit: true });
    $('btn-permit-off').onclick = () => command('r-permit', '/controller/set-running-permit', { permit: false });

    $('btn-xyz').onclick = () => {
        const x = num('in-x'), y = num('in-y'), z = num('in-z');
        if (x === null || y === null || z === null) return;
        command('r-xyz', '/controller/set-arm-position', { x, y, z }, true);
    };
    $('btn-xyz-copy').onclick = () => {
        if (!live.pose) return log('no live position yet', 'log-system');
        $('in-x').value = live.pose.x; $('in-y').value = live.pose.y; $('in-z').value = live.pose.z;
    };

    $('btn-joint').onclick = () => {
        const angle = num('in-angle');
        if (angle === null) return;
        command('r-joint', '/controller/set-joint-position', { joint_number: parseInt($('in-joint').value, 10), angle }, true);
    };
    $('btn-joint-copy').onclick = () => {
        const i = parseInt($('in-joint').value, 10) - 1;
        if (!live.angles || live.angles[i] == null) return log('no live angle for this joint yet', 'log-system');
        $('in-angle').value = live.angles[i];
    };

    $('btn-conv').onclick = () => {
        const distance = num('in-dist');
        if (distance === null) return;
        command('r-conv', '/controller/move-conveyor', { direction: $('in-dir').value === 'true', distance: Math.round(distance) }, true);
    };

    $('btn-config').onclick = () => {
        let body;
        try {
            body = JSON.parse($('in-config').value);
        } catch (e) {
            return setBadge($('r-config'), 'bad JSON', 'danger');
        }
        command('r-config', '/controller/update-position-config', body);
    };

    $('btn-raw').onclick = async () => {
        let body;
        try {
            body = JSON.parse($('in-raw-body').value || '{}');
        } catch (e) {
            $('v-raw-resp').textContent = `Body is not JSON: ${e.message}`;
            return;
        }
        const timeout = parseFloat($('in-raw-to').value) || 5;
        $('v-raw-resp').textContent = 'waiting...';
        const r = await esp($('in-raw-ep').value.trim(), body, { timeout });
        $('v-raw-resp').textContent = JSON.stringify(r, null, 2);
    };

    $('btn-clear-reports').onclick = () => api('/api/clear', {}).catch((e) => log(e.message, 'log-error'));
    $('btn-clear-log').onclick = () => $('log').replaceChildren();
    $('chk-poll').onchange = restartPolling;
    $('sel-interval').onchange = restartPolling;
    $('chk-distance').onchange = () => { if ($('chk-distance').checked) $('v-dist').textContent = '--'; };

    pollState();
    setInterval(pollState, STATE_POLL_MS);
    restartPolling();
}

wire();
