; Custom NSIS sections injected by electron-builder
; Installs Morph.vst3 to the system VST3 directory and removes it on uninstall.

!macro customInstall
  CreateDirectory "$PROGRAMFILES64\Common Files\VST3"
  nsExec::ExecToLog 'cmd /C xcopy /E /I /Y "$INSTDIR\resources\Morph.vst3" "$PROGRAMFILES64\Common Files\VST3\Morph.vst3"'
!macroend

!macro customUnInstall
  RMDir /r "$PROGRAMFILES64\Common Files\VST3\Morph.vst3"
!macroend
