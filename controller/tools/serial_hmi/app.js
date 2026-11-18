// Arm controller serial panel: monitoring, simple control and network
// configuration over the serial console of the ESP32 firmware.
//
// The panel polls "INFO" twice a second. The firmware answers with one
// line "#INFO:{json}" (controller/src/common/HmiCommands.cpp).

const POLL_MS = 500;
const JOINT_NAMES = ['1 Base', '2 Shoulder', '3 Elbow', '4 Wrist'];

const demo = new URLSearchParams(location.search).has('demo');
const serial = demo ? new DemoSerial() : new SerialManager();

const $ = (id) => document.getElementById(id);
let info = null;          // last INFO object
let pollTimer = null;
let lastInfoAt = 0;
let cfgDirty = false;     // the user is editing the configuration fields

// ---------------- Serial log ----------------

function log(text, kind) {
    const out = $('terminal-output');
    const line = document.createElement('div');
    line.className = 'log-' + kind;
    line.textContent = `${new Date().toLocaleTimeString()}  ${text}`;
    out.appendChild(line);
    while (out.childElementCount > 500) out.removeChild(out.firstChild);
    out.scrollTop = out.scrollHeight;
}

function send(cmd) {
    return serial.send(cmd);
}

// ---------------- Rendering ----------------

function setBadge(el, text, kind) {
    el.textContent = text;
    el.className = 'badge badge-' + kind;
}

function fmt(v) {
    return typeof v === 'number' ? v.toFixed(1) : '--';
}

function render() {
    const live = serial.isConnected && info && Date.now() - lastInfoAt < 3 * POLL_MS + 500;
    if (!serial.isConnected) {
        setBadge($('badge-conn'), 'Disconnected', 'secondary');
    } else if (live) {
        setBadge($('badge-conn'), demo ? 'Demo' : 'Connected', 'success');
    } else {
        setBadge($('badge-conn'), 'No answer from board', 'warning');
    }
    if (!info) return;

    $('app-name').textContent = info.app + (demo ? ' (demo data)' : '');

    const statusKind = { idle: 'success', stop: 'secondary', manual: 'warning' }[info.status] || 'info';
    setBadge($('badge-status'), info.status, statusKind);
    $('v-permit').textContent = info.permit ? 'ON' : 'OFF';
    $('v-permit').style.color = info.permit ? 'var(--accent-green)' : 'var(--text-secondary)';
    $('v-moving').textContent = info.moving ? 'Moving' : 'At target';
    $('v-manual').textContent = info.manual ? 'Joystick' : 'HMI / auto';

    // The obstacle sensor exists only in the 4-DOF build.
    const hasSensor = 'distance' in info;
    $('tile-distance').hidden = !hasSensor;
    if (hasSensor) {
        const d = $('v-distance');
        d.textContent = info.distance < 0 ? 'Nothing in range' : `${info.distance} mm`;
        d.classList.toggle('small', info.distance < 0);
        d.style.color = info.alarm ? 'var(--accent-red)' : '';
        if (info.alarm) d.textContent += '  ALARM';
    }

    $('v-x').textContent = fmt(info.x);
    $('v-y').textContent = fmt(info.y);
    $('v-z').textContent = fmt(info.z);
    info.joints.forEach((q, i) => { $(`v-j${i + 1}`).textContent = fmt(q); });
    info.limits.forEach((lim, i) => {
        $(`jr-range-${i}`).textContent = `${lim[0]} to ${lim[1]}`;
        const input = $(`jr-in-${i}`);
        if (document.activeElement !== input && !input.dataset.edited) input.value = info.target[i];
    });

    // Joystick readings exist only in the 4-DOF build.
    const js = info.joystick;
    $('joystick-block').hidden = !js;
    if (js) {
        $('v-joy-x').textContent = js.x.toFixed(2);
        $('v-joy-y').textContent = js.y.toFixed(2);
        $('v-joy-rawx').textContent = js.raw_x;
        $('v-joy-rawy').textContent = js.raw_y;
        $('v-joy-cx').textContent = js.centre_x;
        $('v-joy-cy').textContent = js.centre_y;
        setBadge($('badge-sw'), js.sw ? 'SW pressed' : 'SW released', js.sw ? 'warning' : 'secondary');
        // World X (forward) is drawn upward, world Y (side) to the right.
        const dot = $('joy-dot');
        dot.style.left = `${50 + 42 * js.y}%`;
        dot.style.top = `${50 - 42 * js.x}%`;
        dot.style.backgroundColor = js.x || js.y ? 'var(--accent-cyan)' : 'var(--text-muted)';
    }

    const w = info.wifi;
    setBadge($('badge-wifi'), w.connected ? 'Wi-Fi connected' : 'Wi-Fi not connected', w.connected ? 'success' : 'danger');
    $('v-ssid').textContent = w.ssid || '--';
    $('v-ip').textContent = w.connected ? w.ip : '--';
    $('v-rssi').textContent = w.connected ? `${w.rssi} dBm` : '--';
    $('v-gw').textContent = `${w.gwhost}:${w.gwport}`;

    $('badge-pending').hidden = !info.cfg.pending;
    if (!cfgDirty) fillConfig();
}

function fillConfig() {
    if (!info) return;
    $('cfg-ssid').value = info.cfg.ssid;
    $('cfg-gwhost').value = info.cfg.gwhost;
    $('cfg-gwport').value = info.cfg.gwport;
    $('cfg-pass').value = '';
    $('cfg-pass').placeholder = info.cfg.pass_set ? 'stored, leave blank to keep' : 'none stored (open network)';
}

function setConnectedUi(connected) {
    document.querySelectorAll('.need-conn').forEach((b) => { b.disabled = !connected; });
    $('btn-connect').textContent = connected ? 'Disconnect' : 'Connect serial';
    $('btn-connect').className = connected ? 'btn btn-secondary' : 'btn btn-primary';
}

// ---------------- Serial events ----------------

serial.onConnect = () => {
    log(demo ? 'Demo mode: no board, simulated answers.' : 'Serial port open.', 'system');
    setConnectedUi(true);
    pollTimer = setInterval(() => { send('INFO'); render(); }, POLL_MS);
    send('INFO');
};

serial.onDisconnect = () => {
    clearInterval(pollTimer);
    log('Serial port closed.', 'system');
    setConnectedUi(false);
    render();
};

serial.onError = (err) => log(err.message || String(err), 'error');

serial.onTx = (cmd) => {
    if (cmd === 'INFO' && !$('chk-show-poll').checked) return;
    // Never show a password in the log.
    log('> ' + cmd.replace(/^(SET\s+PASS)\s.*$/i, '$1 ********'), 'tx');
};

serial.onLine = (line) => {
    if (line.startsWith('#INFO:')) {
        try {
            info = JSON.parse(line.slice(6));
            lastInfoAt = Date.now();
            render();
        } catch (e) {
            // A line cut by a log message from another task: wait for the next one.
        }
        if ($('chk-show-poll').checked) log(line, 'rx');
        return;
    }
    let kind = 'rx';
    if (line.includes('[Nano OK]') || line.startsWith('#CFG:OK') || line.startsWith('#J:OK')) kind = 'success';
    if (line.includes('IK FAIL') || line.includes('ERR') || line.includes('ERROR') ||
        line.includes('Unknown command') || line.includes('Format error')) kind = 'error';
    log(line, kind);
    if (line.startsWith('#CFG:ERR')) $('cfg-hint').textContent = line.slice(9);
};

// ---------------- Controls ----------------

$('btn-connect').addEventListener('click', async () => {
    try {
        if (serial.isConnected) {
            await serial.disconnect();
        } else {
            await serial.connect(115200);
        }
    } catch (err) {
        // The user closing the port chooser is not an error worth showing.
        if (err.name !== 'NotFoundError') log(err.message, 'error');
    }
});

$('btn-permit-on').addEventListener('click', () => send('PERMIT 1'));
$('btn-permit-off').addEventListener('click', () => send('PERMIT 0'));
$('btn-home').addEventListener('click', () => send('HOME'));
$('btn-stop').addEventListener('click', () => send('STOP'));

$('btn-xyz').addEventListener('click', () => {
    const v = ['in-x', 'in-y', 'in-z'].map((id) => parseFloat($(id).value));
    if (v.some(Number.isNaN)) {
        log('X, Y and Z must be numbers.', 'error');
        return;
    }
    send(`XYZ ${v[0]} ${v[1]} ${v[2]}`);
});

$('btn-xyz-copy').addEventListener('click', () => {
    if (!info) return;
    $('in-x').value = info.x;
    $('in-y').value = info.y;
    $('in-z').value = info.z;
});

// One row per joint: name, range, target field, Go, and step buttons.
JOINT_NAMES.forEach((name, i) => {
    const row = document.createElement('div');
    row.className = 'row';
    row.innerHTML = `
        <span class="joint-name">${name}<br><span class="joint-range" id="jr-range-${i}">--</span></span>
        <button class="btn btn-secondary btn-jog need-conn" data-dir="-1" aria-label="${name} minus">&minus;</button>
        <input id="jr-in-${i}" class="field num" type="number" step="1" aria-label="${name} angle">
        <button class="btn btn-secondary btn-jog need-conn" data-dir="1" aria-label="${name} plus">+</button>
        <button class="btn btn-primary need-conn" data-go="1">Go</button>`;
    const input = row.querySelector('input');
    const go = (angle) => {
        if (Number.isNaN(angle)) return;
        delete input.dataset.edited;
        input.value = angle;
        send(`J ${i + 1} ${angle}`);
    };
    input.addEventListener('input', () => { input.dataset.edited = '1'; });
    input.addEventListener('keydown', (e) => { if (e.key === 'Enter') go(parseFloat(input.value)); });
    row.querySelector('[data-go]').addEventListener('click', () => go(parseFloat(input.value)));
    row.querySelectorAll('[data-dir]').forEach((b) => b.addEventListener('click', () => {
        if (!info) return;
        const step = parseFloat($('sel-step').value) * parseFloat(b.dataset.dir);
        const [lo, hi] = info.limits[i];
        go(Math.min(hi, Math.max(lo, +(info.target[i] + step).toFixed(1))));
    }));
    $('joint-rows').appendChild(row);
});

// ---------------- Configuration ----------------

['cfg-ssid', 'cfg-pass', 'cfg-gwhost', 'cfg-gwport'].forEach((id) => {
    $(id).addEventListener('input', () => { cfgDirty = true; });
});

$('btn-show-pass').addEventListener('click', () => {
    const field = $('cfg-pass');
    const show = field.type === 'password';
    field.type = show ? 'text' : 'password';
    $('btn-show-pass').textContent = show ? 'Hide' : 'Show';
});

$('btn-reload-cfg').addEventListener('click', () => {
    cfgDirty = false;
    fillConfig();
});

$('btn-save').addEventListener('click', async () => {
    const ssid = $('cfg-ssid').value.trim();
    const pass = $('cfg-pass').value;
    const host = $('cfg-gwhost').value.trim();
    const port = parseInt($('cfg-gwport').value, 10);
    const hint = $('cfg-hint');

    if (!ssid) { hint.textContent = 'Enter the Wi-Fi name.'; return; }
    if (pass && (pass.length < 8 || pass.length > 63)) { hint.textContent = 'The Wi-Fi password must be 8 to 63 characters.'; return; }
    if (!host) { hint.textContent = 'Enter the gateway host (IP address of the Ignition PC).'; return; }
    if (!(port >= 1 && port <= 65535)) { hint.textContent = 'The gateway port must be 1 to 65535.'; return; }

    await send(`SET SSID ${ssid}`);
    if (pass) await send(`SET PASS ${pass}`);
    await send(`SET GWHOST ${host}`);
    await send(`SET GWPORT ${port}`);
    hint.textContent = 'Saved on the board. Press Reboot to connect with the new settings.';
    cfgDirty = false;
});

$('btn-reboot').addEventListener('click', () => {
    if (confirm('Reboot the ESP32? The arm returns to its home pose and the machine to "stop".')) send('REBOOT');
});

// ---------------- Terminal ----------------

function sendTerminal() {
    const input = $('terminal-input');
    if (input.value.trim()) send(input.value);
    input.value = '';
}
$('btn-send').addEventListener('click', sendTerminal);
$('terminal-input').addEventListener('keydown', (e) => { if (e.key === 'Enter') sendTerminal(); });
$('btn-clear').addEventListener('click', () => { $('terminal-output').textContent = ''; });

// ---------------- Start ----------------

setConnectedUi(false);
if (!serial.isSupported()) {
    log('This browser has no Web Serial. Use Google Chrome or Microsoft Edge.', 'error');
    $('btn-connect').disabled = true;
}
if (demo) serial.connect();
