#!/usr/bin/env python3
"""Настольный клиент чата (вариант 1): окно в стиле «жидкое стекло» + подключение к настоящему серверу.

Окно рисует системный WebView (pywebview), интерфейс лежит в ui/index.html. Сеть делает Python:
тот же протокол, что у client.c — сервер приглашает представиться, клиент отвечает «#NewName имя»,
дальше обмен строками UTF-8, каждая заканчивается символом '\\n'.

Запуск:  python chat_app.py          (или двойной щелчок по run.command)
"""
import json
import queue
import re
import socket
import sys
import threading
import time
from pathlib import Path

import webview

HERE = Path(__file__).resolve().parent
UI_FILE = HERE / "ui" / "index.html"
ICON_FILE = HERE / "assets" / "icon.png"
SETTINGS_FILE = Path.home() / ".chat_variant1.json"
DEFAULT_HOST = "170.168.13.61"
LEGACY_HOSTS = {"10.160.48.122"}
DEFAULT_SETTINGS = {"name": "", "host": DEFAULT_HOST, "port": "5777", "com_port": ""}
MAX_LINE_BYTES = 1024          # как MSG_MAX_BYTES в common.h
HANDSHAKE_TIMEOUT = 8.0        # секунд на приветствие и регистрацию имени
SERIAL_BAUD = 9600


class Rejected(Exception):
    """Сервер не принял подключение или имя (текст можно показать пользователю)."""


class ArduinoError(Exception):
    """Не удалось открыть или использовать COM-порт Arduino."""


def zabor_text(text):
    """Чередует регистр букв, начиная с верхнего: «привет» -> «ПрИвЕт»."""
    result = []
    upper = True
    for char in str(text):
        if char.isalpha():
            result.append(char.upper() if upper else char.lower())
            upper = not upper
        else:
            result.append(char)
    return "".join(result)


def zabor_chat_line(line):
    """Готовит пользовательскую строку чата для Arduino; служебные строки пропускает."""
    line = str(line)
    if not line or line[0].isspace() or line.startswith("[Сервер] ") or ": " not in line:
        return None
    name, text = line.split(": ", 1)
    if not name or not text:
        return None
    return "%s: %s" % (name, zabor_text(text))


class ArduinoOutput:
    """Необязательный асинхронный вывод строк в Arduino через COM-порт."""

    def __init__(self, serial_factory=None):
        self._serial_factory = serial_factory
        self._port = None
        self._queue = queue.Queue(maxsize=64)
        self._thread = None
        self._lock = threading.Lock()
        self._error = None

    def open(self, name, baud=SERIAL_BAUD):
        self.close()
        factory = self._serial_factory
        if factory is None:
            try:
                import serial
            except ImportError as exc:
                raise ArduinoError("не установлен модуль pyserial") from exc
            factory = serial.Serial
        try:
            port = factory(name, baudrate=baud, timeout=0, write_timeout=1)
        except Exception as exc:
            raise ArduinoError("не удалось открыть %s: %s" % (name, exc)) from exc
        with self._lock:
            self._port = port
            self._error = None
        self._thread = threading.Thread(target=self._write_loop, daemon=True)
        self._thread.start()

    def _write_loop(self):
        while True:
            data = self._queue.get()
            try:
                if data is None:
                    return
                with self._lock:
                    port = self._port
                if port is not None:
                    port.write(data)
            except Exception as exc:
                with self._lock:
                    self._error = str(exc)
            finally:
                self._queue.task_done()

    def send_chat_line(self, line):
        transformed = zabor_chat_line(line)
        with self._lock:
            enabled = self._port is not None and self._error is None
        if not transformed or not enabled:
            return
        data = (transformed + "\n").encode("utf-8")
        try:
            self._queue.put_nowait(data)
        except queue.Full:
            try:
                self._queue.get_nowait()
                self._queue.task_done()
            except queue.Empty:
                pass
            try:
                self._queue.put_nowait(data)
            except queue.Full:
                pass

    def flush_for_test(self):
        """Дожидается очереди; используется автоматическими тестами."""
        self._queue.join()

    def close(self):
        thread = self._thread
        with self._lock:
            port, self._port = self._port, None
        if thread is not None and thread.is_alive():
            while True:
                try:
                    self._queue.put_nowait(None)
                    break
                except queue.Full:
                    try:
                        self._queue.get_nowait()
                        self._queue.task_done()
                    except queue.Empty:
                        break
            thread.join(timeout=2)
        self._thread = None
        if port is not None:
            try:
                port.close()
            except Exception:
                pass


def list_com_ports():
    """Возвращает доступные последовательные порты; без pyserial — пустой список."""
    try:
        from serial.tools import list_ports
        return [port.device for port in list_ports.comports()]
    except Exception:
        return []


class LineReader:
    """Собирает строки из TCP-потока: данные приходят кусками или слитно."""

    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def readline(self, timeout=None):
        deadline = None if timeout is None else time.time() + timeout
        while b"\n" not in self.buf:
            if deadline is None:
                self.sock.settimeout(None)
            else:
                remaining = deadline - time.time()
                if remaining <= 0:
                    raise socket.timeout()
                self.sock.settimeout(remaining)
            data = self.sock.recv(4096)
            if not data:
                raise ConnectionError("closed")
            self.buf += data
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode("utf-8", "replace").rstrip("\r")


class Session:
    """Одно подключение к серверу чата."""

    def __init__(self, on_line, on_closed):
        self._on_line = on_line
        self._on_closed = on_closed
        self._lock = threading.Lock()
        self._sock = None
        self._reader = None
        self._gen = 0

    def connect(self, host, port, name):
        """Подключается и регистрирует имя. Возвращает строки сервера, пришедшие при входе."""
        self.disconnect()
        sock = socket.create_connection((host, port), timeout=6)
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            reader = LineReader(sock)
            lines = []
            while True:                                           # приветствие и приглашение
                line = reader.readline(HANDSHAKE_TIMEOUT)
                if "переполнен" in line:
                    raise Rejected("Сервер переполнен, попробуйте подключиться позже")
                if line.startswith("[Сервер] Представьтесь"):
                    break
                lines.append(line)
            sock.sendall(("#NewName %s\n" % name).encode("utf-8"))
            while True:                                           # ответ на #NewName
                line = reader.readline(HANDSHAKE_TIMEOUT)
                if line.startswith("[Сервер] Имя не принято:"):
                    reason = line.split(":", 1)[1].strip()
                    reason = re.sub(r"\s*(Выберите другое|Попробуйте ещё раз).*$", "", reason)
                    raise Rejected(reason[:1].upper() + reason[1:])
                lines.append(line)
                if line.startswith("[Сервер] Вы вошли в чат"):
                    break
        except Exception:
            sock.close()
            raise
        with self._lock:
            self._sock, self._reader = sock, reader
            self._gen += 1
        return lines

    def start(self):
        """Начинает передавать входящие строки в интерфейс (после того, как он готов их принять)."""
        with self._lock:
            sock, reader, gen = self._sock, self._reader, self._gen
        if sock is not None:
            threading.Thread(target=self._pump, args=(sock, reader, gen), daemon=True).start()

    def _pump(self, sock, reader, gen):
        try:
            while True:
                self._on_line(reader.readline())
        except (OSError, ConnectionError):
            pass
        with self._lock:
            current = self._gen == gen and self._sock is sock
            if current:
                self._sock = None
        try:
            sock.close()
        except OSError:
            pass
        if current:                      # соединение оборвал сервер, а не пользователь
            self._on_closed()

    def send(self, text):
        with self._lock:
            sock = self._sock
        if sock is None:
            raise ConnectionError("not connected")
        sock.sendall(text.encode("utf-8") + b"\n")

    def disconnect(self):
        with self._lock:
            sock, self._sock = self._sock, None
            self._gen += 1
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()


class Api:
    """Методы, которые интерфейс вызывает из JavaScript (window.pywebview.api.*).
    Всё служебное начинается с «_»: pywebview такое наружу не отдаёт."""

    def __init__(self):
        self._window = None
        self._arduino = ArduinoOutput()
        self._session = Session(self._push_line, self._push_closed)

    def _bind(self, window):
        self._window = window

    def _js(self, code):
        try:
            self._window.evaluate_js(code)
        except Exception:
            pass                          # окно уже закрывается

    def _push_line(self, text):
        self._arduino.send_chat_line(text)
        self._js("window.onChatLine(%s)" % json.dumps(text, ensure_ascii=False))

    def _push_closed(self):
        self._arduino.close()
        self._js("window.onChatClosed()")

    # --- настройки: помним последнее имя и адрес сервера ---
    def load_settings(self):
        try:
            data = json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            data = {}
        settings = {k: str(data.get(k, v)) for k, v in DEFAULT_SETTINGS.items()}
        # Старый учебный сервер больше не доступен. Обновляем только известный
        # старый адрес, не перетирая адреса, которые пользователь ввёл сам.
        if settings["host"] in LEGACY_HOSTS:
            settings["host"] = DEFAULT_HOST
        return settings

    def save_settings(self, name, host, port, com_port=""):
        try:
            SETTINGS_FILE.write_text(
                json.dumps(dict(name=name, host=host, port=str(port), com_port=str(com_port)), ensure_ascii=False),
                encoding="utf-8",
            )
        except OSError:
            pass
        return {"ok": True}

    def list_com_ports(self):
        return {"ok": True, "ports": list_com_ports()}

    # --- чат ---
    def connect(self, name, host, port, com_port=""):
        name, host, com_port = str(name).strip(), str(host).strip(), str(com_port).strip()
        try:
            port = int(port)
        except (TypeError, ValueError):
            port = 0
        if not host or len(host) > 255 or re.search(r"\s", host):
            return {"ok": False, "error": "Укажите адрес сервера"}
        if not 1 <= port <= 65535:
            return {"ok": False, "error": "Порт — число от 1 до 65535"}
        # имя уходит в команду «#NewName имя»: никаких пробелов и управляющих символов
        if not re.fullmatch(r"\S{1,64}", name) or any(ord(c) < 32 for c in name):
            return {"ok": False, "error": "Имя — одно слово без пробелов"}
        self._arduino.close()
        if com_port:
            try:
                self._arduino.open(com_port)
            except ArduinoError as e:
                return {"ok": False, "error": "Arduino: %s" % e}
        try:
            lines = self._session.connect(host, port, name)
        except Rejected as e:
            self._arduino.close()
            return {"ok": False, "error": str(e)}
        except socket.timeout:
            self._arduino.close()
            return {"ok": False, "error": "Сервер не ответил вовремя"}
        except ConnectionRefusedError:
            self._arduino.close()
            return {"ok": False, "error": "Сервер отклонил подключение. Проверьте адрес, порт и что сервер запущен"}
        except socket.gaierror:
            self._arduino.close()
            return {"ok": False, "error": "Не удалось найти адрес «%s»" % host}
        except ConnectionError:
            self._arduino.close()
            return {"ok": False, "error": "Сервер закрыл соединение"}
        except OSError as e:
            self._arduino.close()
            return {"ok": False, "error": "Не удалось подключиться: %s" % (e.strerror or e)}
        return {"ok": True, "lines": lines}

    def start(self):
        self._session.start()
        return {"ok": True}

    def send(self, text):
        text = re.sub(r"[\r\n]+", " ", str(text)).strip()
        if not text:
            return {"ok": True}
        if len(text.encode("utf-8")) > MAX_LINE_BYTES:
            return {"ok": False, "error": "Сообщение длиннее %d байт" % MAX_LINE_BYTES}
        try:
            self._session.send(text)
        except (OSError, ConnectionError):
            return {"ok": False, "error": "Нет соединения с сервером"}
        return {"ok": True}

    def disconnect(self):
        self._session.disconnect()
        self._arduino.close()
        return {"ok": True}


def build_html():
    """Страница интерфейса с пометкой, что она работает внутри настольного приложения."""
    return '<script>window.APP_MODE = "desktop";</script>' + UI_FILE.read_text(encoding="utf-8")


def create_window(width=1100, height=780):
    api = Api()
    window = webview.create_window(
        "Клиент чата", html=build_html(), js_api=api,
        width=width, height=height, min_size=(480, 620),
        background_color="#d9ecff", text_select=True,
    )
    api._bind(window)

    def on_window_closed():                         # закрыли окно — сервер сразу увидит выход
        api.disconnect()

    window.events.closed += on_window_closed        # обработчик должен возвращать None
    return window


def decorate_macos():
    """Имя в меню и значок в Dock вместо «Python»."""
    if sys.platform != "darwin":
        return
    try:
        from AppKit import NSApplication, NSImage
        from Foundation import NSBundle
        from PyObjCTools import AppHelper

        NSBundle.mainBundle().infoDictionary()["CFBundleName"] = "Клиент чата"

        def apply_icon():
            image = NSImage.alloc().initByReferencingFile_(str(ICON_FILE))
            if image is not None:
                NSApplication.sharedApplication().setApplicationIconImage_(image)

        AppHelper.callAfter(apply_icon)
    except Exception:
        pass                                        # оформление необязательно


def main():
    create_window()
    decorate_macos()
    webview.start()


if __name__ == "__main__":
    main()
