# Standoff Market Trader

Сейчас скрипт может анализировать текущую цену запроса и цену последнего лота на рынке Standoff 2.

## Компиляция

### 1. Установить компилятор

```bash
winget install --id BrechtSanders.WinLibs.MCF.UCRT -e
```

### 2. Скомпилировать

```bash
gcc market_ocr.c -O2 -std=c11 -lole32 -lwindowscodecs -lgdi32 -luser32 -o market_ocr.exe
```

## Использование

После компиляции запустите программу:

```bash
market_ocr.exe
```