@echo off
cd /d "%~dp0"
".\saige_desktop.exe" --scene ".\scene\saige.scene.json" %*
exit /b %ERRORLEVEL%
