const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { startsInBackground, setOpenAtLogin, desktopExec } = require('../lib/startup');
const { loadConfig, saveConfig, validateConfig } = require('../lib/config');

function temporaryDirectory(t) {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'sidekick-startup-'));
    t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
    return directory;
}

test('existing settings default to enabled; disabling persists without a config URL', t => {
    const filename = path.join(temporaryDirectory(t), 'settings.json');
    assert.equal(loadConfig(filename, {}).openAtLogin, true);
    fs.writeFileSync(filename, JSON.stringify({ configUrl: 'https://example.com', serialPort: '' }));
    assert.equal(loadConfig(filename, {}).openAtLogin, true);
    saveConfig(filename, { configUrl: '', serialPort: '', openAtLogin: false });
    assert.equal(loadConfig(filename, {}).openAtLogin, false);
    assert.throws(() => validateConfig({ configUrl: '', serialPort: '', openAtLogin: 'false' }));
});

test('login launches are hidden but manual launches are visible', () => {
    const app = { getLoginItemSettings: () => ({ wasOpenedAtLogin: true }) };
    assert.equal(startsInBackground(app, ['sidekick'], 'win32'), false);
    assert.equal(startsInBackground(app, ['sidekick', '--background'], 'win32'), true);
    assert.equal(startsInBackground(app, ['sidekick', '--background'], 'linux'), true);
    assert.equal(startsInBackground(app, ['sidekick'], 'darwin'), true);
    app.getLoginItemSettings = () => ({ wasOpenedAtLogin: false });
    assert.equal(startsInBackground(app, ['sidekick'], 'darwin'), false);
});

test('Windows uses a background argument; macOS uses native login registration', () => {
    const calls = [];
    const app = { isPackaged: true, setLoginItemSettings: value => calls.push(value) };
    for (const enabled of [true, false]) {
        setOpenAtLogin(app, enabled, { platform: 'win32', executable: 'C:\\Program Files\\Sidekick.exe' });
        assert.deepEqual(calls.pop(), { openAtLogin: enabled, path: 'C:\\Program Files\\Sidekick.exe', args: ['--background'] });
        setOpenAtLogin(app, enabled, { platform: 'darwin' });
        assert.deepEqual(calls.pop(), { openAtLogin: enabled });
    }
    app.isPackaged = false;
    setOpenAtLogin(app, true, { platform: 'win32' });
    assert.equal(calls.length, 0);
});

test('Linux uses the stable AppImage path and removes only its own entry', t => {
    const directory = temporaryDirectory(t);
    const app = { isPackaged: true, getPath: () => directory };
    const options = { platform: 'linux', env: { XDG_CONFIG_HOME: directory, APPIMAGE: '/Apps/Sidekick 1.2.AppImage' },
        executable: '/tmp/.mount_sidekick/sidekick' };
    setOpenAtLogin(app, true, options);
    const filename = path.join(directory, 'autostart', 'trp.solutions.sidekick.desktop');
    const other = path.join(directory, 'autostart', 'other.desktop');
    fs.writeFileSync(other, 'unchanged');
    assert.match(fs.readFileSync(filename, 'utf8'), /Exec="\/Apps\/Sidekick 1\.2\.AppImage" --background\n/);
    setOpenAtLogin(app, false, options);
    setOpenAtLogin(app, false, options);
    assert.equal(fs.existsSync(filename), false);
    assert.equal(fs.readFileSync(other, 'utf8'), 'unchanged');
    setOpenAtLogin(app, true, { platform: 'linux', env: {}, executable: '/opt/Sidekick/sidekick' });
    assert.ok(fs.existsSync(path.join(directory, '.config/autostart/trp.solutions.sidekick.desktop')));
});

test('desktop commands escape field codes and shell metacharacters', () => {
    const command = desktopExec('/Apps/100% "$HOME" `test`\\Sidekick');
    assert.ok(command.includes('100%%'));
    assert.ok(command.includes('\\\\$HOME'));
    assert.ok(command.includes('\\\\"'));
    assert.ok(command.includes('\\\\`'));
    assert.ok(command.includes('\\\\\\\\Sidekick'));
    assert.throws(() => desktopExec('/Apps/Bad\nName'));
});
