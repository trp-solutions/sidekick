const { performance } = require('node:perf_hooks');

function utcTime(value, field) {
    if (typeof value !== 'string' || !/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,9})?Z$/.test(value) || !Number.isFinite(Date.parse(value))) {
        throw new Error(`${field} must be a UTC ISO timestamp ending in Z`);
    }
    const canonical = value.includes('.')
        ? value.replace(/\.(\d{1,9})Z$/, (_match, fraction) => `.${fraction.slice(0, 3).padEnd(3, '0')}Z`)
        : value.replace('Z', '.000Z');
    if (new Date(canonical).toISOString() !== canonical) throw new Error(`${field} must be a valid UTC date`);
    return Date.parse(canonical);
}
function formatTimer(value, direction, format) {
    const seconds = Math.max(0, direction === 'down' ? Math.ceil(value) : Math.floor(value));
    const pad = value => String(value).padStart(2, '0');
    const minutes = Math.floor(seconds / 60);
    return format === 'hh:mm:ss'
        ? `${pad(Math.floor(minutes / 60))}:${pad(minutes % 60)}:${pad(seconds % 60)}`
        : `${pad(minutes)}:${pad(seconds % 60)}`;
}
function currentValue(entry, now) {
    const elapsed = Math.max(0, now - entry.baseTime) / 1000;
    // Slew clock corrections at up to 100 ms per second. Polls never jump
    // an unchanged timer directly to the newest server reading.
    const correction = Math.sign(entry.correction) * Math.min(Math.abs(entry.correction), elapsed * 0.1);
    return Math.max(0, entry.baseValue + entry.rate * elapsed + correction);
}
class TimerTimeline {
    constructor(now = () => performance.now()) { this.now = now; this.entries = new Map(); }
    update(overlays, serverTime, receivedAt) {
        const now = this.now();
        const next = new Map();
        for (const overlay of overlays) {
            if (overlay.type !== 'timer') continue;
            const anchor = utcTime(overlay.anchorTime, 'anchorTime');
            const serverNow = serverTime + now - receivedAt;
            const rate = overlay.state === 'paused' ? 0 : overlay.direction === 'up' ? 1 : -1;
            const desired = Math.max(0, overlay.valueSeconds + rate * Math.max(0, serverNow - anchor) / 1000);
            const signature = JSON.stringify([overlay.direction, overlay.state, overlay.anchorTime, overlay.valueSeconds]);
            const previous = this.entries.get(overlay.id);
            const continuing = previous?.signature === signature;
            const value = continuing ? currentValue(previous, now) : desired;
            next.set(overlay.id, { signature, rate, baseValue: value,
                baseTime: now + (rate ? Math.max(0, anchor - serverNow) : 0),
                correction: continuing && rate ? desired - value : 0 });
        }
        this.entries = next;
    }
    textOverlays(overlays, entries = this.entries) {
        const now = this.now();
        return overlays.map(overlay => overlay.type === 'timer'
            ? { ...overlay, type: 'text', text: formatTimer(currentValue(entries.get(overlay.id), now), overlay.direction, overlay.format) }
            : overlay);
    }
}
module.exports = { utcTime, formatTimer, TimerTimeline };
