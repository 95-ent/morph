'use strict'
const { ipcRenderer, contextBridge } = require('electron')

// Polyfill window.webkit.messageHandlers so the web app's companion-bridge.tsx
// works identically to the macOS WKWebView environment.
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
