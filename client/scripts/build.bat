C:\devkitPro\tools\bin\bannertool.exe makebanner -i banner.png -a banner.wav -o banner.bnr

C:\devkitPro\tools\bin\bannertool.exe makesmdh -i icon.png -s "Spotify 3DS" -l "Display your spotify session on the 3DS!" -p "David Abejon" -o icon.icn

C:\devkitPro\tools\bin\makerom.exe -f cia -o Spotify3DS.cia -elf spotify-3ds.elf -rsf template.rsf -icon icon.icn -banner banner.bnr