const path = require('node:path');
const fs = require('node:fs');
const { nativeImage, nativeTheme } = require('electron');

const icons = path.join(__dirname, '..', 'icons', 'generated');

function createTrayIcon() {
    // Windows can use different themes for applications and the taskbar.
    const dark = process.platform === 'win32'
        ? nativeTheme.shouldUseDarkColorsForSystemIntegratedUI
        : nativeTheme.shouldUseDarkColors;
    const suffix = dark ? '-white' : '';
    // Keep the ICO path: Windows loads its size variants natively.
    if (process.platform === 'win32') return path.join(icons, `sidekick${suffix}.ico`);
    if (process.platform === 'darwin') {
        // Electron also loads the @2x representation for Retina displays.
        const image = nativeImage.createFromPath(path.join(icons, 'sidekickTemplate.png'));
        image.setTemplateImage(true);
        return image;
    }
    const image = nativeImage.createEmpty();
    for (const size of [24, 16, 22, 32]) {
        image.addRepresentation({
            scaleFactor: size / 24,
            buffer: fs.readFileSync(path.join(icons, `linux-${size}${suffix}.png`)),
        });
    }
    return image;
}

module.exports = { createTrayIcon };
