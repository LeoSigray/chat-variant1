/*
 * common.h — общие определения для сервера и клиента чата.
 *
 * Код кроссплатформенный: Windows (Winsock2) и Linux/Unix (BSD-сокеты).
 * Все строки, которые ходят по сети, — в кодировке UTF-8, каждая строка
 * протокола заканчивается символом '\n'.
 *
 * ВАЖНО: этот заголовок должен подключаться ПЕРВЫМ в каждом .c файле,
 * т.к. он задаёт макросы, влияющие на системные заголовки.
 */
#ifndef CHAT_COMMON_H
#define CHAT_COMMON_H

#ifdef _WIN32
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0600          /* нужен inet_ntop (Vista+) */
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef _CRT_SECURE_NO_WARNINGS
#    define _CRT_SECURE_NO_WARNINGS
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "ws2_32.lib")
#  endif
typedef SOCKET sock_t;
#  define SOCK_INVALID INVALID_SOCKET
#else
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  ifndef _DEFAULT_SOURCE
#    define _DEFAULT_SOURCE              /* getifaddrs() в glibc */
#  endif
#  ifdef __APPLE__
#    define _DARWIN_C_SOURCE
#  endif
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <errno.h>
typedef int sock_t;
#  define SOCK_INVALID (-1)
#endif

#include <stddef.h>
#include <stdarg.h>

/*
 * Проверка при компиляции: строковые литералы должны быть в UTF-8.
 * В Visual Studio добавьте ключ компилятора /utf-8, иначе русские
 * строки уйдут в сеть в cp1251 и здесь будет ошибка компиляции.
 */
typedef char chat_requires_utf8_string_literals[(sizeof("Ж") == 3) ? 1 : -1];

/* Проверка строк формата printf компилятором GCC/Clang */
#if defined(__GNUC__)
#  if defined(__MINGW_PRINTF_FORMAT)
#    define CHAT_PRINTF(fmt, args) __attribute__((format(__MINGW_PRINTF_FORMAT, fmt, args)))
#  else
#    define CHAT_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#  endif
#else
#  define CHAT_PRINTF(fmt, args)
#endif

/* ---------- Параметры чата ---------- */
#define DEFAULT_PORT    5777
#define MAX_CLIENTS     32          /* одновременно подключённых клиентов   */
#define NAME_MAX_CHARS  16          /* максимум символов в имени            */
#define NAME_MAX_BYTES  (NAME_MAX_CHARS * 4)   /* то же в байтах UTF-8      */
#define MSG_MAX_BYTES   1024        /* максимум байт в одной строке протокола */
#define IP_STR_LEN      64          /* буфер под текст "a.b.c.d"            */

/* ---------- Системные сообщения сервера ----------
 * Все служебные строки сервера начинаются с SRV_PREFIX.
 * Строки-продолжения многострочных ответов (#users, #help)
 * начинаются с двух пробелов.
 * Обычное сообщение:  "имя: текст"
 * Личное сообщение:   "@имя: текст"
 */
#define SRV_PREFIX       "[Сервер] "
#define SRV_INVITE       SRV_PREFIX "Представьтесь командой: #NewName <имя>"
#define SRV_NAME_OK      SRV_PREFIX "Вы вошли в чат под именем "   /* + имя */
#define SRV_NAME_CHANGED SRV_PREFIX "Ваше новое имя: "             /* + имя */
#define SRV_NAME_BAD     SRV_PREFIX "Имя не принято: "             /* + причина */

/* ---------- Сеть ---------- */
int         net_init(void);               /* 0 — успех */
void        net_cleanup(void);
void        sock_close(sock_t s);
int         sock_last_error(void);
const char *net_strerror(int err);        /* текст ошибки в UTF-8 */
int         send_all(sock_t s, const char *data, size_t len);  /* 0 — успех */
int         send_line(sock_t s, const char *text);   /* text + '\n' */
void        sock_set_nonblocking(sock_t s);
int         sock_would_block(int err);    /* ошибка "операция не завершена сразу" */
void        sock_enable_keepalive(sock_t s);  /* обнаружение "мёртвых" соединений */

/* ---------- Консоль (UTF-8 + цвета) ---------- */
typedef enum {
    CLR_DEFAULT = 0,
    CLR_SYSTEM,     /* сообщения сервера           */
    CLR_PRIVATE,    /* личные сообщения @имя: ...  */
    CLR_NAME,       /* имя отправителя в общем чате */
    CLR_ERROR,      /* локальные ошибки             */
    CLR_INFO        /* подсказки клиента            */
} color_t;

void console_init(void);
void console_disable_quickedit(void);  /* Windows: выделение мышью не "замораживает" вывод */
int  console_owned(void);              /* Windows: окно консоли закроется вместе с программой */
void console_pause_if_owned(void);     /* "Нажмите Enter для выхода..." при запуске двойным щелчком */
void con_write(color_t color, const char *utf8, size_t len);
void con_print(color_t color, const char *fmt, ...) CHAT_PRINTF(2, 3);
void con_vprint(color_t color, const char *fmt, va_list ap) CHAT_PRINTF(2, 0);
void con_lock(void);
void con_unlock(void);

/* ---------- COM-порт (лабораторная 2) ----------
 * Сообщения кладутся в очередь, а отдельный поток пишет их в порт: медленный
 * порт (9600 бод) не должен тормозить сервер или клиент. */
#define SERIAL_DEFAULT_BAUD 9600

typedef enum { EOL_NONE = 0, EOL_LF, EOL_CRLF, EOL_CR } serial_eol_t;

int  serial_open(const char *name, int baud, char *err, size_t err_size);  /* 0 — успех */
void serial_close(void);
int  serial_enabled(void);                     /* порт открыт и пересылка работает */
void serial_set_eol(serial_eol_t eol);         /* чем заканчивать строку (по умолчанию LF) */
void serial_set_translit(int on);              /* кириллица -> латиница (для дисплея без кириллицы) */
void serial_send_line(const char *utf8_text);  /* поставить строку в очередь, не блокирует */
void serial_list(void);                        /* показать доступные COM-порты */
void translit_ru(const char *utf8, char *out, size_t out_size);
void zabor_utf8(const char *utf8, char *out, size_t out_size); /* ПрИвЕт МиР */
int  zabor_chat_line(const char *line, char *out, size_t out_size);

/* ---------- Строки и UTF-8 ---------- */
int    is_space_char(char c);
char  *str_trim(char *s);                         /* обрезает пробелы с обеих сторон */
int    str_starts_with(const char *s, const char *prefix);
int    ascii_casecmp(const char *a, const char *b);  /* 0 — равны без учёта регистра */
int    utf8_decode(const unsigned char *s, unsigned *cp);  /* длина символа или 0 */
size_t utf8_strlen(const char *s);
void   sanitize_text(char *s);   /* убирает управляющие символы и битый UTF-8 */

/* Проверка имени пользователя. 0 — имя допустимо, иначе в err причина. */
int    name_check(const char *name, char *err, size_t err_size);
/* Ключ для сравнения имён без учёта регистра (латиница + кириллица). */
void   name_fold(const char *name, char *out, size_t out_size);
/* Ключ для поиска внешне неотличимых имён ("ВОВ" и "BOB"). */
void   name_skeleton(const char *name, char *out, size_t out_size);

#endif /* CHAT_COMMON_H */
