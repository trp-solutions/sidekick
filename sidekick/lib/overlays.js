const { utcTime } = require('./timers');
const SIZE = 160;
function validateOverlays(value) {
    if (value === undefined) return [];
    if (!Array.isArray(value) || value.length > 8) throw new Error('Expected at most 8 overlays');
    const ids = new Set();
    return value.map(item => {
        if (!item || !['text', 'timer'].includes(item.type)) throw new Error('Expected a text or timer overlay');
        let content;
        if (item.type === 'text') {
            if (typeof item.text !== 'string' || item.text.length > 1000) throw new Error('Expected text with at most 1000 characters');
            content = { type: 'text', text: item.text };
        } else {
            const { id, direction, state, anchorTime, valueSeconds, format = 'mm:ss' } = item;
            if (typeof id !== 'string' || !id.trim() || id.length > 100 || ids.has(id)) throw new Error('Timer IDs must be nonempty and unique');
            ids.add(id);
            if (!['up', 'down'].includes(direction) || !['running', 'paused'].includes(state)) throw new Error('Invalid timer direction or state');
            utcTime(anchorTime, 'anchorTime');
            if (typeof valueSeconds !== 'number' || !Number.isFinite(valueSeconds) || valueSeconds < 0 || valueSeconds > 315360000) throw new Error('Timer valueSeconds must be between 0 and 315360000');
            if (!['mm:ss', 'hh:mm:ss'].includes(format)) throw new Error('Invalid timer format');
            content = { type: 'timer', id, direction, state, anchorTime, valueSeconds, format };
        }
        const { position = 'center', fontSize = 20, color = '#FFFFFF', backgroundColor } = item;
        if (!['top', 'center', 'bottom'].includes(position)) throw new Error('Invalid overlay position');
        if (!Number.isInteger(fontSize) || fontSize < 8 || fontSize > 64) throw new Error('Overlay fontSize must be 8–64 pixels');
        const validColor = value => typeof value === 'string' && /^#[0-9a-f]{6}([0-9a-f]{2})?$/i.test(value);
        if (!validColor(color) || (backgroundColor !== undefined && !validColor(backgroundColor))) throw new Error('Overlay colors must be #RRGGBB or #RRGGBBAA');
        return { ...content, position, fontSize, color, ...(backgroundColor === undefined ? {} : { backgroundColor }) };
    });
}
// Composite a small RGBA layer at transmission time; never modify cached video frames.
function compositeFrame(frame, layer) {
    if (!layer) return frame;
    const output = Buffer.from(frame);
    for (let pixel = 0; pixel < SIZE * SIZE; pixel++) {
        const i = pixel * 4;
        const alpha = layer[i + 3] / 255;
        if (!alpha) continue;
        const packed = frame.readUInt16BE(pixel * 2);
        const r = Math.round(layer[i] * alpha + ((packed >> 11) * 255 / 31) * (1 - alpha));
        const g = Math.round(layer[i + 1] * alpha + (((packed >> 5) & 63) * 255 / 63) * (1 - alpha));
        const b = Math.round(layer[i + 2] * alpha + ((packed & 31) * 255 / 31) * (1 - alpha));
        output.writeUInt16BE((Math.round(r * 31 / 255) << 11) | (Math.round(g * 63 / 255) << 5) | Math.round(b * 31 / 255), pixel * 2);
    }
    return output;
}
module.exports = { validateOverlays, compositeFrame };
