'use strict'

const { app, BrowserWindow, ipcMain, Tray, Menu, nativeImage, shell } = require('electron')
const net  = require('net')
const path = require('path')
const fs   = require('fs')
const os   = require('os')

// ── Constants ────────────────────────────────────────────────────────────────

const kPort    = 59812
const kBaseURL = 'https://water.95ent.ai'
const kUA      = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) WaterMorphCompanion/1.0'

// ── Single-instance guard ────────────────────────────────────────────────────

const gotLock = app.requestSingleInstanceLock()
if (!gotLock) { app.quit(); process.exit(0) }

// Register watermorph:// protocol handler (runtime fallback — installer also handles this)
app.setAsDefaultProtocolClient('watermorph')

// ── State ────────────────────────────────────────────────────────────────────

let win   = null
let tray  = null
let dawState = { bpm: 0, key: '', isConnected: false }

// ── Window ───────────────────────────────────────────────────────────────────

function createWindow() {
  // titleBarStyle 'hidden' doesn't work correctly on Windows (content area
  // doesn't fill under the title bar). Use titleBarOverlay for Windows
  // custom chrome, which keeps a native draggable area but lets us color it.
  const isMac = process.platform === 'darwin'
  win = new BrowserWindow({
    width:           420,
    height:          700,
    minWidth:        340,
    minHeight:       480,
    frame:           true,
    titleBarStyle:   isMac ? 'hidden' : 'hiddenInset',
    titleBarOverlay: isMac ? false : {
      color:       '#0a0a0a',
      symbolColor: '#8B5CF6',   // mauve — Water brand
      height:      28,
    },
    backgroundColor: '#0a0a0a',
    title:           'Water Morph',
    skipTaskbar:     false,
    webPreferences: {
      preload:            path.join(__dirname, 'preload.js'),
      contextIsolation:   true,
      nodeIntegration:    false,
      devTools:           true,
    }
  })

  win.loadURL(`${kBaseURL}/library`, {
    userAgent: kUA,
    extraHeaders: 'X-Water-Companion: windows\n'
  })

  win.webContents.on('before-input-event', (_event, input) => {
    if (input.type === 'keyDown' && input.key === 'I' && input.control && input.shift) {
      win?.webContents.toggleDevTools()
    }
  })

  win.webContents.on('did-fail-load', () => {
    // Retry after 3s on network failure
    setTimeout(() => win?.loadURL(`${kBaseURL}/library`, { userAgent: kUA }), 3000)
  })

  win.on('close', (e) => {
    // Minimize to tray on close
    e.preventDefault()
    win.hide()
  })
}

// ── Tray ─────────────────────────────────────────────────────────────────────

function createTray() {
  // Use a 16x16 transparent icon — replace icon.ico with a real icon in production
  const icon = nativeImage.createEmpty()
  tray = new Tray(icon)
  tray.setToolTip('Water Morph')
  updateTrayMenu()
  tray.on('click', () => {
    if (win?.isVisible()) { win.focus() } else { win?.show() }
  })
}

function updateTrayMenu() {
  const conn = dawState.isConnected
    ? `Connected — ${dawState.bpm > 0 ? dawState.bpm + ' BPM' : ''} ${dawState.key}`
    : 'DAW not connected'
  const menu = Menu.buildFromTemplate([
    { label: 'Water Morph', enabled: false },
    { label: conn, enabled: false },
    { type: 'separator' },
    { label: 'Show Library', click: () => { win?.show(); win?.focus() } },
    { type: 'separator' },
    { label: 'Quit', click: () => { app.exit(0) } }
  ])
  tray.setContextMenu(menu)
}

// ── Register autostart ────────────────────────────────────────────────────────

function registerAutostart() {
  app.setLoginItemSettings({
    openAtLogin:    true,
    openAsHidden:   true,
    name:           'WaterMorphHelper',
    path:           app.getPath('exe'),
  })
}

// ── TCP server ────────────────────────────────────────────────────────────────

function startTCPServer() {
  const server = net.createServer((conn) => {
    conn.write('READY\n')
    let pending = ''

    conn.on('data', (chunk) => {
      pending += chunk.toString('utf8')
      let nl
      while ((nl = pending.indexOf('\n')) !== -1) {
        const line = pending.slice(0, nl).trim()
        pending = pending.slice(nl + 1)
        handleLine(conn, line)
      }
    })

    conn.on('error', () => {})
  })

  server.listen(kPort, '127.0.0.1', () => {
    console.log(`[Morph] TCP listening on 127.0.0.1:${kPort}`)
  })

  server.on('error', (err) => {
    console.error('[Morph] TCP server error:', err.message)
    // Retry after 2s (another instance may have just exited)
    setTimeout(startTCPServer, 2000)
  })
}

function handleLine(conn, line) {
  if (line === 'PING') {
    conn.write('PONG\n')

  } else if (line.startsWith('DRAG ')) {
    // On Windows with VST3, drag is handled natively in the plugin.
    // The companion still sends OK to keep the plugin happy.
    conn.write('OK\n')

  } else if (line === 'SHOW_WINDOW') {
    // Plugin clicked "Open Water" — bring companion window to front
    if (win) { win.show(); win.focus() }
    conn.write('OK\n')

  } else if (line.startsWith('SYNC ')) {
    const parts = line.slice(5).split(' ')
    const bpm   = parseFloat(parts[0] || '0') || 0
    const key   = (parts[1] === '?' || !parts[1]) ? '' : parts[1]
    dawState = { bpm, key, isConnected: true }
    clearTimeout(dawState._disconnectTimer)
    dawState._disconnectTimer = setTimeout(() => {
      dawState.isConnected = false
      updateTrayMenu()
    }, 5000)
    updateTrayMenu()

  } else if (line.startsWith('TRANSPORT ')) {
    const parts   = line.slice(10).split(' ')
    const playing = parts[0]?.startsWith('play') ?? false
    const timeSecs = parseFloat(parts[1] || '0') || 0
    const ppq     = parseFloat(parts[2] || '0') || 0

    dawState.isConnected = true
    updateTrayMenu()

    // Inject transport into web app
    if (win?.webContents) {
      const js = `
        if (window.__waterCompanion?.transport) {
          window.__waterCompanion.transport({ playing: ${playing}, timeSecs: ${timeSecs}, ppq: ${ppq} });
        }
      `
      win.webContents.executeJavaScript(js).catch(() => {})
    }
  }
}

// ── IPC from web app ──────────────────────────────────────────────────────────

ipcMain.on('morphCopy', async (_event, { base64, filename }) => {
  try {
    const tmpFile = path.join(os.tmpdir(), filename)
    fs.writeFileSync(tmpFile, Buffer.from(base64, 'base64'))
    // Open the temp folder so user can drag the file into the DAW.
    // Phase 2: native CF_HDROP clipboard integration for direct Ctrl+V paste.
    shell.showItemInFolder(tmpFile)
  } catch (err) {
    console.error('[Morph] morphCopy error:', err)
  }
})

ipcMain.on('morphPlayerState', (_event, state) => {
  const title = state.name
    ? `${state.name}${state.bpm ? ' · ' + state.bpm + ' BPM' : ''}${state.key ? ' · ' + state.key : ''}`
    : 'Water Morph'
  win?.setTitle(title)
  tray?.setToolTip(title)
})

ipcMain.on('morphHover',  (_event, _id)  => { /* no-op on Windows */ })
ipcMain.on('morphSelect', (_event, _ids) => { /* no-op on Windows */ })

// ── App lifecycle ─────────────────────────────────────────────────────────────

app.whenReady().then(() => {
  createWindow()
  createTray()
  startTCPServer()
  registerAutostart()
})

app.on('second-instance', (_event, argv) => {
  // Handle watermorph:// deep-link from second instance
  const url = argv.find((a) => a.startsWith('watermorph://'))
  if (url) handleProtocolUrl(url)
  if (win) { win.show(); win.focus() }
})

app.on('open-url', (_event, url) => {
  handleProtocolUrl(url)
})

function handleProtocolUrl(url) {
  if (!url.startsWith('watermorph://')) return
  if (win) { win.show(); win.focus() }
}

app.on('window-all-closed', (e) => {
  e.preventDefault()  // Keep running in tray
})
