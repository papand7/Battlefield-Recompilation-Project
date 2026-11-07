@echo off
rem Battlefield 1943 dedicated server: hosts on your network (UDP 1003, browsers on UDP 19430).
rem Edit the name, maps and bots below. Maps: Wake_island_s Iwo_Jima_s Guadal_Canal Coral_sea
set GAME=original\v106
set NAME=My Server
set MAPS=Wake_island_s,Iwo_Jima_s,Guadal_Canal,Coral_sea
set BOTS=0
build\game\bf1943.exe --game "%GAME%" --user server_data --dedicated --name "%NAME%" --maps %MAPS% --bots %BOTS%
