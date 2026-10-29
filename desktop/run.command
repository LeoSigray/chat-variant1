#!/bin/bash
# Запуск настольного клиента чата. Двойной щелчок в Finder или ./run.command в терминале.
# При первом запуске создаёт окружение .venv и ставит pywebview (нужен интернет, около 50 МБ).
cd "$(dirname "$0")" || exit 1

pause() { read -r -p "Нажмите Enter, чтобы закрыть окно..." _; }

if [ ! -x .venv/bin/python ]; then
  PY=""
  for p in /opt/homebrew/bin/python3.12 /opt/homebrew/bin/python3 /usr/local/bin/python3 python3 /usr/bin/python3; do
    if command -v "$p" >/dev/null 2>&1; then PY=$(command -v "$p"); break; fi
  done
  if [ -z "$PY" ]; then echo "Не найден Python 3."; pause; exit 1; fi
  echo "Первый запуск: создаю окружение ($PY) и ставлю pywebview..."
  if ! "$PY" -m venv .venv || ! .venv/bin/pip install --quiet --disable-pip-version-check -r requirements.txt; then
    echo "Не удалось установить зависимости."; rm -rf .venv; pause; exit 1
  fi
fi

# Старое окружение могло быть создано до добавления Arduino. Не мешаем первой
# лабораторной запускаться, даже если дополнительный pyserial установить не удалось.
if ! .venv/bin/python -c 'import serial' >/dev/null 2>&1; then
  echo "Добавляю поддержку Arduino (pyserial)..."
  .venv/bin/pip install --quiet --disable-pip-version-check 'pyserial>=3.5' || \
    echo "Предупреждение: Arduino пока недоступна, обычный чат продолжит работать."
fi

exec .venv/bin/python chat_app.py
