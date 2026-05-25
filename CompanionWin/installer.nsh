; Custom NSIS sections injected by electron-builder
; Installs Morph.vst3 to the user VST3 directory (no admin required).
; Standard user VST3 path: %APPDATA%\VST3\ — recognized by FL Studio, Ableton,
; Bitwig, Studio One, Reaper, and most modern DAWs on Windows.

!macro customInstall
  ; $APPDATA = C:\Users\<user>\AppData\Roaming (no admin required)
  CreateDirectory "$APPDATA\VST3"
  nsExec::ExecToLog 'cmd /C xcopy /E /I /Y "$INSTDIR\resources\Morph.vst3" "$APPDATA\VST3\Morph.vst3"'
!macroend

!macro customUnInstall
  RMDir /r "$APPDATA\VST3\Morph.vst3"
!macroend
