const path = require('node:path');
const { validateOverlays } = require('./overlays');
class OverlayRenderer {
    async render(value) {
        const overlays = validateOverlays(value);
        if (!overlays.length) return null;
        const key = JSON.stringify(overlays);
        if (key === this.key) return this.layer;
        if (!this.window || this.window.isDestroyed()) {
            const { BrowserWindow } = require('electron');
            this.window = new BrowserWindow({ width: 160, height: 160, show: false,
                webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false, offscreen: true, backgroundThrottling: false } });
            this.window.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
            this.window.webContents.on('will-navigate', event => event.preventDefault());
            this.loaded = this.window.loadFile(path.join(__dirname, '../overlay.html'));
        }
        await this.loaded;
        const pixels = await this.window.webContents.executeJavaScript(`window.renderOverlays(${key})`);
        this.layer = Buffer.from(pixels);
        this.key = key;
        return this.layer;
    }
    close() { this.window?.destroy(); this.window = null; this.key = null; this.layer = null; }
}
module.exports = { OverlayRenderer };
