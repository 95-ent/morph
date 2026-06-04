; Custom NSIS sections injected by electron-builder
; Installs Morph.vst3 to the user VST3 directory (no admin required).
; Standard user VST3 path: %APPDATA%\VST3\ — recognized by FL Studio, Ableton,
; Bitwig, Studio One, Reaper, and most modern DAWs on Windows.

!macro customInit
  ; Kill any running companion process BEFORE NSIS pages load.
  ; This prevents the "cannot be closed" dialog entirely.
  ; productName has shipped as BOTH "Water Morph" and "Water", so the running
  ; process may be "Water Morph.exe" (current/legacy) OR "Water.exe" (interim
  ; build). Kill BOTH so any prior install is cleanly closed before overwrite.
  ; /F = force, /T = kill child processes (Electron spawns GPU/renderer helpers).
  nsExec::ExecToLog 'cmd /C taskkill /F /T /IM "Water Morph.exe" 2>nul & taskkill /F /T /IM "Water.exe" 2>nul & exit 0'
  ; Wait for OS to fully release file handles.
  Sleep 1200
!macroend

!macro customInstall
  ; Safety net: kill again in case the process restarted (e.g. autostart).
  nsExec::ExecToLog 'cmd /C taskkill /F /T /IM "Water Morph.exe" 2>nul & taskkill /F /T /IM "Water.exe" 2>nul & exit 0'
  Sleep 500
  ; $APPDATA = C:\Users\<user>\AppData\Roaming (no admin required)
  CreateDirectory "$APPDATA\VST3"
  nsExec::ExecToLog 'cmd /C xcopy /E /I /Y "$INSTDIR\resources\Morph.vst3" "$APPDATA\VST3\Morph.vst3"'
!macroend

!macro customUnInstall
  RMDir /r "$APPDATA\VST3\Morph.vst3"
!macroend
