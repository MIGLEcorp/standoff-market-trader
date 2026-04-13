# standoff-market-trader
Сейчас скрипт может анализировать текущую цену запроса и цену последнего лота на рынке standoff 2
Компиляция
1. Установить компилятор
  winget install --id BrechtSanders.WinLibs.MCF.UCRT -e 
2. Скомпилировать
  gcc market_ocr.c -O2 -std=c11 -lole32 -lwindowscodecs -lgdi32 -luser32 -o market_ocr.exe
