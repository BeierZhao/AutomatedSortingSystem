// Web Serial manager for the arm controller panel.
// Derived from the ArmTest 3D GUI (gui/serial.js): same line reader and
// FIFO write queue, without the Nano-specific telemetry parsing.

class SerialManager {
    constructor() {
        this.port = null;
        this.reader = null;
        this.writer = null;
        this.encoder = new TextEncoder();
        this.decoder = new TextDecoder();
        this.isConnected = false;

        this._txQueue = [];
        this._isSending = false;

        // Callback hooks
        this.onConnect = null;
        this.onDisconnect = null;
        this.onLine = null;   // (line) one received line, trimmed
        this.onTx = null;     // (cmd) one sent command
        this.onError = null;
    }

    isSupported() {
        return 'serial' in navigator;
    }

    async connect(baudRate = 115200) {
        if (!this.isSupported()) {
            throw new Error('Web Serial is not supported in this browser. Use Google Chrome or Microsoft Edge.');
        }
        this.port = await navigator.serial.requestPort();
        await this.port.open({ baudRate: parseInt(baudRate, 10) });

        // Release DTR/RTS so the ESP32 auto-reset circuit lets the board run.
        try {
            await this.port.setSignals({ dataTerminalReady: false, requestToSend: false });
        } catch (e) { /* not every adapter supports it */ }

        this.writer = this.port.writable.getWriter();
        this.isConnected = true;
        if (this.onConnect) this.onConnect();
        this.readLoop();
        return true;
    }

    async disconnect() {
        if (!this.port) return;
        this.isConnected = false;
        while (this._txQueue.length > 0) {
            this._txQueue.shift().resolve(false);
        }
        try {
            if (this.reader) {
                await this.reader.cancel();
            }
            if (this.writer) {
                this.writer.releaseLock();
                this.writer = null;
            }
            await this.port.close();
        } catch (err) {
            if (this.onError) this.onError(err);
        }
        this.port = null;
        if (this.onDisconnect) this.onDisconnect();
    }

    async readLoop() {
        let buffer = '';
        try {
            this.reader = this.port.readable.getReader();
            while (this.isConnected) {
                const { value, done } = await this.reader.read();
                if (done) break;
                if (!value) continue;
                buffer += this.decoder.decode(value, { stream: true });
                const lines = buffer.split(/\r?\n/);
                buffer = lines.pop();
                for (const line of lines) {
                    const trimmed = line.trim();
                    if (trimmed.length > 0 && this.onLine) this.onLine(trimmed);
                }
            }
        } catch (error) {
            // Cable pulled or board reset: report and fall through to cleanup.
            if (this.isConnected && this.onError) this.onError(error);
        } finally {
            if (this.reader) {
                try { this.reader.releaseLock(); } catch (e) {}
                this.reader = null;
            }
            if (this.isConnected) this.disconnect();
        }
    }

    // Queues one command line. Resolves true once written, false if dropped.
    send(cmd) {
        const trimmed = cmd.trim();
        if (!this.isConnected || !this.writer || !trimmed) return Promise.resolve(false);
        return new Promise((resolve) => {
            this._txQueue.push({ cmd: trimmed, resolve });
            this._processTxQueue();
        });
    }

    async _processTxQueue() {
        if (this._isSending || this._txQueue.length === 0) return;
        this._isSending = true;
        const item = this._txQueue.shift();
        try {
            if (this.onTx) this.onTx(item.cmd);
            await this.writer.write(this.encoder.encode(item.cmd + '\n'));
            item.resolve(true);
        } catch (err) {
            if (this.onError) this.onError(new Error(`Failed to send [${item.cmd}]: ${err.message}`));
            item.resolve(false);
        } finally {
            this._isSending = false;
            if (this._txQueue.length > 0) setTimeout(() => this._processTxQueue(), 5);
        }
    }
}

// Stand-in for a board, used when the page is opened with ?demo, so the
// panel can be tried without hardware. It answers the same commands.
class DemoSerial {
    constructor() {
        this.isConnected = false;
        this.onConnect = this.onDisconnect = this.onLine = this.onTx = this.onError = null;
        this.s = {
            app: 'arm4dof', status: 'stop', permit: false, manual: false, blocked: false, moving: false,
            joints: [0, 67, 32, 0], target: [0, 67, 32, 0],
            limits: [[-90, 90], [25, 110], [5, 50], [-90, 90]],
            x: 186.2, y: 0, z: 46.7,
            wifi: { connected: true, ssid: 'demo-wifi', ip: '192.168.1.50', rssi: -52, gwhost: '192.168.1.10', gwport: 8080,
                    stpath: '/scada/report-current-status', alpath: '/scada/report-alarm' },
            cfg: { ssid: 'demo-wifi', pass_set: true, gwhost: '192.168.1.10', gwport: 8080,
                   stpath: '/scada/report-current-status', alpath: '/scada/report-alarm', pending: false },
            distance: 183, alarm: false,
            joystick: { raw_x: 2048, raw_y: 2048, centre_x: 2048, centre_y: 2048, x: 0, y: 0, sw: false }
        };
    }
    isSupported() { return true; }
    async connect() { this.isConnected = true; if (this.onConnect) this.onConnect(); return true; }
    async disconnect() { this.isConnected = false; if (this.onDisconnect) this.onDisconnect(); }
    _fk() {
        const d = Math.PI / 180, [yaw, b, a] = this.s.joints;
        const r = 140 * Math.cos(b * d) + 155 * Math.cos(a * d);
        this.s.x = +(r * Math.cos(yaw * d)).toFixed(1);
        this.s.y = +(r * Math.sin(yaw * d)).toFixed(1);
        this.s.z = +(140 * Math.sin(b * d) - 155 * Math.sin(a * d)).toFixed(1);
    }
    send(cmd) {
        if (!this.isConnected) return Promise.resolve(false);
        if (this.onTx) this.onTx(cmd);
        const reply = (line) => setTimeout(() => this.onLine && this.onLine(line), 20);
        const up = cmd.toUpperCase();
        const s = this.s;
        if (up === 'INFO') {
            s.distance = 150 + Math.round(40 * Math.sin(Date.now() / 1500));
            // A stick slowly moved in a circle.
            const t = Date.now() / 2000;
            const j = s.joystick;
            j.x = +(0.8 * Math.cos(t)).toFixed(2);
            j.y = +(0.8 * Math.sin(t)).toFixed(2);
            j.raw_x = Math.round(2048 + 2047 * j.x);
            j.raw_y = Math.round(2048 + 2047 * j.y);
            j.sw = Math.floor(t) % 4 === 0;
            reply('#INFO:' + JSON.stringify(s));
        } else if (up.startsWith('PERMIT ')) {
            s.permit = up.endsWith('1');
            s.status = s.permit ? 'idle' : 'stop';
            reply('[Nano] Machine status: ' + s.status);
        } else if (up.startsWith('J ')) {
            const [, n, ang] = cmd.split(/\s+/);
            const lim = s.limits[n - 1];
            if (!lim || +ang < lim[0] || +ang > lim[1]) {
                reply('#J:ERR outside joint limits');
            } else {
                s.joints[n - 1] = s.target[n - 1] = +ang;
                this._fk();
                reply(`#J:OK ${n} ${ang}`);
            }
        } else if (up === 'HOME') {
            s.joints = [0, 67, 32, 0]; s.target = [0, 67, 32, 0];
            this._fk();
            reply('[Nano] Moved to HOME position.');
        } else if (up.startsWith('SET ')) {
            const m = cmd.match(/^SET\s+(\S+)\s*(.*)$/i);
            const key = m[1].toUpperCase();
            if (key === 'SSID') s.cfg.ssid = m[2];
            if (key === 'PASS') s.cfg.pass_set = m[2].length > 0;
            if (key === 'GWHOST') s.cfg.gwhost = m[2];
            if (key === 'GWPORT') s.cfg.gwport = +m[2];
            if (key === 'STPATH') s.cfg.stpath = m[2] || '/scada/report-current-status';
            if (key === 'ALPATH') s.cfg.alpath = m[2] || '/scada/report-alarm';
            s.cfg.pending = true;
            reply(`#CFG:OK ${key} stored, REBOOT to apply`);
        } else if (up === 'REBOOT') {
            s.wifi.ssid = s.cfg.ssid; s.wifi.gwhost = s.cfg.gwhost; s.wifi.gwport = s.cfg.gwport;
            s.wifi.stpath = s.cfg.stpath; s.wifi.alpath = s.cfg.alpath;
            s.cfg.pending = false; s.permit = false; s.status = 'stop';
            reply('#CFG:OK rebooting');
        } else {
            reply('[demo] ' + cmd);
        }
        return Promise.resolve(true);
    }
}
