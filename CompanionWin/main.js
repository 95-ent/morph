'use strict'

const { app, BrowserWindow, ipcMain, Tray, Menu, nativeImage, shell, dialog } = require('electron')
const { autoUpdater } = require('electron-updater')
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

let win        = null
let tray       = null
let isQuitting = false   // set to true before any real quit so close handler doesn't fight it
let dawState   = { bpm: 0, key: '', isConnected: false }

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

  // Set UA at session level so navigator.userAgent propagates to JS in all pages,
  // not just the initial HTTP request. Without this, isCompanion detection fails
  // and the web app renders the full browser UI instead of the companion UI.
  win.webContents.session.setUserAgent(kUA)

  // Clear service worker + cache on every launch so stale Next.js HTML never
  // causes React hydration mismatch (#418) between cached server HTML and new JS.
  win.webContents.session.clearStorageData({
    storages: ['serviceworkers', 'cachestorage']
  }).catch(() => {}).finally(() => {
    win.loadURL(`${kBaseURL}/discover?companion=windows`, {
      userAgent: kUA,
      extraHeaders: 'X-Water-Companion: windows\n'
    })
  })

  win.webContents.on('before-input-event', (_event, input) => {
    if (input.type === 'keyDown' && input.key === 'I' && input.control && input.shift) {
      win?.webContents.toggleDevTools()
    }
  })

  win.webContents.on('did-fail-load', () => {
    // Retry after 3s on network failure
    setTimeout(() => win?.loadURL(`${kBaseURL}/discover?companion=windows`, { userAgent: kUA }), 3000)
  })

  // Send current DAW state and kill web chrome as soon as the page is interactive
  win.webContents.on('did-finish-load', () => {
    injectCompanionCSS()
    win.webContents.send('daw-state', {
      connected: dawState.isConnected,
      bpm: dawState.bpm,
      key: dawState.key,
    })
  })

  // Re-inject on SPA navigation (Next.js history.pushState)
  win.webContents.on('did-navigate-in-page', () => { injectCompanionCSS() })

  win.on('close', (e) => {
    // If we're doing a real quit (update install or tray Quit), let it through.
    if (isQuitting) { win = null; return }
    // Otherwise minimize to tray.
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
    { label: 'Quit', click: () => { isQuitting = true; app.exit(0) } }
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

// ── Companion CSS injection — kills web chrome, re-injects on SPA navigation ──

function injectCompanionCSS() {
  if (!win?.webContents) return
  const js = `
    (function(){
      // Patch React's removeChild/insertBefore to prevent 'parentNode' crash
      // when third-party scripts (MutationObserver, ads, extensions) remove DOM
      // nodes that React still holds references to.
      if(!window.__wmDOMPatched){
        window.__wmDOMPatched=true;
        var _rc=Element.prototype.removeChild;
        Element.prototype.removeChild=function(child){
          if(child&&child.parentNode!==this) return child;
          return _rc.call(this,child);
        };
        var _ib=Node.prototype.insertBefore;
        Node.prototype.insertBefore=function(node,child){
          if(child&&child.parentNode!==this) return node;
          return _ib.call(this,node,child);
        };
      }
      if(!document.getElementById('__wmCSS')){
        var s=document.createElement('style');
        s.id='__wmCSS';
        s.textContent='[data-bottom-nav]{display:none!important;visibility:hidden!important;height:0!important;overflow:hidden!important;pointer-events:none!important;}[data-mobile-spacer]{display:none!important;height:0!important;}#__wm_plugin_status{display:none!important;}';
        document.head.appendChild(s);
      }
      var _kill=function(){
        ['[data-bottom-nav]','[data-mobile-spacer]'].forEach(function(sel){
          document.querySelectorAll(sel).forEach(function(el){
            el.style.setProperty('display','none','important');
            el.style.setProperty('height','0','important');
            el.style.setProperty('visibility','hidden','important');
            el.style.setProperty('overflow','hidden','important');
            el.style.setProperty('pointer-events','none','important');
          });
        });
      };
      _kill();
      if(window.__wmNavInterval) clearInterval(window.__wmNavInterval);
      window.__wmNavInterval=setInterval(_kill,250);
      if(window.__wmNavObs) window.__wmNavObs.disconnect();
      window.__wmNavObs=new MutationObserver(_kill);
      window.__wmNavObs.observe(document.documentElement,{childList:true,subtree:true});
    })();
  `
  win.webContents.executeJavaScript(js).catch(() => {})
  injectDragBridge()
}

// ── Drag bridge injection — fetch interception + mousedown drag detection ──

function injectDragBridge() {
  if (!win?.webContents) return
  const js = `
    (function(){
      if(window.__wmDragBridgeInjected) return;
      window.__wmDragBridgeInjected=true;

      // Track which track ID the user last interacted with (play/hover).
      window.__morphCurrentId='';

      // Intercept fetch to:
      //  1. detect which track is being played (sets __morphCurrentId)
      //  2. cache the audio blob so startDrag can fire immediately
      if(!window.__wmFetchPatched){
        window.__wmFetchPatched=true;
        var _fetch=window.fetch;
        window.fetch=function(){
          var url=typeof arguments[0]==='string'?arguments[0]:(arguments[0]&&arguments[0].url)||'';
          var m=url.match(/\\/demo\\/([0-9a-f-]{36})/);
          if(m){
            var trackId=m[1];
            window.__morphCurrentId=trackId;
            if(window.webkit&&window.webkit.messageHandlers&&window.webkit.messageHandlers.morphHover){
              window.webkit.messageHandlers.morphHover.postMessage(trackId);
            }
            // Clone response and cache audio bytes via native bridge.
            var result=_fetch.apply(this,arguments);
            result.then(function(resp){
              resp.clone().arrayBuffer().then(function(buf){
                if(buf.byteLength>0&&window.__waterNativeDrag){
                  var bytes=new Uint8Array(buf);
                  var b64='';
                  var chunk=8192;
                  for(var i=0;i<bytes.length;i+=chunk){
                    b64+=String.fromCharCode.apply(null,bytes.subarray(i,i+chunk));
                  }
                  b64=btoa(b64);
                  var fname=url.split('/').pop()||'track.mp3';
                  window.__waterNativeDrag.cacheTrack(trackId,b64,fname);
                }
              }).catch(function(){});
            }).catch(function(){});
            return result;
          }
          return _fetch.apply(this,arguments);
        };
      }

      // Drag detection: mousedown + move > 8px threshold → trigger native startDrag.
      var _downX=0,_downY=0,_mouseDown=false;
      document.addEventListener('mousedown',function(e){ _downX=e.clientX;_downY=e.clientY;_mouseDown=true; });
      document.addEventListener('mousemove',function(e){
        if(!_mouseDown) return;
        var dx=e.clientX-_downX,dy=e.clientY-_downY;
        if(Math.sqrt(dx*dx+dy*dy)>8){
          _mouseDown=false;
          if(window.__morphCurrentId&&window.__waterNativeDrag){
            window.__waterNativeDrag.startDrag(window.__morphCurrentId);
          }
        }
      });
      document.addEventListener('mouseup',function(){ _mouseDown=false; });
    })();
  `
  win.webContents.executeJavaScript(js).catch(() => {})
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
      injectPluginStatus(false, 0, '')
      if (win?.webContents) win.webContents.send('daw-state', { connected: false, bpm: 0, key: '' })
    }, 5000)
    updateTrayMenu()
    injectPluginStatus(true, bpm, key)
    if (win?.webContents) win.webContents.send('daw-state', { connected: true, bpm, key })

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

// Track cache: id → absolute local path (populated by fetch interception + morphCopy)
const trackCache = new Map()

ipcMain.on('morphCopy', (event, { base64, filename, trackId }) => {
  try {
    const tmpFile = path.join(os.tmpdir(), filename)
    fs.writeFileSync(tmpFile, Buffer.from(base64, 'base64'))
    if (trackId) trackCache.set(trackId, tmpFile)
    // Try native startDrag — works when called during an active drag gesture.
    try {
      event.sender.startDrag({ file: tmpFile, icon: path.join(__dirname, 'assets', 'icon.ico') })
    } catch (_) {
      // Not during a drag: show the file so user can drag manually as fallback.
      shell.showItemInFolder(tmpFile)
    }
  } catch (err) {
    console.error('[Morph] morphCopy error:', err)
  }
})

// Fired from injected JS when user mousedown+moves on a track row.
ipcMain.on('morphDragStart', (event, { trackId }) => {
  const filePath = trackId && trackCache.get(trackId)
  if (filePath && fs.existsSync(filePath)) {
    try {
      event.sender.startDrag({ file: filePath, icon: path.join(__dirname, 'assets', 'icon.ico') })
    } catch (err) {
      console.error('[Morph] startDrag error:', err)
    }
  }
})

// Cache audio intercepted by injected JS on demo URL fetch responses.
ipcMain.on('morphCacheTrack', (_event, { trackId, base64, filename }) => {
  try {
    const tmpFile = path.join(os.tmpdir(), `wm_${trackId}_${filename}`)
    fs.writeFileSync(tmpFile, Buffer.from(base64, 'base64'))
    trackCache.set(trackId, tmpFile)
  } catch (err) {
    console.error('[Morph] morphCacheTrack error:', err)
  }
})

ipcMain.on('morphPlayerState', (_event, state) => {
  const title = state.name
    ? `${state.name}${state.bpm ? ' · ' + state.bpm + ' BPM' : ''}${state.key ? ' · ' + state.key : ''}`
    : 'Water Morph'
  win?.setTitle(title)
  tray?.setToolTip(title)
})

ipcMain.on('morphHover',  (_event, _id)  => { /* hover tracked in renderer via injected JS */ })
ipcMain.on('morphSelect', (_event, _ids) => { /* no-op on Windows */ })
ipcMain.on('morphInstallUpdate', () => { isQuitting = true; autoUpdater.quitAndInstall() })

// ── Auto-updater ──────────────────────────────────────────────────────────────

function setupAutoUpdater() {
  autoUpdater.autoDownload    = true   // download silently in background
  autoUpdater.autoInstallOnAppQuit = false  // we control when to install

  autoUpdater.on('update-available', (info) => {
    // Notify the web view so it can show a banner
    injectPluginStatus(dawState.isConnected, dawState.bpm, dawState.key)
    if (win?.webContents) {
      win.webContents.executeJavaScript(`
        (function() {
          var el = document.getElementById('__wm_update_banner');
          if (el) return;
          el = document.createElement('div');
          el.id = '__wm_update_banner';
          el.style.cssText = 'position:fixed;bottom:80px;right:16px;z-index:99999;background:#1a0a2e;border:1px solid #8B5CF6;border-radius:10px;padding:10px 14px;font-size:11px;font-family:system-ui,sans-serif;color:#c4b5fd;box-shadow:0 4px 20px rgba(139,92,246,0.3);cursor:pointer;';
          el.innerHTML = '<b style="display:block;margin-bottom:3px;color:#a78bfa;">Nouvelle version disponible</b>Mise à jour en cours de téléchargement…';
          document.body.appendChild(el);
        })()
      `).catch(() => {})
    }
  })

  autoUpdater.on('update-downloaded', (info) => {
    // Replace banner with "ready to install" prompt
    if (win?.webContents) {
      win.webContents.executeJavaScript(`
        (function() {
          var el = document.getElementById('__wm_update_banner');
          if (!el) return;
          el.innerHTML = '<b style="display:block;margin-bottom:3px;color:#a78bfa;">Mise à jour prête</b><span>Water Morph ${info.version} — </span><span id="__wm_install_btn" style="text-decoration:underline;cursor:pointer;color:#c4b5fd;">Redémarrer maintenant</span>';
          document.getElementById('__wm_install_btn')?.addEventListener('click', function() {
            window.webkit?.messageHandlers?.morphInstallUpdate?.postMessage({});
          });
        })()
      `).catch(() => {})
    }
    // Also offer via tray menu
    updateTrayMenu()
  })

  autoUpdater.on('error', () => { /* silent — no crash on update failure */ })

  // Check on launch, then every 4 hours
  autoUpdater.checkForUpdates().catch(() => {})
  setInterval(() => autoUpdater.checkForUpdates().catch(() => {}), 4 * 60 * 60 * 1000)
}

// ── App lifecycle ─────────────────────────────────────────────────────────────

app.whenReady().then(() => {
  createWindow()
  createTray()
  startTCPServer()
  registerAutostart()
  setupAutoUpdater()
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

app.on('before-quit', () => { isQuitting = true })

app.on('window-all-closed', (e) => {
  if (!isQuitting) e.preventDefault()  // Keep running in tray; let real quits through
})
