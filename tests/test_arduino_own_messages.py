"""Один клиент с Arduino (виртуальный порт) должен видеть на плате и свои сообщения.

Запуск: python3 tests/test_arduino_own_messages.py [папка со собранными server и client]
Только macOS/Linux (нужен pty). Сначала соберите проект: make.
"""
import os, pty, select, socket, subprocess, sys, time
from pathlib import Path
ROOT = sys.argv[1] if len(sys.argv) > 1 else str(Path(__file__).resolve().parents[1])
PORT = 40500
def rd(fd, secs):
    data, end = b"", time.time()+secs
    while time.time() < end:
        r,_,_ = select.select([fd],[],[],0.1)
        if r:
            try: data += os.read(fd, 65536)
            except OSError: break
    return data.decode("utf-8","replace")
fails=[]
def check(t, ok, got=None):
    print(("ok   " if ok else "FAIL ")+t)
    if not ok: fails.append(t); print("    получено:", repr(got))
srv = subprocess.Popen([ROOT+"/server", str(PORT)], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
time.sleep(0.8)
m, s = pty.openpty(); com = os.ttyname(s)                      # «Arduino»
cl = subprocess.Popen([ROOT+"/client", "127.0.0.1", str(PORT), "--com", com], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
def say(t): cl.stdin.write((t+"\n").encode()); cl.stdin.flush()
say("Иван"); time.sleep(2.8)                                    # плата перезагружается 2 с
say("привет мир"); out = rd(m, 1.2)
check("ОДИН клиент: свои сообщения доходят до Arduino", "Иван: ПрИвЕт МиР\n" in out, out)
say("#users"); say("#help"); out = rd(m, 1.0)
check("команды и ответы сервера на Arduino не идут", out == "", out)
say("#NewName Пётр"); time.sleep(0.6); say("тест"); out = rd(m, 1.2)
check("после #NewName используется новое имя", "Пётр: ТеСт\n" in out, out)
# второй пользователь пишет — тоже на Arduino, ровно один раз
o = socket.create_connection(("127.0.0.1", PORT), timeout=3); time.sleep(0.3); o.recv(65536)
o.sendall("#NewName Мария\n".encode()); time.sleep(0.4); o.recv(65536); rd(m,0.3)
o.sendall("всем привет\n".encode()); out = rd(m, 1.2)
check("чужое сообщение выводится один раз", out.count("Мария: ВсЕм ПрИвЕт") == 1, out)
say("ответ"); out = rd(m, 1.2)
check("своё сообщение при двух клиентах — один раз", out.count("Пётр: ОтВеТ") == 1, out)
say("#ToUser Мария секрет"); out = rd(m, 1.0)
check("свой #ToUser на Arduino не идёт", "СеКрЕт" not in out and out == "", out)
cl.stdin.close(); cl.wait(timeout=5); srv.terminate()
print("ПРОВАЛ: %d" % len(fails) if fails else "ВСЕ ПРОВЕРКИ ПРОЙДЕНЫ"); sys.exit(1 if fails else 0)
