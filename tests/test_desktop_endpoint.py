#!/usr/bin/env python3
"""Проверяет адрес учебного сервера и миграцию старых настроек desktop-клиента."""

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import types


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "desktop" / "chat_app.py"
HTML_PATH = ROOT / "desktop" / "ui" / "index.html"
NEW_HOST = "170.168.13.61"
OLD_HOST = "10.160.48.122"

# Для теста настроек настоящий pywebview не нужен.
sys.modules.setdefault("webview", types.SimpleNamespace())
spec = importlib.util.spec_from_file_location("chat_app", MODULE_PATH)
chat_app = importlib.util.module_from_spec(spec)
spec.loader.exec_module(chat_app)


assert chat_app.DEFAULT_SETTINGS["host"] == NEW_HOST
assert chat_app.DEFAULT_SETTINGS["port"] == "5777"

with tempfile.TemporaryDirectory() as directory:
    settings = Path(directory) / "settings.json"
    settings.write_text(
        json.dumps({"name": "Сергей", "host": OLD_HOST, "port": "5777"}),
        encoding="utf-8",
    )
    chat_app.SETTINGS_FILE = settings
    loaded = chat_app.Api().load_settings()
    assert loaded["host"] == NEW_HOST, loaded
    assert loaded["name"] == "Сергей"

html = HTML_PATH.read_text(encoding="utf-8")
# Старый адрес встречается ровно один раз — только в безопасной миграции настроек.
assert html.count(OLD_HOST) == 1
assert NEW_HOST in html

print("OK: desktop-клиент использует 170.168.13.61:5777")
