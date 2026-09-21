const { app, BrowserWindow, Tray, Menu, ipcMain, session, dialog } = require('electron');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
const { SerialPort } = require('serialport');
const { DEFAULTS, loadConfig, saveConfig, fetchConfig } = require('./lib/config');
const { DisplayController, loadVideos } = require('./lib/display');
const { StatusPoller } = require('./lib/status');
const { VideoCache } = require('./lib/videos');
const { TimerTimeline } = require('./lib/timers');
const { OverlayRenderer } = require('./lib/overlay-renderer');
const overlayRenderer = new OverlayRenderer();
const { ButtonActions, BUTTON_EVENTS } = require('./lib/buttons');
let buttonActions;
let videoCache;
let routes, configController, configError;

let mainWindow, settingsWindow, tray, display, poller, pmtSession, config, configFile, videos;
let isQuitting = false;
let offline = true;
let loading = false;
let navigation = 0;
let showingOffline = false;
let retryTimer;
let applying = false;
let usbStatus = 'Waiting for USB';
const settingsURL = pathToFileURL(path.join(__dirname, 'settings.html')).href;
const offlineURL = pathToFileURL(path.join(__dirname, 'index.html')).href;
const securePreferences = { contextIsolation: true, nodeIntegration: false, sandbox: true };

function showWindow() {
    if (mainWindow.isMinimized()) mainWindow.restore();
    mainWindow.show();
    mainWindow.focus();
}
function showSettings() {
    if (settingsWindow && !settingsWindow.isDestroyed()) return settingsWindow.focus();
    settingsWindow = new BrowserWindow({ width: 560, height: 580, parent: mainWindow,
        webPreferences: { ...securePreferences, preload: path.join(__dirname, 'preload.js') } });
    settingsWindow.setMenu(null);
    settingsWindow.webContents.on('will-navigate', event => event.preventDefault());
    settingsWindow.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
    void settingsWindow.loadFile(path.join(__dirname, 'settings.html'));
}
function trustedSettings(event) {
    if (!settingsWindow || event.sender !== settingsWindow.webContents ||
        event.senderFrame !== settingsWindow.webContents.mainFrame || event.senderFrame.url !== settingsURL) {
        throw new Error('Settings are only available from the local Settings window');
    }
}
function updateTray() {
    tray?.setToolTip(`Sidekick — ${configError ? `Config: ${configError}` : usbStatus}`.slice(0, 127));
}
async function showOffline() {
    offline = true;
    if (showingOffline || isQuitting || mainWindow.webContents.getURL() === offlineURL) return;
    showingOffline = true;
    try { await mainWindow.loadFile(path.join(__dirname, 'index.html')); }
    catch (error) { console.error('Could not load the offline page:', error.message); }
    finally { showingOffline = false; }
}
async function loadPage(force = false) {
    if (isQuitting || !routes?.pageUrl) return;
    if (loading && !force) return;
    const version = ++navigation;
    loading = true;
    try {
        await mainWindow.loadURL(routes.pageUrl);
        if (version !== navigation) return;
        offline = mainWindow.webContents.getURL() === offlineURL;
    } catch (error) {
        // A new navigation cancels the previous load; it is not a page failure.
        if (error.code === 'ERR_ABORTED' || error.errno === -3) return;
        if (!isQuitting && version === navigation) await showOffline();
    } finally {
        if (version === navigation) loading = false;
    }
}
function startPolling() {
    poller?.stop();
    display?.setState('idle');
    if (!routes?.dataUrl) return;
    const target = display;
    const cache = videoCache;
    const timeline = new TimerTimeline();
    poller = new StatusPoller({
        fetch: pmtSession.fetch.bind(pmtSession), url: routes.dataUrl,
        async onResult(result, signal) {
            if (result.reason !== 'ok') {
                target.setState('idle');
                if (result.error) {
                    usbStatus = `Data: ${result.error}`;
                    updateTray();
                    console.error('Data unavailable:', result.error);
                }
                return;
            }
            try {
                const frames = await cache.get(result.info, signal);
                if (signal.aborted) return;
                timeline.update(result.info.overlays, result.info.serverTime, result.receivedAt);
                const entries = timeline.entries;
                const render = () => overlayRenderer.render(timeline.textOverlays(result.info.overlays, entries));
                const overlay = await render();
                const dynamic = result.info.overlays.some(item => item.type === 'timer' && item.state === 'running');
                if (!signal.aborted) target.setVideo(result.info.checksum, frames, dynamic ? render : overlay, overlay);
            } catch (error) {
                if (signal.aborted) return;
                target.setState('idle');
                usbStatus = error.message;
                updateTray();
                console.error('Video unavailable:', error.message);
            }
        },
    });
    poller.start();
}
async function refreshConfig() {
    if (!config.configUrl || isQuitting || configController) return;
    const controller = configController = new AbortController();
    const deadline = setTimeout(() => controller.abort(), 5000);
    try {
        const next = await fetchConfig(pmtSession.fetch.bind(pmtSession), config.configUrl, controller.signal);
        if (configController !== controller || isQuitting) return;
        const previous = routes;
        routes = next;
        buttonActions?.stop();
        buttonActions = new ButtonActions({
            fetch: pmtSession.fetch.bind(pmtSession), actions: routes.buttons,
            windowActions: {
                toggleWindow() { if (mainWindow.isVisible() && !mainWindow.isMinimized()) mainWindow.hide(); else showWindow(); },
                showWindow,
                hideWindow() { mainWindow.hide(); },
            },
            onComplete() { poller?.refresh(); },
            onError(message) { usbStatus = `Button: ${message}`; updateTray(); console.error('Button action failed:', message); },
        });
        configError = null;
        updateTray();
        const dataChanged = previous?.dataUrl !== next.dataUrl;
        if (dataChanged) poller?.stop();
        const pageLoad = previous?.pageUrl !== next.pageUrl || offline ? loadPage(true) : null;
        if (dataChanged) startPolling();
        await pageLoad;
    } catch (error) {
        if (configController !== controller || isQuitting) return;
        configError = error.message;
        updateTray();
        console.error('Config unavailable:', error.message);
        if (!routes) await showOffline();
    } finally {
        clearTimeout(deadline);
        if (configController === controller) configController = null;
    }
}
async function applyConfig(next) {
    buttonActions?.stop();
    configController?.abort();
    configController = null;
    routes = null;
    configError = null;
    navigation++;
    poller?.stop();
    if (display) await display.stop();
    config = next;
    videoCache = new VideoCache({ directory: path.join(app.getPath('userData'), 'videos'), fetch: pmtSession.fetch.bind(pmtSession) });
    display = new DisplayController({ SerialPort, videos, path: config.serialPort,
        onButton(event) {
            const name = BUTTON_EVENTS[event - 1];
            const action = routes?.buttons[name];
            const description = action ? (action.action === 'request' ? `${action.type} request` : action.action) : 'disabled';
            console.info(`[Button] ${name}: ${description}`);
            void buttonActions?.trigger(event);
        },
        onStatus(message) { usbStatus = message; updateTray(); } });
    display.start();
    await refreshConfig();
}

if (!app.requestSingleInstanceLock()) app.quit();
else {
    app.on('second-instance', () => { if (mainWindow) showWindow(); });
    app.whenReady().then(async () => {
        configFile = path.join(app.getPath('userData'), 'settings.json');
        try { config = loadConfig(configFile); }
        catch (error) {
            config = { ...DEFAULTS };
            dialog.showErrorBox('Sidekick settings', `Using default settings: ${error.message}`);
        }
        videos = loadVideos(path.join(__dirname, 'media'));
        pmtSession = session.fromPartition('persist:sidekick-pmt');
        pmtSession.setPermissionRequestHandler((_webContents, _permission, callback) => callback(false));
        pmtSession.setPermissionCheckHandler(() => false);
        mainWindow = new BrowserWindow({ width: 800, height: 600,
            webPreferences: { ...securePreferences, session: pmtSession } });
        mainWindow.on('close', event => { if (!isQuitting) { event.preventDefault(); mainWindow.hide(); } });
        mainWindow.webContents.on('will-navigate', (event, url) => {
            if (!/^https?:\/\//i.test(url)) event.preventDefault();
        });
        mainWindow.webContents.setWindowOpenHandler(({ url }) => {
            // Open web links in the same isolated session, including login links.
            if (/^https?:\/\//i.test(url)) void mainWindow.loadURL(url).catch(() => showOffline());
            return { action: 'deny' };
        });
        mainWindow.webContents.on('did-navigate', (_event, url, status) => {
            if (/^https?:\/\//i.test(url) && status >= 400) void showOffline();
            else if (/^https?:\/\//i.test(url)) offline = false;
        });
        mainWindow.webContents.on('did-fail-load', (_event, code, _description, _url, isMainFrame) => {
            if (isMainFrame && code !== -3 && !isQuitting) void showOffline();
        });
        mainWindow.webContents.on('render-process-gone', () => { void showOffline(); });
        const menuItems = [
            { label: 'Show Sidekick', click: showWindow },
            { label: 'Settings…', click: showSettings },
            { label: 'Reconnect', click: () => { void refreshConfig().then(() => loadPage()); } },
            { type: 'separator' },
            { label: 'Quit', click: () => app.quit() },
        ];
        tray = new Tray(path.join(__dirname, 'icon.png'));
        tray.setContextMenu(Menu.buildFromTemplate(menuItems));
        tray.on('click', () => mainWindow.isVisible() ? mainWindow.hide() : showWindow());
        Menu.setApplicationMenu(Menu.buildFromTemplate([
            { label: 'Sidekick', submenu: menuItems },
            { label: 'Edit', submenu: [{ role: 'undo' }, { role: 'redo' }, { role: 'cut' }, { role: 'copy' }, { role: 'paste' }, { role: 'selectAll' }] },
        ]));
        ipcMain.handle('settings:get', event => { trustedSettings(event); return config; });
        ipcMain.handle('settings:save', async (event, value) => {
            trustedSettings(event);
            if (applying) throw new Error('Reconnection is already in progress');
            applying = true;
            try {
                const next = saveConfig(configFile, value);
                await applyConfig(next);
                if (configError) throw new Error(`Settings saved. ${configError}. Retrying automatically.`);
                return next;
            } finally { applying = false; }
        });
        await applyConfig(config);
        if (!config.configUrl) { await showOffline(); showSettings(); }
        retryTimer = setInterval(() => {
            if (!routes || configError) void refreshConfig();
            else if (offline) void loadPage();
        }, 10000);
        app.on('activate', showWindow);
    }).catch(error => { dialog.showErrorBox('Sidekick could not start', error.message); app.quit(); });
    app.on('window-all-closed', () => {});
    app.on('before-quit', event => {
        if (isQuitting) return;
        event.preventDefault();
        isQuitting = true;
        clearInterval(retryTimer);
        buttonActions?.stop();
        configController?.abort();
        overlayRenderer.close();
        poller?.stop();
        Promise.resolve(display?.stop()).finally(() => app.quit());
    });
}
