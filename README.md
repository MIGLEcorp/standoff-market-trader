# Standoff Market Trader

Сейчас скрипт может анализировать текущую цену запроса, цену последнего лота и пытаться ловить передачи на рынке Standoff 2.

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

## Результат
<img width="731" height="398" alt="result" src="https://github.com/user-attachments/assets/6f32758d-4603-467e-86de-85ab99709030" />
