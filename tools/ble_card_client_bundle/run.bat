@echo off
cd /d "%~dp0"
py ble_card_client.py
if errorlevel 1 pause
