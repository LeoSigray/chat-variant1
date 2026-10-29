/*
 * common.c — общие функции сервера и клиента:
 * сеть, вывод в консоль (UTF-8 + цвета), работа со строками и UTF-8.
 */
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>

#ifdef _WIN32
#  include <mstcpip.h>              /* SIO_KEEPALIVE_VALS */
#else
#  include <pthread.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <dirent.h>
#  include <termios.h>              /* настройка последовательного порта */
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
 *                 COM-порт: пересылка сообщений чата на плату
 * ====================================================================== */

#define SERIAL_QUEUE_LEN 64     /* строк в очереди; при переполнении старые отбрасываются */
#define SERIAL_RESET_MS  2000   /* плата Arduino перезагружается при открытии порта */

#ifdef _WIN32
typedef HANDLE serial_handle_t;
#  define SERIAL_NONE INVALID_HANDLE_VALUE
static CRITICAL_SECTION   g_ser_lock;
static CONDITION_VARIABLE g_ser_cond;
static HANDLE             g_ser_thread;
static INIT_ONCE           g_ser_once = INIT_ONCE_STATIC_INIT;
#else
typedef int serial_handle_t;
#  define SERIAL_NONE (-1)
static pthread_mutex_t g_ser_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_ser_cond = PTHREAD_COND_INITIALIZER;
static pthread_t       g_ser_thread;
#endif

static serial_handle_t g_ser = SERIAL_NONE;
static char *g_ser_queue[SERIAL_QUEUE_LEN];
static int   g_ser_head, g_ser_count;        /* голова и число строк в очереди               */
static int   g_ser_running, g_ser_stop;      /* поток запущен / просьба остановиться         */
static int   g_ser_failed;                   /* запись не удалась — порт больше не используем */
static int   g_ser_warned_drop;
static int   g_ser_errcode;                  /* код ошибки последней неудачной записи        */
static serial_eol_t g_ser_eol = EOL_LF;
static int   g_ser_translit;

#ifdef _WIN32
static BOOL CALLBACK ser_init_once(PINIT_ONCE once, PVOID param, PVOID *context)
{
    (void)once;
    (void)param;
    (void)context;
    InitializeCriticalSection(&g_ser_lock);
    InitializeConditionVariable(&g_ser_cond);
    return TRUE;
}

static void ser_sync_init(void)
{
    InitOnceExecuteOnce(&g_ser_once, ser_init_once, NULL, NULL);
}
#endif

static void ser_lock(void)
{
#ifdef _WIN32
    ser_sync_init();
    EnterCriticalSection(&g_ser_lock);
#else
    pthread_mutex_lock(&g_ser_lock);
#endif
}

static void ser_unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&g_ser_lock);
#else
    pthread_mutex_unlock(&g_ser_lock);
#endif
}

static void ser_wait(void)                   /* вызывать под блокировкой */
{
#ifdef _WIN32
    SleepConditionVariableCS(&g_ser_cond, &g_ser_lock, INFINITE);
#else
    pthread_cond_wait(&g_ser_cond, &g_ser_lock);
#endif
}

static void ser_wake(void)
{
#ifdef _WIN32
    WakeAllConditionVariable(&g_ser_cond);
#else
    pthread_cond_broadcast(&g_ser_cond);
#endif
}

static void sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

static int ser_stopping(void)
{
    int s;
    ser_lock();
    s = g_ser_stop;
    ser_unlock();
    return s;
}

/* Записать всё. 0 — успех, 1 — просили остановиться, -1 — ошибка (код в g_ser_errcode).
 * Запись не блокируется надолго: так поток можно остановить, даже если плата молчит. */
static int ser_write_all(const char *data, size_t len)
{
#ifdef _WIN32
    while (len > 0) {
        DWORD n = 0;
        if (!WriteFile(g_ser, data, (DWORD)len, &n, NULL)) {
            g_ser_errcode = (int)GetLastError();
            return -1;
        }
        data += n;
        len -= n;
        if (len > 0 && ser_stopping())      /* тайм-аут записи (см. COMMTIMEOUTS) */
            return 1;
    }
    return 0;
#else
    while (len > 0) {
        ssize_t n = write(g_ser, data, len);
        if (n > 0) {
            data += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p;
            p.fd = g_ser;
            p.events = POLLOUT;
            p.revents = 0;
            poll(&p, 1, 200);
            if (ser_stopping())
                return 1;
            continue;
        }
        g_ser_errcode = errno;
        return -1;
    }
    return 0;
#endif
}

#ifdef _WIN32
static DWORD WINAPI ser_thread_main(LPVOID arg)
#else
static void *ser_thread_main(void *arg)
#endif
{
    int waited;
    (void)arg;

    /* Arduino перезагружается, когда порт открывают: первые байты потерялись бы */
    for (waited = 0; waited < SERIAL_RESET_MS && !ser_stopping(); waited += 100)
        sleep_ms(100);

    for (;;) {
        char *line;
        int rc, i;

        ser_lock();
        while (g_ser_count == 0 && !g_ser_stop)
            ser_wait();
        if (g_ser_stop) {
            ser_unlock();
            break;
        }
        line = g_ser_queue[g_ser_head];
        g_ser_queue[g_ser_head] = NULL;
        g_ser_head = (g_ser_head + 1) % SERIAL_QUEUE_LEN;
        g_ser_count--;
        ser_unlock();

        rc = ser_write_all(line, strlen(line));
        free(line);
        if (rc > 0)
            break;
        if (rc < 0) {
            ser_lock();
            g_ser_failed = 1;
            for (i = 0; i < SERIAL_QUEUE_LEN; i++) {
                free(g_ser_queue[i]);
                g_ser_queue[i] = NULL;
            }
            g_ser_count = 0;
            ser_unlock();
            con_print(CLR_ERROR, "[COM] Ошибка записи в порт: %s. Пересылка остановлена.\n",
#ifdef _WIN32
                      net_strerror(g_ser_errcode));
#else
                      strerror(g_ser_errcode));
#endif
            break;
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

#ifndef _WIN32
static int baud_to_speed(int baud, speed_t *out)
{
    switch (baud) {
    case 1200:   *out = B1200;   return 1;
    case 2400:   *out = B2400;   return 1;
    case 4800:   *out = B4800;   return 1;
    case 9600:   *out = B9600;   return 1;
    case 19200:  *out = B19200;  return 1;
    case 38400:  *out = B38400;  return 1;
    case 57600:  *out = B57600;  return 1;
    case 115200: *out = B115200; return 1;
#  ifdef B230400
    case 230400: *out = B230400; return 1;
#  endif
    default:     return 0;
    }
}
#endif

int serial_open(const char *name, int baud, char *err, size_t err_size)
{
    int already;
    ser_lock();
    already = g_ser_running;
    ser_unlock();
    if (already) {
        snprintf(err, err_size, "COM-порт уже открыт");
        return -1;
    }

#ifdef _WIN32
    {
        char path[300];
        DCB dcb;
        COMMTIMEOUTS to;
        HANDLE h;

        /* для COM10 и выше обязательна форма "\\.\COM10"; она подходит и для COM1..COM9 */
        if (name[0] == '\\')
            snprintf(path, sizeof(path), "%s", name);
        else
            snprintf(path, sizeof(path), "\\\\.\\%s", name);
        h = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            snprintf(err, err_size, "не удалось открыть %s: %s", name, net_strerror((int)GetLastError()));
            return -1;
        }
        memset(&dcb, 0, sizeof(dcb));
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(h, &dcb)) {
            snprintf(err, err_size, "%s не похож на COM-порт: %s", name, net_strerror((int)GetLastError()));
            CloseHandle(h);
            return -1;
        }
        dcb.BaudRate = (DWORD)baud;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        if (!SetCommState(h, &dcb)) {
            snprintf(err, err_size, "не удалось настроить %s (%d бод): %s", name, baud, net_strerror((int)GetLastError()));
            CloseHandle(h);
            return -1;
        }
        memset(&to, 0, sizeof(to));
        to.WriteTotalTimeoutConstant = 500;     /* запись не зависает навсегда */
        SetCommTimeouts(h, &to);
        g_ser = h;
    }
#else
    {
        struct termios tio;
        speed_t speed;
        int fd;

        if (!baud_to_speed(baud, &speed)) {
            snprintf(err, err_size, "скорость %d бод не поддерживается (1200..115200)", baud);
            return -1;
        }
        fd = open(name, O_WRONLY | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) {
            snprintf(err, err_size, "не удалось открыть %s: %s", name, strerror(errno));
            return -1;
        }
        if (tcgetattr(fd, &tio) != 0) {
            snprintf(err, err_size, "%s не похож на последовательный порт: %s", name, strerror(errno));
            close(fd);
            return -1;
        }
        cfmakeraw(&tio);
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cflag &= ~(tcflag_t)(CSIZE | PARENB | CSTOPB | CRTSCTS);
        tio.c_cflag |= CS8;
        cfsetospeed(&tio, speed);
        cfsetispeed(&tio, speed);
        if (tcsetattr(fd, TCSANOW, &tio) != 0) {
            snprintf(err, err_size, "не удалось настроить %s (%d бод): %s", name, baud, strerror(errno));
            close(fd);
            return -1;
        }
        g_ser = fd;
    }
#endif

    ser_lock();
    g_ser_head = g_ser_count = 0;
    g_ser_stop = g_ser_failed = g_ser_warned_drop = 0;
    g_ser_running = 1;
    ser_unlock();
#ifdef _WIN32
    g_ser_thread = CreateThread(NULL, 0, ser_thread_main, NULL, 0, NULL);
    if (g_ser_thread == NULL) {
        snprintf(err, err_size, "не удалось создать поток записи");
        CloseHandle(g_ser);
        g_ser = SERIAL_NONE;
        g_ser_running = 0;
        return -1;
    }
#else
    if (pthread_create(&g_ser_thread, NULL, ser_thread_main, NULL) != 0) {
        snprintf(err, err_size, "не удалось создать поток записи");
        close(g_ser);
        g_ser = SERIAL_NONE;
        g_ser_running = 0;
        return -1;
    }
#endif
    return 0;
}

void serial_close(void)
{
    int i;
    ser_lock();
    if (!g_ser_running) {
        ser_unlock();
        return;
    }
    g_ser_stop = 1;
    ser_wake();
    ser_unlock();
#ifdef _WIN32
    WaitForSingleObject(g_ser_thread, 3000);
    CloseHandle(g_ser_thread);
    CloseHandle(g_ser);
#else
    pthread_join(g_ser_thread, NULL);
    close(g_ser);
#endif
    g_ser = SERIAL_NONE;
    ser_lock();
    for (i = 0; i < SERIAL_QUEUE_LEN; i++) {
        free(g_ser_queue[i]);
        g_ser_queue[i] = NULL;
    }
    g_ser_head = g_ser_count = 0;
    g_ser_running = 0;
    ser_unlock();
}

int serial_enabled(void)
{
    int ok;
    ser_lock();
    ok = g_ser_running && !g_ser_failed;
    ser_unlock();
    return ok;
}

void serial_set_eol(serial_eol_t eol)  { g_ser_eol = eol; }
void serial_set_translit(int on)       { g_ser_translit = on; }

void serial_send_line(const char *text)
{
    char buf[MSG_MAX_BYTES * 4 + 4];       /* транслитерация может удлинить текст ("щ" -> "shch") */
    const char *eol = "";
    char *copy;
    size_t n;

    if (!serial_enabled())
        return;
    if (g_ser_translit)
        translit_ru(text, buf, sizeof(buf) - 4);
    else
        snprintf(buf, sizeof(buf) - 4, "%s", text);
    switch (g_ser_eol) {
    case EOL_LF:   eol = "\n";   break;
    case EOL_CRLF: eol = "\r\n"; break;
    case EOL_CR:   eol = "\r";   break;
    default:       break;
    }
    n = strlen(buf);
    memcpy(buf + n, eol, strlen(eol) + 1);
    n = strlen(buf);

    copy = (char *)malloc(n + 1);
    if (copy == NULL)
        return;
    memcpy(copy, buf, n + 1);

    ser_lock();
    if (g_ser_count == SERIAL_QUEUE_LEN) {      /* плата не успевает: самое старое сообщение отбрасываем */
        free(g_ser_queue[g_ser_head]);
        g_ser_queue[g_ser_head] = NULL;
        g_ser_head = (g_ser_head + 1) % SERIAL_QUEUE_LEN;
        g_ser_count--;
        if (!g_ser_warned_drop) {
            g_ser_warned_drop = 1;
            con_print(CLR_ERROR, "[COM] Порт не успевает принимать данные: старые сообщения отбрасываются.\n");
        }
    }
    g_ser_queue[(g_ser_head + g_ser_count) % SERIAL_QUEUE_LEN] = copy;
    g_ser_count++;
    ser_wake();
    ser_unlock();
}

/* Показать порты, к которым можно подключиться (имя для параметра --com). */
void serial_list(void)
{
    int found = 0;
#ifdef _WIN32
    int i;
    for (i = 1; i <= 255; i++) {
        char name[16], target[256];
        snprintf(name, sizeof(name), "COM%d", i);
        if (QueryDosDeviceA(name, target, sizeof(target)) != 0) {
            con_print(CLR_DEFAULT, "  %s\n", name);
            found++;
        }
    }
#else
    DIR *d = opendir("/dev");
    struct dirent *e;
    static const char *const prefixes[] = { "cu.usb", "cu.wch", "cu.SLAB", "ttyUSB", "ttyACM" };
    if (d != NULL) {
        while ((e = readdir(d)) != NULL) {
            size_t k;
            for (k = 0; k < sizeof(prefixes) / sizeof(prefixes[0]); k++) {
                if (strncmp(e->d_name, prefixes[k], strlen(prefixes[k])) == 0) {
                    con_print(CLR_DEFAULT, "  /dev/%s\n", e->d_name);
                    found++;
                    break;
                }
            }
        }
        closedir(d);
    }
#endif
    if (found == 0)
        con_print(CLR_INFO, "  COM-портов не найдено. Подключите плату и проверьте драйвер.\n");
}

/* Русские (и украинские/белорусские) буквы латиницей; ъ и ь пропускаются. */
static const char *translit_cp(unsigned cp)
{
    static const char *const up[32] = {
        "A","B","V","G","D","E","Zh","Z","I","Y","K","L","M","N","O","P",
        "R","S","T","U","F","Kh","Ts","Ch","Sh","Shch","","Y","","E","Yu","Ya" };
    static const char *const lo[32] = {
        "a","b","v","g","d","e","zh","z","i","y","k","l","m","n","o","p",
        "r","s","t","u","f","kh","ts","ch","sh","shch","","y","","e","yu","ya" };
    if (cp >= 0x410 && cp <= 0x42F) return up[cp - 0x410];
    if (cp >= 0x430 && cp <= 0x44F) return lo[cp - 0x430];
    switch (cp) {
    case 0x401: return "Yo";  case 0x451: return "yo";
    case 0x404: return "Ye";  case 0x454: return "ye";
    case 0x406: return "I";   case 0x456: return "i";
    case 0x407: return "Yi";  case 0x457: return "yi";
    case 0x40E: return "U";   case 0x45E: return "u";
    case 0x490: return "G";   case 0x491: return "g";
    default:    return NULL;
    }
}

void translit_ru(const char *utf8, char *out, size_t out_size)
{
    const unsigned char *p = (const unsigned char *)utf8;
    size_t pos = 0;

    if (out_size == 0)
        return;
    while (*p) {
        unsigned cp;
        char one[2];
        const char *rep;
        size_t n;
        int len = utf8_decode(p, &cp);

        if (len == 0) {                    /* битый байт */
            one[0] = '?'; one[1] = '\0'; rep = one; len = 1;
        } else if (cp < 0x80) {
            one[0] = (char)cp; one[1] = '\0'; rep = one;
        } else {
            rep = translit_cp(cp);
            if (rep == NULL) {             /* символ, которого нет на дисплее */
                one[0] = '?'; one[1] = '\0'; rep = one;
            }
        }
        n = strlen(rep);
        if (pos + n >= out_size)
            break;
        memcpy(out + pos, rep, n);
        pos += n;
        p += len;
    }
    out[pos] = '\0';
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

/* Изменить регистр буквы для стиля «ЗаБоР». Возвращает код символа и через
 * is_letter сообщает, нужно ли переключить регистр следующей буквы. */
static unsigned zabor_case(unsigned cp, int upper, int *is_letter)
{
    *is_letter = 1;
    if (cp >= 'A' && cp <= 'Z')
        return upper ? cp : cp + ('a' - 'A');
    if (cp >= 'a' && cp <= 'z')
        return upper ? cp - ('a' - 'A') : cp;
    if (cp >= 0x0410 && cp <= 0x042F)            /* А..Я */
        return upper ? cp : cp + 0x20;
    if (cp >= 0x0430 && cp <= 0x044F)            /* а..я */
        return upper ? cp - 0x20 : cp;
    switch (cp) {
    case 0x0401: return upper ? 0x0401 : 0x0451;  /* Ё ё */
    case 0x0451: return upper ? 0x0401 : 0x0451;
    case 0x0404: return upper ? 0x0404 : 0x0454;  /* Є є */
    case 0x0454: return upper ? 0x0404 : 0x0454;
    case 0x0406: return upper ? 0x0406 : 0x0456;  /* І і */
    case 0x0456: return upper ? 0x0406 : 0x0456;
    case 0x0407: return upper ? 0x0407 : 0x0457;  /* Ї ї */
    case 0x0457: return upper ? 0x0407 : 0x0457;
    case 0x040E: return upper ? 0x040E : 0x045E;  /* Ў ў */
    case 0x045E: return upper ? 0x040E : 0x045E;
    case 0x0490: return upper ? 0x0490 : 0x0491;  /* Ґ ґ */
    case 0x0491: return upper ? 0x0490 : 0x0491;
    default:
        *is_letter = 0;
        return cp;
    }
}

void zabor_utf8(const char *utf8, char *out, size_t out_size)
{
    const unsigned char *p = (const unsigned char *)utf8;
    size_t pos = 0;
    int upper = 1;

    if (out_size == 0)
        return;
    while (*p) {
        unsigned cp;
        char encoded[4];
        size_t n;
        int len = utf8_decode(p, &cp);
        int letter;

        if (len == 0) {
            cp = '?';
            len = 1;
        }
        cp = zabor_case(cp, upper, &letter);
        n = utf8_encode(cp, encoded);
        if (pos + n >= out_size)
            break;
        memcpy(out + pos, encoded, n);
        pos += n;
        p += len;
        if (letter)
            upper = !upper;
    }
    out[pos] = '\0';
}

/* Подготовить входящую строку чата для Arduino. Имя и разделитель сохраняются,
 * регистр меняется только в тексте сообщения. Служебные строки не проходят. */
int zabor_chat_line(const char *line, char *out, size_t out_size)
{
    const char *sep;
    size_t prefix_len;

    if (line == NULL || out_size == 0 || line[0] == '\0' || line[0] == ' ' ||
        str_starts_with(line, SRV_PREFIX))
        return 0;
    sep = strstr(line, ": ");
    if (sep == NULL || sep == line || sep[2] == '\0')
        return 0;
    prefix_len = (size_t)(sep - line) + 2;
    if (prefix_len >= out_size)
        return 0;
    memcpy(out, line, prefix_len);
    zabor_utf8(sep + 2, out + prefix_len, out_size - prefix_len);
    return 1;
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
