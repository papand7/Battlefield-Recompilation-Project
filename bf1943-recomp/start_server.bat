@echo off
rem Battlefield 1943 dedicated server: hosts on your network (UDP 1003, browsers on UDP 19430).
rem Edit the name, maps and bots below. Maps: Wake_island_s Iwo_Jima_s Guadal_Canal Coral_sea
set GAME=original\v106
set NAME=My Server
set MAPS=Wake_island_s,Iwo_Jima_s,Guadal_Canal,Coral_sea
set BOTS=0
rem PUBLISH=1: also listed on the internet (the master server), not only on your network;
rem your router must pass UDP ports 1003 and 19430 to this PC.
set PUBLISH=0
rem BOT_LLM=1: in difficult spots (stuck, losing a fight) the bots ask a small vision model
rem (Qwen3.5 4B through llama.cpp, run on this PC) where to go. Installed into llm\ the first
rem time (about 3.5 GB; needs Python). Faster with a graphics card.
set BOT_LLM=0
rem TRAIN=1: a training run instead: nobody can join, 16 bots, the game 4x as fast, the bots
rem ask about more (with BOT_LLM=1) and learn; it ends after TRAIN_MINUTES (real time) and
rem writes a viewer of the whole run: server_data\bot_advice\runs\<map>_<date>\index.html
set TRAIN=0
set TRAIN_MINUTES=30
set EXTRA=
if "%PUBLISH%"=="1" set EXTRA=--publish
if "%TRAIN%"=="1" set EXTRA=--train --speed 4 --train-minutes %TRAIN_MINUTES%
cd /d "%~dp0"
if not "%BOT_LLM%"=="1" goto run
if exist llm\bot_llm.ini goto llm
python tools\setup_bot_llm.py --dir llm || py -3 tools\setup_bot_llm.py --dir llm || goto run
:llm
set EXTRA=%EXTRA% --bot-llm llm
:run
build\game\bf1943.exe --game "%GAME%" --user server_data --dedicated --name "%NAME%" --maps %MAPS% --bots %BOTS% %EXTRA%
