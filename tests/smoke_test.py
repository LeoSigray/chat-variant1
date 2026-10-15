"""Дымовой тест чата: python tests/smoke_test.py <порт>

Запущенный сервер должен слушать на 127.0.0.1:<порт>. Проверяет вход по #NewName,
запрет повторного имени, общий чат, личные сообщения и вариант 1 (#LastMsg).
"""
import socket
import sys
import time

PORT = int(sys.argv[1])
failed = []


class Client:
    def __init__(self, name):
        self.sock = socket.create_connection(("127.0.0.1", PORT), timeout=3)
        self.read()                                  # приветствие и приглашение
        self.reply = self.say("#NewName " + name)

    def read(self, wait=0.5):
        data, end = b"", time.time() + wait
        while time.time() < end:
            self.sock.settimeout(max(0.01, end - time.time()))
            try:
                chunk = self.sock.recv(65536)
            except (socket.timeout, OSError):
                break
            if not chunk:
                break
            data += chunk
        return data.decode("utf-8", "replace").splitlines()

    def say(self, text):
        self.sock.sendall(text.encode("utf-8") + b"\n")
        return self.read()


def check(title, ok, got):
    print(("ok   " if ok else "FAIL ") + title)
    if not ok:
        print("     получено:", got)
        failed.append(title)


alice = Client("Alice")
check("вход под именем", any("Вы вошли в чат под именем Alice" in s for s in alice.reply), alice.reply)

bob = Client("Bob")
dup = Client.__new__(Client)                         # второе подключение с занятым именем
dup.sock = socket.create_connection(("127.0.0.1", PORT), timeout=3)
dup.read()
r = dup.say("#NewName alice")
check("повтор имени (без учёта регистра) отклонён", any("уже занято" in s for s in r), r)

alice.read()
alice.say("первое")
alice.say("привет, Боб")
r = bob.read()
check("общий чат: Bob получил «Alice: привет, Боб»", "Alice: привет, Боб" in r, r)

r = bob.say("#LastMsg alice")
check("#LastMsg возвращает последнее сообщение",
      any("Последнее сообщение пользователя Alice" in s and s.endswith(": привет, Боб") for s in r), r)

alice.say("#ToUser Bob секрет")
r = bob.read()
check("личное сообщение приходит как @Alice: ...", "@Alice: секрет" in r, r)

r = bob.say("#LastMsg alice")
check("личное сообщение не стало «последним»", any(s.endswith(": привет, Боб") for s in r), r)

r = bob.say("#users")
check("#users содержит обоих", any("Alice" in s for s in r) and any("Bob" in s for s in r), r)

print()
print("ПРОВАЛ: %d" % len(failed) if failed else "Все проверки пройдены")
sys.exit(1 if failed else 0)
