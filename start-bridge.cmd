@echo off
rem SAX-CE Bridge: starts the relay (lets the bridge page reach IRC) and
rem opens the bridge page. Keep this window open while you chat.
cd /d "%~dp0"
where node >nul 2>nul || (
  echo Node.js is needed: https://nodejs.org/ ^(version 18 or newer^)
  pause
  exit /b 1
)
title SAX-CE relay
node relay\relay.mjs %*
if errorlevel 1 pause
