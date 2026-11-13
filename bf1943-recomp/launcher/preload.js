const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('launcher', {
  state: () => ipcRenderer.invoke('state'),
  play: (settings) => ipcRenderer.invoke('play', settings),
  minimize: () => ipcRenderer.send('minimize'),
  close: () => ipcRenderer.send('close'),
  onGameExited: (fn) => ipcRenderer.on('game-exited', fn),
  setupStatus: (game) => ipcRenderer.invoke('setup-status', game),
  setupPick: (game) => ipcRenderer.invoke('setup-pick', game),
  setupRun: (game, gamePath) => ipcRenderer.invoke('setup-run', game, gamePath),
  onSetupEvent: (fn) => ipcRenderer.on('setup-event', (_, ev) => fn(ev)),
  texturesMake: () => ipcRenderer.invoke('textures-make'),
  texturesCancel: () => ipcRenderer.invoke('textures-cancel'),
  onTexturesEvent: (fn) => ipcRenderer.on('textures-event', (_, ev) => fn(ev)),
});
