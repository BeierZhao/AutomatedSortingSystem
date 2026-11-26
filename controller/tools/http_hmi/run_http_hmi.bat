@echo off
cd /d "%~dp0"
python run_http_hmi.py %*
pause
