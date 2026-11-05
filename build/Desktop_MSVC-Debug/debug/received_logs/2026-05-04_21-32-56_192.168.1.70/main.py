#!/usr/bin/env python3
"""
Анализатор логов logcat: извлекает строки с ошибками (E) и предупреждениями (W).
Использование:
    python logcat_errors_warnings.py input.log output.log
    cat input.log | python logcat_errors_warnings.py > filtered.log
    python logcat_errors_warnings.py input.log  # вывод в stdout
"""

import sys
import re

# Регулярное выражение для строки logcat: дата, PID, TID, уровень (буква), остальное
# Уровень — третье поле после PID и TID (разделители: пробелы)
LOGCAT_LINE_RE = re.compile(
    r'^\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}'  # временная метка
    r'\s+\d+\s+\d+\s+'                         # PID и TID
    r'([WE])\s'                                # захватываем уровень W или E
)

def filter_logcat(input_stream, output_stream):
    """Фильтрует строки logcat, оставляя W и E."""
    for line in input_stream:
        match = LOGCAT_LINE_RE.match(line)
        if match:
            output_stream.write(line)

def main():
    # Определяем входной и выходной потоки
    if len(sys.argv) > 1:
        # Чтение из файла
        input_file = sys.argv[1]
        try:
            input_stream = open(input_file, 'r', encoding='utf-8', errors='replace')
        except FileNotFoundError:
            print(f"Ошибка: файл '{input_file}' не найден.", file=sys.stderr)
            sys.exit(1)
    else:
        # Чтение из stdin (для конвейера)
        input_stream = sys.stdin

    if len(sys.argv) > 2:
        # Запись в файл
        output_file = sys.argv[2]
        output_stream = open(output_file, 'w', encoding='utf-8')
    else:
        # Вывод в stdout
        output_stream = sys.stdout

    try:
        filter_logcat(input_stream, output_stream)
    finally:
        if input_stream is not sys.stdin:
            input_stream.close()
        if output_stream is not sys.stdout:
            output_stream.close()

if __name__ == '__main__':
    main()