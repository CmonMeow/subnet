@echo off
setlocal
rem The scheduled task retains its account, working folder and no-login settings.
rem Account on the directory PC. The password is requested when this script runs.
rem schtasks prompts for the password; do not put it in this shared file.
set "directoryUser=DESKTOP-S6IPCA3\Anon"
if /I "%COMPUTERNAME%"=="DESKTOP-S6IPCA3" goto localTask
echo Enter the directory account password when prompted, not its Windows Hello PIN.
schtasks /Run /S DESKTOP-S6IPCA3 /U "%directoryUser%" /TN "\Directory"
goto taskResult
:localTask
schtasks /Run /TN "\Directory"
:taskResult
if errorlevel 1 (
    echo.
    echo Could not start the Directory task.
    echo For remote use, enable Remote Scheduled Tasks Management in Windows
    echo Firewall on DESKTOP-S6IPCA3 and use an account allowed to manage the task.
) else (
    echo.
    echo Start requested. This does not confirm the directory started successfully.
)
pause
