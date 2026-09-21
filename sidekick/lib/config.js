const { readFileSync, writeFileSync, mkdirSync, renameSync } = require('node:fs');
const { dirname } = require('node:path');
const { BUTTON_EVENTS, WINDOW_ACTIONS } = require('./buttons');
const DEFAULTS = { configUrl: '', serialPort: '' };

function httpUrl(value) {
    const url = new URL(value);
    if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password) {
        throw new Error('Use an HTTP or HTTPS URL without embedded credentials.');
    }
    return url.href;
}
function validateConfig(value) {
    if (!value || typeof value.configUrl !== 'string' || typeof value.serialPort !== 'string') {
        throw new Error('Enter a config URL and an optional USB port.');
    }
    return { configUrl: httpUrl(value.configUrl.trim()), serialPort: value.serialPort.trim() };
}
function loadConfig(filename, env = process.env) {
    let saved = {};
    try { saved = JSON.parse(readFileSync(filename, 'utf8')); }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
    // A config endpoint cannot be inferred from the previous page/data URLs.
    const value = { configUrl: env.SIDEKICK_CONFIG_URL || saved.configUrl || '', serialPort: saved.serialPort || '' };
    if (!value.configUrl) return { ...DEFAULTS, serialPort: value.serialPort.trim() };
    return validateConfig(value);
}
function remoteConfig(value, configUrl) {
    if (!value || value.success !== true || value.error !== null || !value.data?.endpoints) throw new Error('Invalid config response');
    const resolve = key => {
        const link = value.data.endpoints[key];
        if (typeof link !== 'string' || !link.trim()) throw new Error(`Config is missing endpoints.${key}`);
        return httpUrl(new URL(link, configUrl).href);
    };
    const buttons = {};
    for (const name of BUTTON_EVENTS) {
        const action = value.data.endpoints[name];
        if (action == null) continue;
        if (typeof action === 'object' && !Array.isArray(action) && action.action === 'request') {
            if (!['GET', 'POST'].includes(action.type) || typeof action.url !== 'string' || !action.url.trim() ||
                Object.keys(action).some(key => !['action', 'type', 'url'].includes(key))) {
                throw new Error(`Invalid request action: ${name}; specify type GET or POST and a URL`);
            }
            buttons[name] = { action: 'request', type: action.type, url: httpUrl(new URL(action.url, configUrl).href) };
        }
        else if (typeof action === 'object' && !Array.isArray(action) && WINDOW_ACTIONS.includes(action.action) && Object.keys(action).length === 1) buttons[name] = { action: action.action };
        else throw new Error(`Invalid button action: ${name}`);
    }
    return { pageUrl: resolve('page'), dataUrl: resolve('info'), buttons };
}
async function fetchConfig(fetch, url, signal) {
    const response = await fetch(url, { credentials: 'include', redirect: 'follow', cache: 'no-store', headers: { Accept: 'application/json' }, signal });
    if (!response.ok) throw new Error(`Config endpoint returned HTTP ${response.status}`);
    if (!/\bapplication\/(?:[\w.-]+\+)?json\b/i.test(response.headers.get('content-type') || '')) throw new Error('Config endpoint must return JSON; it may require login');
    const text = await response.text();
    if (text.length > 65536) throw new Error('Config response is too large');
    return remoteConfig(JSON.parse(text), url);
}
function saveConfig(filename, value) {
    const config = validateConfig(value);
    mkdirSync(dirname(filename), { recursive: true });
    writeFileSync(`${filename}.tmp`, JSON.stringify(config, null, 2) + '\n', { mode: 0o600 });
    renameSync(`${filename}.tmp`, filename);
    return config;
}
module.exports = { DEFAULTS, httpUrl, validateConfig, loadConfig, saveConfig, remoteConfig, fetchConfig };
