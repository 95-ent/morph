'use strict'
const { ipcRenderer, contextBridge } = require('electron')

// Inject a synchronous flag into the main world so the web app's isCompanion
// detection works reliably. session.setUserAgent() only affects HTTP request
// headers — navigator.userAgent in the renderer is controlled by Chromium and
// does NOT change with contextIsolation: true. This flag is the canonical signal.
contextBridge.exposeInMainWorld('__waterCompanionPlatform', 'windows')

// Expose DAW state updates from main process to React components.
// main.js sends 'daw-state' IPC when the JUCE plugin syncs BPM/Key via TCP.
contextBridge.exposeInMainWorld('__morphDAW', {
  onDawState: (cb) => ipcRenderer.on('daw-state', (_, data) => cb(data))
})

// Polyfill window.webkit.messageHandlers so companion-bridge.tsx works
// identically to the macOS WKWebView environment.
contextBridge.exposeInMainWorld('webkit', {
  messageHandlers: {
    morphCopy: {
      postMessage: (msg) => ipcRenderer.send('morphCopy', msg)
    },
    morphPlayerState: {
      postMessage: (msg) => ipcRenderer.send('morphPlayerState', msg)
    },
    morphHover: {
      postMessage: (id) => ipcRenderer.send('morphHover', id)
    },
    morphSelect: {
      postMessage: (ids) => ipcRenderer.send('morphSelect', ids)
    },
    morphInstallUpdate: {
      postMessage: () => ipcRenderer.send('morphInstallUpdate')
    }
  }
})
