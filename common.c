/*
 * common.c — общие функции сервера и клиента:
 * сеть, вывод в консоль (UTF-8 + цвета), работа со строками и UTF-8.
 */
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#ifdef _WIN32
#  include <mstcpip.h>              /* SIO_KEEPALIVE_VALS */
#else
#  include <pthread.h>
#  include <fcntl.h>
#  include <netinet/tcp.h>          /* TCP_KEEPIDLE и др. */
#  ifndef MSG_NOSIGNAL
#    define MSG_NOSIGNAL 0          /* на macOS нет — там SIGPIPE игнорируется */
#  endif
#endif

/* ======================================================================
 *                                 Сеть
 * ====================================================================== */

int net_init(void)
{
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
#else
    return 0;
#endif
}

void net_cleanup(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

void sock_close(sock_t s)
{
    if (s == SOCK_INVALID)
        return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

int sock_last_error(void)
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

const char *net_strerror(int err)
{
    static char buf[512];
#ifdef _WIN32
    /* FormatMessageW + перевод в UTF-8, чтобы русский текст ошибки не "поехал" */
    wchar_t wbuf[256];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, (DWORD)err, 0, wbuf, 256, NULL);
    int len = 0;
    while (n > 0 && (wbuf[n - 1] == L'\r' || wbuf[n - 1] == L'\n' || wbuf[n - 1] == L' '))
        n--;
    if (n > 0)
        len = WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)n, buf, (int)sizeof(buf) - 16, NULL, NULL);
    if (len <= 0)
        snprintf(buf, sizeof(buf), "код ошибки %d", err);
    else
        snprintf(buf + len, sizeof(buf) - (size_t)len, " (%d)", err);
#else
    snprintf(buf, sizeof(buf), "%s (%d)", strerror(err), err);
#endif
    return buf;
}

int send_all(sock_t s, const char *data, size_t len)
{
    while (len > 0) {
        int chunk = len > (size_t)INT_MAX ? INT_MAX : (int)len;
#ifdef _WIN32
        int n = send(s, data, chunk, 0);
#else
        ssize_t n = send(s, data, (size_t)chunk, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
#endif
        if (n <= 0)
            return -1;
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

int send_line(sock_t s, const char *text)
{
    char buf[MSG_MAX_BYTES * 2];
    size_t len = strlen(text);
    if (len > sizeof(buf) - 1)
        len = sizeof(buf) - 1;
    memcpy(buf, text, len);
    buf[len++] = '\n';
    return send_all(s, buf, len);   /* одним вызовом, чтобы строка не рвалась */
}

void sock_set_nonblocking(sock_t s)
{
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags != -1)
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

int sock_would_block(int err)
{
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

/* Если компьютер собеседника выключили или у него пропала сеть,
 * TCP сам не узнает об этом, пока не попробует что-то отправить.
 * Keepalive-проверки раз в несколько секунд простоя выявляют такие
 * соединения примерно за полминуты. */
void sock_enable_keepalive(sock_t s)
{
#ifdef _WIN32
    struct tcp_keepalive ka;
    DWORD bytes = 0;
    ka.onoff = 1;
    ka.keepalivetime = 15000;       /* первая проверка после 15 с простоя */
    ka.keepaliveinterval = 3000;    /* затем каждые 3 с */
    WSAIoctl(s, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), NULL, 0, &bytes, NULL, NULL);
#else
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
#  if defined(TCP_KEEPIDLE)
    { int v = 15; setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &v, sizeof(v)); }
#  elif defined(TCP_KEEPALIVE)          /* macOS */
    { int v = 15; setsockopt(s, IPPROTO_TCP, TCP_KEEPALIVE, &v, sizeof(v)); }
#  endif
#  ifdef TCP_KEEPINTVL
    { int v = 3; setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &v, sizeof(v)); }
#  endif
#  ifdef TCP_KEEPCNT
    { int v = 5; setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &v, sizeof(v)); }
#  endif
#  ifdef TCP_USER_TIMEOUT
    /* Linux не шлёт keepalive, пока есть неподтверждённые данные, а повторные
     * передачи длятся ~15 минут. Ограничиваем ожидание подтверждения 30 с.
     * (Собеседник, который 30 с вообще не читает данные и держит окно TCP
     * закрытым, тоже будет отключён — как и "нечитающие" клиенты на сервере.) */
    { unsigned v = 30000; setsockopt(s, IPPROTO_TCP, TCP_USER_TIMEOUT, &v, sizeof(v)); }
#  endif
#endif
}

/* ======================================================================
 *                     Консоль: UTF-8 и цветной вывод
 * ====================================================================== */

#ifdef _WIN32
static CRITICAL_SECTION g_con_lock;
static HANDLE g_out = INVALID_HANDLE_VALUE;
static int    g_out_is_console = 0;
static WORD   g_default_attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
#else
static pthread_mutex_t g_con_lock;   /* рекурсивный, как CRITICAL_SECTION в Windows */
static int g_use_ansi = 0;
#endif
static int g_con_ready = 0;

void console_init(void)
{
    if (g_con_ready)
        return;
#ifdef _WIN32
    DWORD mode;
    CONSOLE_SCREEN_BUFFER_INFO info;
    InitializeCriticalSection(&g_con_lock);
    /* Кодовую страницу консоли не меняем: вывод идёт через WriteConsoleW,
     * ввод (в клиенте) — через ReadConsoleW, т.е. сразу в UTF-16. */
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_out_is_console = (g_out != INVALID_HANDLE_VALUE && GetConsoleMode(g_out, &mode));
    if (g_out_is_console && GetConsoleScreenBufferInfo(g_out, &info))
        g_default_attr = info.wAttributes;
#else
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_con_lock, &attr);
    pthread_mutexattr_destroy(&attr);
    g_use_ansi = isatty(STDOUT_FILENO);
#endif
    g_con_ready = 1;
}

#ifdef _WIN32
static HANDLE g_in_handle = INVALID_HANDLE_VALUE;
static DWORD  g_in_saved_mode;
static int    g_in_mode_changed = 0;

static void restore_input_mode(void)
{
    if (g_in_mode_changed)
        SetConsoleMode(g_in_handle, g_in_saved_mode);
}
#endif

/* В консоли Windows щелчок мышью включает режим выделения (QuickEdit),
 * и пока выделение активно, любой вывод в консоль блокируется. Для
 * однопоточного сервера это означало бы остановку всего чата. */
void console_disable_quickedit(void)
{
#ifdef _WIN32
    DWORD mode;
    g_in_handle = GetStdHandle(STD_INPUT_HANDLE);
    if (g_in_handle != INVALID_HANDLE_VALUE && GetConsoleMode(g_in_handle, &mode) &&
        (mode & ENABLE_QUICK_EDIT_MODE)) {
        g_in_saved_mode = mode;
        if (SetConsoleMode(g_in_handle, (mode | ENABLE_EXTENDED_FLAGS) & ~(DWORD)ENABLE_QUICK_EDIT_MODE)) {
            g_in_mode_changed = 1;
            atexit(restore_input_mode);   /* вернуть режим окну cmd после выхода */
        }
    }
#endif
}

int console_owned(void)
{
#ifdef _WIN32
    DWORD ids[2];
    /* 1 — к консоли подключены только мы: программу запустили двойным
     * щелчком, и окно закроется сразу после её завершения. */
    return GetConsoleProcessList(ids, 2) == 1;
#else
    return 0;
#endif
}

void console_pause_if_owned(void)
{
    int ch;
    if (!console_owned())
        return;
    con_print(CLR_INFO, "Нажмите Enter для выхода...");
    while ((ch = getchar()) != EOF && ch != '\n')
        ;
}

void con_lock(void)
{
#ifdef _WIN32
    EnterCriticalSection(&g_con_lock);
#else
    pthread_mutex_lock(&g_con_lock);
#endif
}

void con_unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&g_con_lock);
#else
    pthread_mutex_unlock(&g_con_lock);
#endif
}

#ifdef _WIN32
static WORD color_attr(color_t c)
{
    WORD bg = g_default_attr & 0xF0;   /* фон оставляем как был */
    switch (c) {
    case CLR_SYSTEM:  return bg | FOREGROUND_GREEN | FOREGROUND_BLUE;                      /* бирюзовый */
    case CLR_PRIVATE: return bg | FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;  /* ярко-малиновый */
    case CLR_NAME:    return bg | FOREGROUND_GREEN | FOREGROUND_INTENSITY;                  /* ярко-зелёный */
    case CLR_ERROR:   return bg | FOREGROUND_RED | FOREGROUND_INTENSITY;                    /* красный */
    case CLR_INFO:    return bg | FOREGROUND_RED | FOREGROUND_GREEN;                        /* жёлтый */
    default:          return g_default_attr;
    }
}

/* Вывод UTF-8 строки в консоль Windows через WriteConsoleW:
 * так кириллица отображается правильно при любой кодовой странице. */
static void win_write_utf8(const char *s, size_t len)
{
    DWORD written;
    if (len == 0)
        return;
    if (!g_out_is_console) {
        fwrite(s, 1, len, stdout);
        fflush(stdout);
        return;
    }
    while (len > 0) {
        /* режем на куски по границе UTF-8 символа */
        size_t chunk = len > 4096 ? 4096 : len;
        wchar_t wbuf[4096];
        int wn;
        while (chunk > 0 && chunk < len && ((unsigned char)s[chunk] & 0xC0) == 0x80)
            chunk--;
        if (chunk == 0)
            chunk = len > 4096 ? 4096 : len;
        wn = MultiByteToWideChar(CP_UTF8, 0, s, (int)chunk, wbuf, 4096);
        if (wn > 0)
            WriteConsoleW(g_out, wbuf, (DWORD)wn, &written, NULL);
        s += chunk;
        len -= chunk;
    }
}
#else
static const char *color_ansi(color_t c)
{
    switch (c) {
    case CLR_SYSTEM:  return "\033[36m";
    case CLR_PRIVATE: return "\033[1;35m";
    case CLR_NAME:    return "\033[1;32m";
    case CLR_ERROR:   return "\033[1;31m";
    case CLR_INFO:    return "\033[33m";
    default:          return "\033[0m";
    }
}
#endif

void con_write(color_t color, const char *utf8, size_t len)
{
    if (!g_con_ready)
        console_init();
    con_lock();
#ifdef _WIN32
    if (g_out_is_console && color != CLR_DEFAULT)
        SetConsoleTextAttribute(g_out, color_attr(color));
    win_write_utf8(utf8, len);
    if (g_out_is_console && color != CLR_DEFAULT)
        SetConsoleTextAttribute(g_out, g_default_attr);
#else
    if (g_use_ansi && color != CLR_DEFAULT)
        fputs(color_ansi(color), stdout);
    fwrite(utf8, 1, len, stdout);
    if (g_use_ansi && color != CLR_DEFAULT)
        fputs("\033[0m", stdout);
    fflush(stdout);
#endif
    con_unlock();
}

void con_vprint(color_t color, const char *fmt, va_list ap)
{
    char buf[MSG_MAX_BYTES * 4];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = (int)sizeof(buf) - 1;
    con_write(color, buf, (size_t)n);
}

void con_print(color_t color, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    con_vprint(color, fmt, ap);
    va_end(ap);
}

/* ======================================================================
 *                           Строки и UTF-8
 * ====================================================================== */

int is_space_char(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

char *str_trim(char *s)
{
    char *end;
    while (is_space_char(*s))
        s++;
    end = s + strlen(s);
    while (end > s && is_space_char(end[-1]))
        end--;
    *end = '\0';
    return s;
}

int str_starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

int ascii_casecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb || ca == 0)
            return ca - cb;
    }
}

/* Декодирует один символ UTF-8. Возвращает его длину в байтах
 * или 0, если последовательность некорректна. */
int utf8_decode(const unsigned char *s, unsigned *cp)
{
    unsigned c = s[0];
    if (c < 0x80) {
        *cp = c;
        return c ? 1 : 0;
    }
    if (c >= 0xC2 && c <= 0xDF) {
        if ((s[1] & 0xC0) != 0x80) return 0;
        *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if (c >= 0xE0 && c <= 0xEF) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        *cp = ((c & 0x0F) << 12) | ((unsigned)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        if (*cp < 0x800 || (*cp >= 0xD800 && *cp <= 0xDFFF)) return 0;
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        *cp = ((c & 0x07) << 18) | ((unsigned)(s[1] & 0x3F) << 12) |
              ((unsigned)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        if (*cp < 0x10000 || *cp > 0x10FFFF) return 0;
        return 4;
    }
    return 0;
}

static size_t utf8_encode(unsigned cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

size_t utf8_strlen(const char *s)
{
    size_t n = 0;
    for (; *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80)
            n++;
    return n;
}

/* Заменяет управляющие символы пробелом, а некорректные байты UTF-8 — '?'.
 * Защищает терминалы клиентов от ESC-последовательностей и мусора. */
void sanitize_text(char *s)
{
    unsigned char *p = (unsigned char *)s;
    while (*p) {
        unsigned cp;
        int len;
        if (*p < 0x20 || *p == 0x7F) {
            *p++ = ' ';
            continue;
        }
        len = utf8_decode(p, &cp);
        if (len == 0) {
            *p++ = '?';
            continue;
        }
        if (cp >= 0x80 && cp < 0xA0) {       /* управляющие символы C1 */
            memset(p, ' ', (size_t)len);
        }
        p += len;
    }
}

static int is_latin_letter(unsigned cp)
{
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
}

/* Буквы русского, украинского и белорусского алфавитов */
static int is_cyrillic_letter(unsigned cp)
{
    if (cp >= 0x0410 && cp <= 0x044F)                     /* А..я */
        return 1;
    switch (cp) {
    case 0x0401: case 0x0451:                             /* Ё ё */
    case 0x0404: case 0x0454:                             /* Є є */
    case 0x0406: case 0x0456:                             /* І і */
    case 0x0407: case 0x0457:                             /* Ї ї */
    case 0x040E: case 0x045E:                             /* Ў ў */
    case 0x0490: case 0x0491:                             /* Ґ ґ */
        return 1;
    default:
        return 0;
    }
}

int name_check(const char *name, char *err, size_t err_size)
{
    const unsigned char *p = (const unsigned char *)name;
    size_t chars = 0;
    int has_latin = 0, has_cyrillic = 0;
    char key[NAME_MAX_BYTES + 1];

    if (*name == '\0') {
        snprintf(err, err_size, "имя не может быть пустым");
        return -1;
    }
    for (; *p; p++) {
        if (is_space_char((char)*p)) {
            snprintf(err, err_size, "имя должно состоять из одного слова (без пробелов)");
            return -1;
        }
    }
    for (p = (const unsigned char *)name; *p; ) {
        unsigned cp;
        int len = utf8_decode(p, &cp);
        if (len == 0) {
            snprintf(err, err_size, "некорректная кодировка имени (нужна UTF-8)");
            return -1;
        }
        if (is_latin_letter(cp)) {
            has_latin = 1;
        } else if (is_cyrillic_letter(cp)) {
            has_cyrillic = 1;
        } else if (!((cp >= '0' && cp <= '9') || cp == '_' || cp == '-' || cp == '.')) {
            snprintf(err, err_size,
                     "допустимы только буквы (латиница или кириллица), цифры и символы _ - .");
            return -1;
        }
        chars++;
        p += len;
    }
    if (chars > NAME_MAX_CHARS) {
        snprintf(err, err_size, "имя слишком длинное (максимум %d символов)", NAME_MAX_CHARS);
        return -1;
    }
    if (!has_latin && !has_cyrillic) {
        snprintf(err, err_size, "имя должно содержать хотя бы одну букву");
        return -1;
    }
    /* Запрет смешения алфавитов: иначе "Воb" (кириллические В и о)
     * выглядел бы точно так же, как уже занятое имя "Bob". */
    if (has_latin && has_cyrillic) {
        snprintf(err, err_size, "нельзя смешивать латиницу и кириллицу в одном имени");
        return -1;
    }
    {
        static const char *reserved[] = { "Сервер", "СЕРВЕР", "Server" };
        char skel[NAME_MAX_BYTES + 1], rkey[NAME_MAX_BYTES + 1], rskel[NAME_MAX_BYTES + 1];
        size_t i;
        name_fold(name, key, sizeof(key));
        name_skeleton(name, skel, sizeof(skel));
        for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
            name_fold(reserved[i], rkey, sizeof(rkey));
            name_skeleton(reserved[i], rskel, sizeof(rskel));
            if (strcmp(key, rkey) == 0 || strcmp(skel, rskel) == 0) {
                snprintf(err, err_size, "это имя зарезервировано");
                return -1;
            }
        }
    }
    return 0;
}

/* "Скелет" имени: кириллические буквы, которые выглядят точно так же, как
 * латинские (в том же регистре), заменяются латинскими, затем регистр
 * убирается. Имена "ВОВ" и "BOB" или "сосо" и "coco" получают одинаковый
 * скелет, и сервер не допустит второе из них, раз они неотличимы на экране. */
void name_skeleton(const char *name, char *out, size_t out_size)
{
    /* пары "кириллица -> похожая латинская буква" */
    static const struct { unsigned cyr; char lat; } look_alike[] = {
        { 0x0410, 'A' }, { 0x0412, 'B' }, { 0x0415, 'E' }, { 0x041A, 'K' }, { 0x041C, 'M' },
        { 0x041D, 'H' }, { 0x041E, 'O' }, { 0x0420, 'P' }, { 0x0421, 'C' }, { 0x0422, 'T' },
        { 0x0423, 'Y' }, { 0x0425, 'X' }, { 0x0406, 'I' }, { 0x0417, '3' },
        { 0x0430, 'a' }, { 0x0435, 'e' }, { 0x043E, 'o' }, { 0x0440, 'p' }, { 0x0441, 'c' },
        { 0x0443, 'y' }, { 0x0445, 'x' }, { 0x0456, 'i' }
    };
    char tmp[NAME_MAX_BYTES * 2 + 1];
    const unsigned char *p = (const unsigned char *)name;
    size_t pos = 0;

    while (*p) {
        unsigned cp;
        char enc[4];
        size_t n, i;
        int len = utf8_decode(p, &cp);
        if (len == 0) {
            enc[0] = (char)*p;
            n = 1;
            len = 1;
        } else {
            for (i = 0; i < sizeof(look_alike) / sizeof(look_alike[0]); i++) {
                if (look_alike[i].cyr == cp) {
                    cp = (unsigned char)look_alike[i].lat;
                    break;
                }
            }
            n = utf8_encode(cp, enc);
        }
        if (pos + n >= sizeof(tmp))
            break;
        memcpy(tmp + pos, enc, n);
        pos += n;
        p += len;
    }
    tmp[pos] = '\0';
    name_fold(tmp, out, out_size);
}

/* Переводит имя в нижний регистр (латиница и русские буквы),
 * чтобы "Иван" и "иван" считались одним и тем же именем. */
void name_fold(const char *name, char *out, size_t out_size)
{
    const unsigned char *p = (const unsigned char *)name;
    size_t pos = 0;
    while (*p) {
        unsigned cp;
        char enc[4];
        size_t n;
        int len = utf8_decode(p, &cp);
        if (len == 0) {                    /* битый байт — копируем как есть */
            cp = *p;
            len = 1;
            enc[0] = (char)cp;
            n = 1;
        } else {
            if (cp >= 'A' && cp <= 'Z')
                cp += 0x20;
            else if (cp >= 0x0410 && cp <= 0x042F)   /* А..Я -> а..я */
                cp += 0x20;
            else if (cp >= 0x0400 && cp <= 0x040F)   /* Ё, Є, І, Ї, Ў -> ё, є, і, ї, ў */
                cp += 0x50;
            else if (cp == 0x0490)                   /* Ґ -> ґ */
                cp = 0x0491;
            n = utf8_encode(cp, enc);
        }
        if (pos + n >= out_size)
            break;
        memcpy(out + pos, enc, n);
        pos += n;
        p += len;
    }
    out[pos] = '\0';
}
