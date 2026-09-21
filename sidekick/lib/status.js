const { performance } = require('node:perf_hooks');
const { utcTime } = require('./timers');
const { validateOverlays } = require('./overlays');
const { httpUrl } = require('./config');

function videoInfo(value, url) {
    if (!value || value.success !== true || value.error !== null || !value.data) throw new Error('Unsuccessful video response');
    const { video, checksum, downloadUrl } = value.data;
    if (typeof video !== 'string' || !/^[^/\\\x00-\x1f]+\.mp4$/i.test(video)) throw new Error('Expected an MP4 filename');
    if (typeof checksum !== 'string' || !/^[a-f0-9]{64}$/i.test(checksum)) throw new Error('Expected a SHA-256 checksum');
    if (downloadUrl != null && (typeof downloadUrl !== 'string' || !downloadUrl.trim())) throw new Error('Invalid download URL');
    const overlays = validateOverlays(value.data.overlays);
    const serverTime = overlays.some(overlay => overlay.type === 'timer') ? utcTime(value.serverTime, 'serverTime') : undefined;
    return { serverTime, overlays, video, checksum: checksum.toLowerCase(), downloadUrl: downloadUrl == null ? null : httpUrl(new URL(downloadUrl, url).href) };
}
async function fetchStatus(fetch, url, signal) {
    const response = await fetch(url, {
        credentials: 'include', redirect: 'follow', cache: 'no-store',
        headers: { Accept: 'application/json' }, signal,
    });
    if ([301, 302, 303, 307, 308, 401, 403].includes(response.status)) return { reason: 'login' };
    if (!response.ok) throw new Error(`Data API returned HTTP ${response.status}`);
    if (!/\bapplication\/(?:[\w.-]+\+)?json\b/i.test(response.headers.get('content-type') || '')) return { reason: 'login' };
    const text = await response.text();
    if (text.length > 65536) throw new Error('Video response is too large');
    return { reason: 'ok', info: videoInfo(JSON.parse(text), url), receivedAt: performance.now() };
}
class StatusPoller {
    constructor({ fetch, url, onResult, interval = 2000, timeout = 5000 }) {
        Object.assign(this, { fetch, url, onResult, interval, timeout });
        this.stopped = true;
    }
    start() { if (!this.stopped) return; this.stopped = false; void this.tick(); }
    refresh() {
        if (this.stopped) return;
        clearTimeout(this.timer);
        if (this.running) this.refreshPending = true;
        else void this.tick();
    }
    stop() { this.stopped = true; clearTimeout(this.timer); this.controller?.abort(); }
    async tick() {
        if (this.stopped || this.running) return;
        this.running = true;
        this.refreshPending = false;
        this.controller = new AbortController();
        const deadline = setTimeout(() => this.controller.abort(), this.timeout);
        try {
            const result = await fetchStatus(this.fetch, this.url, this.controller.signal);
            clearTimeout(deadline);
            if (!this.stopped) await this.onResult(result, this.controller.signal);
        } catch (error) {
            if (!this.stopped) await this.onResult({ reason: 'unavailable', error: error.message }, this.controller.signal);
        } finally {
            clearTimeout(deadline);
            this.running = false;
            if (!this.stopped) this.timer = setTimeout(() => this.tick(), this.refreshPending ? 0 : this.interval);
        }
    }
}
module.exports = { videoInfo, fetchStatus, StatusPoller };
