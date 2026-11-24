#!/usr/bin/env python3
"""Проверяет необязательный Arduino-режим настольного Python-клиента."""

import importlib.util
from pathlib import Path
import sys
import types


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "desktop" / "chat_app.py"
HTML_PATH = ROOT / "desktop" / "ui" / "index.html"

# Для теста логики настоящий pywebview не нужен.
sys.modules.setdefault("webview", types.SimpleNamespace())
spec = importlib.util.spec_from_file_location("chat_app", MODULE_PATH)
chat_app = importlib.util.module_from_spec(spec)
spec.loader.exec_module(chat_app)


assert chat_app.zabor_text("привет мир") == "ПрИвЕт МиР"
assert chat_app.zabor_text("ёжик ABC xyz") == "ЁжИк AbC xYz"
assert chat_app.zabor_text("123, тест!") == "123, ТеСт!"

assert chat_app.zabor_chat_line("Иван: привет мир") == "Иван: ПрИвЕт МиР"
assert chat_app.zabor_chat_line("@Иван: секрет") == "@Иван: СеКрЕт"
assert chat_app.zabor_chat_line("[Сервер] Пользователи в чате (2):") is None
assert chat_app.zabor_chat_line("  1. Иван") is None
assert chat_app.zabor_chat_line("строка без имени") is None

assert chat_app.DEFAULT_SETTINGS["com_port"] == ""

arduino_port = types.SimpleNamespace(
    device="/dev/cu.usbmodem1101", description="Arduino Leonardo",
    manufacturer="Arduino LLC", hwid="USB VID:PID=2341:8036",
)
bluetooth_port = types.SimpleNamespace(
    device="/dev/cu.Bluetooth-Incoming-Port", description="Bluetooth",
    manufacturer=None, hwid="n/a",
)
assert chat_app.recommend_com_port([bluetooth_port, arduino_port]) == "/dev/cu.usbmodem1101"
assert chat_app.recommend_com_port([bluetooth_port]) is None
assert chat_app.recommend_com_port([
    arduino_port,
    types.SimpleNamespace(device="/dev/cu.usbmodem1201", description="USB modem", manufacturer=None, hwid="n/a"),
]) == "/dev/cu.usbmodem1101"
assert chat_app.recommend_com_port([
    arduino_port,
    types.SimpleNamespace(device="/dev/cu.usbmodem1201", description="Arduino Uno", manufacturer="Arduino", hwid="VID:PID=2341:0043"),
]) is None


class FakePort:
    def __init__(self):
        self.writes = []
        self.closed = False

    def write(self, data):
        self.writes.append(data)

    def close(self):
        self.closed = True


port = FakePort()
arduino = chat_app.ArduinoOutput(lambda *args, **kwargs: port)
arduino.open("COM_TEST")
arduino.send_chat_line("Иван: привет")
arduino.send_chat_line("[Сервер] служебная строка")
arduino.flush_for_test()
assert port.writes == ["Иван: ПрИвЕт\n".encode("utf-8")], port.writes
arduino.close()
assert port.closed

# Сервер не возвращает отправителю его же сообщения, поэтому свои сообщения
# клиент сам отправляет на Arduino (иначе один клиент не видел бы ничего).
own_port = FakePort()
api = chat_app.Api()
api._arduino = chat_app.ArduinoOutput(lambda *args, **kwargs: own_port)
api._arduino.open("COM_TEST")
api._my_name = "Иван"
api._session = types.SimpleNamespace(send=lambda text: None)

assert api.send("привет мир") == {"ok": True}
api._arduino.flush_for_test()
assert own_port.writes == ["Иван: ПрИвЕт МиР\n".encode("utf-8")], own_port.writes

own_port.writes.clear()
api.send("#users")                      # команды на Arduino не идут
api.send("#ToUser Мария секрет")
api._arduino.flush_for_test()
assert own_port.writes == [], own_port.writes

api._push_line("[Сервер] Ваше новое имя: Пётр")   # ответ на #NewName
api._arduino.flush_for_test()
assert api._my_name == "Пётр" and own_port.writes == [], (api._my_name, own_port.writes)
api.send("тест")
api._arduino.flush_for_test()
assert own_port.writes == ["Пётр: ТеСт\n".encode("utf-8")], own_port.writes
api._arduino.close()

html = HTML_PATH.read_text(encoding="utf-8")
assert 'id="com"' in html
assert "Без Arduino" in html
assert "list_com_ports" in html

print("OK: Arduino в desktop-клиенте необязательна, ЗаБоР работает")
