@echo off
setlocal
rem Run locally on the directory PC, or remotely from its shared folder.
rem Account on the directory PC. The password is requested when this script runs.
rem schtasks prompts for the password; do not put it in this shared file.
set "directoryUser=DESKTOP-S6IPCA3\Anon"
if /I "%COMPUTERNAME%"=="DESKTOP-S6IPCA3" goto localTask
echo Enter the directory account password when prompted, not its Windows Hello PIN.
schtasks /End /S DESKTOP-S6IPCA3 /U "%directoryUser%" /TN "\Directory"
goto taskResult
:localTask
schtasks /End /TN "\Directory"
:taskResult
if errorlevel 1 (
    echo.
    echo Could not stop the Directory task. No executable was changed.
    echo For remote use, enable Remote Scheduled Tasks Management in Windows
    echo Firewall on DESKTOP-S6IPCA3 and use an account allowed to manage the task.
) else (
    echo.
    echo Stop requested. Copy the new subnet-directory.exe after the task exits,
    echo then run StartDirectory.cmd. Do not replace a file that is still in use.
)
pause
