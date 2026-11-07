const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('launcher', {
  state: () => ipcRenderer.invoke('state'),
  play: (settings) => ipcRenderer.invoke('play', settings),
  minimize: () => ipcRenderer.send('minimize'),
  close: () => ipcRenderer.send('close'),
  onGameExited: (fn) => ipcRenderer.on('game-exited', fn),
});
