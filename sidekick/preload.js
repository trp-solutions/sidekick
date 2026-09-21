const { contextBridge, ipcRenderer } = require('electron');
// Only the local settings window uses this preload. The PMT site has no IPC bridge.
contextBridge.exposeInMainWorld('settings', {
    get: () => ipcRenderer.invoke('settings:get'),
    save: value => ipcRenderer.invoke('settings:save', value),
});
