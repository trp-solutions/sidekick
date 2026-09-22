const fs = require('node:fs');
const path = require('node:path');

const BACKGROUND_ARG = '--background';

function startsInBackground(app, argv = process.argv, platform = process.platform) {
    return argv.includes(BACKGROUND_ARG) ||
        (platform === 'darwin' && app.getLoginItemSettings().wasOpenedAtLogin);
}

function desktopExec(executable) {
    if (/[\r\n\0]/.test(executable)) throw new Error('The application path cannot contain line breaks.');
    // Exec quoting is followed by desktop-entry string escaping; % is a field code.
    const quoted = executable.replace(/[\\"`$]/g, '\\$&').replace(/%/g, '%%');
    return `"${quoted.replace(/\\/g, '\\\\')}" ${BACKGROUND_ARG}`;
}

function setOpenAtLogin(app, enabled, { platform = process.platform, env = process.env, executable = process.execPath } = {}) {
    // Never register the development Electron executable as a login item.
    if (!app.isPackaged) return;
    if (platform === 'win32') {
        app.setLoginItemSettings({ openAtLogin: enabled, path: executable, args: [BACKGROUND_ARG] });
    } else if (platform === 'darwin') {
        app.setLoginItemSettings({ openAtLogin: enabled });
    } else if (platform === 'linux') {
        const configHome = env.XDG_CONFIG_HOME && path.isAbsolute(env.XDG_CONFIG_HOME)
            ? env.XDG_CONFIG_HOME : path.join(app.getPath('home'), '.config');
        const directory = path.join(configHome, 'autostart');
        const filename = path.join(directory, 'trp.solutions.sidekick.desktop');
        if (!enabled) {
            fs.rmSync(filename, { force: true });
            return;
        }
        // AppImage's process.execPath points into a temporary mount.
        const command = desktopExec(env.APPIMAGE || executable);
        fs.mkdirSync(directory, { recursive: true });
        fs.writeFileSync(filename, `[Desktop Entry]\nType=Application\nName=Sidekick\nExec=${command}\nTerminal=false\n`, { mode: 0o644 });
    }
}

module.exports = { startsInBackground, setOpenAtLogin, desktopExec };
