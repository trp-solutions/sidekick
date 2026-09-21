const { readFileSync } = require('node:fs');
const { gunzipSync } = require('node:zlib');
const { join } = require('node:path');
const { performance } = require('node:perf_hooks');
const { compositeFrame } = require('./overlays');
const { Protocol } = require('./protocol');
const { retrySerialReadiness } = require('./serial-poller');

const FRAME_BYTES = 160 * 160 * 2;
const FPS = 10;
const VIDEOS = { idle: 'colors-idle' };
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

function loadVideos(directory) {
    return Object.fromEntries(Object.entries(VIDEOS).map(([state, name]) => {
        const data = gunzipSync(readFileSync(join(directory, `${name}.rgb565.gz`)));
        if (!data.length || data.length % FRAME_BYTES) throw new Error(`Invalid video: ${name}`);
        return [state, data];
    }));
}

function writeFrame(port, pixels, timeout = 3000) {
    return new Promise((resolve, reject) => {
        let finished = false;
        let phase = 'write';
        const timer = setTimeout(() => finish(new Error(`USB ${phase} timed out`)), timeout);
        const onClose = () => finish(new Error('USB disconnected'));
        const finish = error => {
            if (finished) return;
            finished = true;
            clearTimeout(timer);
            port.removeListener('error', finish);
            port.removeListener('close', onClose);
            if (error) reject(error); else resolve();
        };
        port.once('error', finish);
        port.once('close', onClose);
        port.write(pixels, error => {
            if (finished) return;
            if (error) finish(error);
            else { phase = 'drain'; port.drain(finish); }
        });
    });
}

class DisplayController {
    constructor({ SerialPort, videos, path = '', onStatus = () => {}, onButton = () => {} }) {
        Object.assign(this, { SerialPort, videos, path, onStatus, onButton });
        this.state = 'idle';
        this.stopped = false;
    }

    setState(state) {
        if (!(state in this.videos)) throw new Error(`Unknown display state: ${state}`);
        this.state = state;
        if (state === 'idle') { this.overlay = null; this.layer = null; }
    }

    setVideo(key, data, overlay = null, initialLayer = null) {
        if (!Buffer.isBuffer(data) || !data.length || data.length % FRAME_BYTES) throw new Error('Invalid video frames');
        this.videos = { idle: this.videos.idle, [key]: data };
        this.state = key;
        this.overlay = overlay;
        this.layer = typeof overlay === 'function' ? initialLayer : overlay;
    }

    refreshOverlay() {
        const source = this.overlay;
        if (typeof source !== 'function' || this.rendering) return;
        // Never hold USB frames while Chromium renders timer text. Reuse the
        // last completed layer, and discard results from an obsolete source.
        this.rendering = Promise.resolve().then(source).then(layer => {
            if (!this.stopped && this.overlay === source) this.layer = layer;
        }).catch(error => {
            if (!this.stopped && this.overlay === source) {
                console.error('[Overlay]', error);
                this.onStatus(`Overlay: ${error.message}`);
            }
        }).finally(() => { this.rendering = null; });
    }

    start() {
        this.running = this.run();
    }

    async stop() {
        this.stopped = true;
        // Closing USB also releases a blocked write. Firmware switches to idle
        // after its two-second watchdog; never send commands into raw relay data.
        await this.close();
        await this.running;
    }

    async close() {
        const port = this.port;
        if (!port?.isOpen) return;
        // Discard queued bytes before closing, including output which a stalled
        // native drain may still be waiting to transmit.
        await new Promise(resolve => {
            const timer = setTimeout(resolve, 500);
            port.flush(() => { clearTimeout(timer); resolve(); });
        });
        if (port.isOpen) await new Promise(resolve => port.close(() => resolve()));
    }

    async run() {
        while (!this.stopped) {
            try {
                const candidates = (await this.SerialPort.list()).filter(port => port.vendorId?.toLowerCase() === '303a');
                const path = this.path || (candidates.length === 1 ? candidates[0].path : '');
                if (!path) throw new Error(candidates.length > 1 ? 'Choose a USB port in Settings' : 'Connect the ESP native USB port');
                if (this.stopped) break;
                const port = this.port = new this.SerialPort({ path, baudRate: 115200, autoOpen: false });
                let buttonsReady = false;
                const protocol = new Protocol(port, event => {
                    if (buttonsReady && !this.stopped && port.isOpen) this.onButton(event);
                });
                await new Promise((resolve, reject) => port.open(error => error ? reject(error) : resolve()));
                retrySerialReadiness(port);
                // Allow reset on open and the previous raw relay session to expire.
                await sleep(2300);
                if (this.stopped) break;
                const info = await protocol.request(2);
                if (info.readUInt16LE(0) < 1 || (info.readUInt16LE(0) === 1 && info.readUInt16LE(2) < 3)) {
                    throw new Error('Flash firmware 1.3 for the built-in USB fallback');
                }
                await protocol.request(13);
                buttonsReady = true;
                const oldFirmware = info.readUInt16LE(0) === 1 && info.readUInt16LE(2) < 5;
                this.onStatus(`Display connected: ${path}${oldFirmware ? ' (full button support requires firmware 1.5)' : ''}`);
                let playing;
                let origin;
                while (!this.stopped && port.isOpen) {
                    if (playing !== this.state) {
                        playing = this.state;
                        origin = performance.now();
                    }
                    const video = this.videos[playing];
                    const frame = Math.floor((performance.now() - origin) * FPS / 1000);
                    const offset = (frame % (video.length / FRAME_BYTES)) * FRAME_BYTES;
                    this.refreshOverlay();
                    await writeFrame(port, compositeFrame(video.subarray(offset, offset + FRAME_BYTES), this.layer));
                    await sleep(Math.max(0, origin + (frame + 1) * 1000 / FPS - performance.now()));
                }
            } catch (error) {
                if (!this.stopped) {
                    this.onStatus(error.message);
                }
            } finally {
                await this.close();
            }
            if (!this.stopped) await sleep(2000);
        }
    }
}

module.exports = { DisplayController, loadVideos, writeFrame, FRAME_BYTES, FPS, VIDEOS };
