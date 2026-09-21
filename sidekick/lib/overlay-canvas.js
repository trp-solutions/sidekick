// Local, sandboxed canvas renderer. Text is always drawn literally, never as HTML.
window.renderOverlays = overlays => {
    const canvas = document.querySelector('canvas');
    const context = canvas.getContext('2d', { willReadFrequently: true });
    context.clearRect(0, 0, 160, 160);
    const margin = 8;
    const padding = 4;
    const maxWidth = 160 - 2 * (margin + padding);
    for (const overlay of overlays) {
        if (!overlay.text.trim()) continue;
        context.font = `${overlay.fontSize}px sans-serif`;
        context.textAlign = 'center';
        context.textBaseline = 'middle';
        const lines = [];
        for (const paragraph of overlay.text.replace(/\r\n?/g, '\n').split('\n')) {
            let line = '';
            for (const word of paragraph.split(/\s+/).filter(Boolean)) {
                const candidate = line ? `${line} ${word}` : word;
                if (context.measureText(candidate).width <= maxWidth) { line = candidate; continue; }
                if (line) { lines.push(line); line = ''; }
                // Long words wrap too; segment graphemes to preserve emoji and accents.
                for (const { segment } of new Intl.Segmenter(undefined, { granularity: 'grapheme' }).segment(word)) {
                    if (line && context.measureText(line + segment).width > maxWidth) { lines.push(line); line = ''; }
                    line += segment;
                }
            }
            lines.push(line);
        }
        const lineHeight = Math.ceil(overlay.fontSize * 1.2);
        const maxLines = Math.max(1, Math.floor((160 - 2 * (margin + padding)) / lineHeight));
        if (lines.length > maxLines) {
            lines.length = maxLines;
            let last = Array.from(lines[maxLines - 1]);
            while (last.length && context.measureText(last.join('') + '…').width > maxWidth) last.pop();
            lines[maxLines - 1] = last.join('') + '…';
        }
        const height = lines.length * lineHeight + padding * 2;
        const width = Math.min(maxWidth, Math.max(...lines.map(line => context.measureText(line).width))) + padding * 2;
        const top = overlay.position === 'top' ? margin : overlay.position === 'bottom' ? 160 - margin - height : (160 - height) / 2;
        context.save();
        context.beginPath();
        context.rect(margin, margin, 160 - margin * 2, 160 - margin * 2);
        context.clip();
        if (overlay.backgroundColor) {
            context.fillStyle = overlay.backgroundColor;
            context.fillRect((160 - width) / 2, top, width, height);
        }
        context.fillStyle = overlay.color;
        lines.forEach((line, index) => context.fillText(line, 80, top + padding + (index + 0.5) * lineHeight, maxWidth));
        context.restore();
    }
    return Array.from(context.getImageData(0, 0, 160, 160).data);
};
