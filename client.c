/*
 * client.c — клиент чата.
 *
 * При запуске спрашивает имя пользователя, затем подключается к серверу
 * (сервер может находиться на другом компьютере) и в ответ на приглашение
 * сервера регистрирует имя командой #NewName. Если имя занято — предлагает
 * ввести другое.
 *
 * Работает в два потока:
 *   - главный поток читает клавиатуру и отправляет строки на сервер;
 *   - поток приёма печатает входящие сообщения.
 * Личные сообщения (строки вида "@имя: текст") выделяются цветом.
 *
 * Лабораторная 2: с параметром --com каждое полученное сообщение пользователя
 * дополнительно отправляется на Arduino через COM-порт в стиле «ЗаБоР».
 * Имя сохраняется: "Иван: привет" -> "Иван: ПрИвЕт".
 *
 * Запуск: client [IP-адрес-сервера] [порт] [--com ПОРТ] [--baud N]
 *         client --list-com
 *         если IP не указан — клиент спросит его при запуске.
 */
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <process.h>
#else
#  include <pthread.h>
#  include <signal.h>
#  include <sys/select.h>
#endif

#define RECV_BUF_SIZE  (MSG_MAX_BYTES * 4)
#define INPUT_BUF_SIZE (MSG_MAX_BYTES + 2)      /* строка + '\n' + '\0' */

#define READ_OK        1
#define READ_EOF       0
#define READ_TOO_LONG (-1)

/* Буфер для сборки строк из TCP-потока */
typedef struct {
    sock_t sock;
    char   buf[RECV_BUF_SIZE];
    size_t len;
} reader_t;

static reader_t g_reader;
static volatile int g_quit = 0;     /* пользователь сам выходит из чата */
static volatile int g_lost = 0;     /* сервер разорвал соединение       */

/* ---------------------------------------------------------------------- */

/* Начинается ли строка s со слова word (без учёта регистра)?
 * Возвращает длину слова или 0. "#exit bye" -> да, "#exitX" -> нет. */
static size_t first_word_is(const char *s, const char *word)
{
    size_t n = strlen(word), i;
    for (i = 0; i < n; i++) {
        char a = s[i], b = word[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return (s[n] == '\0' || is_space_char(s[n])) ? n : 0;
}

/* Чтение строки с клавиатуры в UTF-8 (без '\n'). */
static int read_line(char *out, size_t out_size)
{
#ifdef _WIN32
    HANDLE in;
    DWORD mode;
#endif
    out[0] = '\0';
#ifdef _WIN32
    /* В консоли Windows читаем UTF-16 через ReadConsoleW и переводим в UTF-8:
     * так русские буквы вводятся корректно при любой кодовой странице. */
    in = GetStdHandle(STD_INPUT_HANDLE);
    if (in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode)) {
        wchar_t wline[MSG_MAX_BYTES + 1];
        size_t wlen = 0;
        int overflow = 0, n = 0;
        for (;;) {
            /* Буфер больше лимита протокола: консоль Windows 10 обрезает
             * строку до размера буфера (минимум 254 символа), а нам нужно
             * увидеть, что строка длиннее лимита, и честно сообщить об этом. */
            wchar_t chunk[2 * MSG_MAX_BYTES];
            DWORD got = 0, k;
            int eol = 0;
            if (!ReadConsoleW(in, chunk, (DWORD)(sizeof(chunk) / sizeof(chunk[0])), &got, NULL) || got == 0)
                return READ_EOF;
            for (k = 0; k < got; k++) {
                wchar_t ch = chunk[k];
                if (ch == L'\n') {
                    eol = 1;
                    break;
                }
                if (ch == L'\r')
                    continue;
                if (ch == 0x1A && wlen == 0 && !overflow)   /* Ctrl+Z — конец ввода */
                    return READ_EOF;
                if (wlen < MSG_MAX_BYTES)
                    wline[wlen++] = ch;
                else
                    overflow = 1;
            }
            if (eol)
                break;
        }
        if (overflow)
            return READ_TOO_LONG;
        if (wlen > 0) {
            n = WideCharToMultiByte(CP_UTF8, 0, wline, (int)wlen, out, (int)out_size - 1, NULL, NULL);
            if (n <= 0)
                return READ_TOO_LONG;          /* не поместилось в буфер */
        }
        out[n] = '\0';
        return strlen(out) > MSG_MAX_BYTES ? READ_TOO_LONG : READ_OK;
    }
#endif
    if (fgets(out, (int)out_size, stdin) == NULL)
        return READ_EOF;
    if (strchr(out, '\n') == NULL && !feof(stdin)) {
        int ch;
        while ((ch = getchar()) != EOF && ch != '\n')
            ;                                   /* пропускаем остаток длинной строки */
        return READ_TOO_LONG;
    }
    out[strcspn(out, "\r\n")] = '\0';
    return strlen(out) > MSG_MAX_BYTES ? READ_TOO_LONG : READ_OK;
}

/* Получить от сервера одну строку (без '\n').
 * 1 — строка получена, 0 — сервер закрыл соединение, -1 — ошибка. */
static int recv_line(reader_t *r, char *line, size_t line_size)
{
    for (;;) {
        char *nl = memchr(r->buf, '\n', r->len);
        if (nl != NULL || r->len == sizeof(r->buf)) {
            size_t take = nl ? (size_t)(nl - r->buf) : r->len;
            size_t copy = take < line_size - 1 ? take : line_size - 1;
            memcpy(line, r->buf, copy);
            line[copy] = '\0';
            if (copy > 0 && line[copy - 1] == '\r')
                line[copy - 1] = '\0';
            if (nl)
                take++;                         /* сам символ '\n' */
            memmove(r->buf, r->buf + take, r->len - take);
            r->len -= take;
            return 1;
        }
        {
#ifdef _WIN32
            int n = recv(r->sock, r->buf + r->len, (int)(sizeof(r->buf) - r->len), 0);
#else
            ssize_t n = recv(r->sock, r->buf + r->len, sizeof(r->buf) - r->len, 0);
            if (n < 0 && errno == EINTR)
                continue;
#endif
            if (n == 0)
                return 0;
            if (n < 0)
                return -1;
            r->len += (size_t)n;
        }
    }
}

/* Есть ли в сокете данные, которые можно прочитать прямо сейчас? */
static int sock_readable_now(sock_t s)
{
    fd_set rfds;
    struct timeval tv;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    return select((int)(s + 1), &rfds, NULL, NULL, &tv) > 0;
}

/* Вывести входящую строку с подсветкой:
 *   @имя: текст      — личное сообщение (ярко-малиновым);
 *   [Сервер] ...     — служебное сообщение (бирюзовым);
 *   имя: текст       — обычное сообщение (имя зелёным). */
static void show_line(char *line)
{
    sanitize_text(line);
    con_lock();
    if (line[0] == '@') {
        con_write(CLR_PRIVATE, line, strlen(line));
    } else if (str_starts_with(line, SRV_PREFIX) || line[0] == ' ') {
        con_write(CLR_SYSTEM, line, strlen(line));
    } else {
        char *sep = strstr(line, ": ");
        if (sep != NULL) {
            con_write(CLR_NAME, line, (size_t)(sep - line) + 1);
            con_write(CLR_DEFAULT, sep + 1, strlen(sep + 1));
        } else {
            con_write(CLR_DEFAULT, line, strlen(line));
        }
    }
    con_write(CLR_DEFAULT, "\n", 1);
    con_unlock();
}

/* Лабораторная 2: пользовательское сообщение из сокета -> ЗаБоР -> Arduino.
 * serial_send_line() только ставит строку в очередь, поэтому медленный COM-порт
 * не задерживает поток, принимающий сообщения чата. */
static void forward_to_arduino(const char *line)
{
    char transformed[RECV_BUF_SIZE + 1];

    if (serial_enabled() && zabor_chat_line(line, transformed, sizeof(transformed)))
        serial_send_line(transformed);
}

/* Спросить у пользователя имя (одно слово). 0 — успех, -1 — конец ввода. */
static int ask_name(char *name, size_t name_size, const char *prompt)
{
    char buf[INPUT_BUF_SIZE], err[200];
    for (;;) {
        char *s;
        int r;
        con_print(CLR_INFO, "%s", prompt);
        r = read_line(buf, sizeof(buf));
        if (r == READ_EOF)
            return -1;
        if (r == READ_TOO_LONG) {
            con_print(CLR_ERROR, "Слишком длинная строка.\n");
            continue;
        }
        s = str_trim(buf);
        {
            /* пользователь мог ввести команду целиком: "#NewName Иван" */
            size_t cmd_len = first_word_is(s, "#NewName");
            if (cmd_len > 0)
                s = str_trim(s + cmd_len);
        }
        if (name_check(s, err, sizeof(err)) != 0) {
            con_print(CLR_ERROR, "Имя не подходит: %s.\n", err);
            continue;
        }
        snprintf(name, name_size, "%s", s);
        return 0;
    }
}

static sock_t connect_to_server(const char *host, const char *port)
{
    struct addrinfo hints, *res = NULL, *p;
    sock_t s = SOCK_INVALID;
    int rc, err = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
#ifdef _WIN32
        con_print(CLR_ERROR, "Не удалось определить адрес «%s»: %s\n", host, net_strerror(rc));
#else
        con_print(CLR_ERROR, "Не удалось определить адрес «%s»: %s\n", host, gai_strerror(rc));
#endif
        return SOCK_INVALID;
    }
    for (p = res; p != NULL; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == SOCK_INVALID) {
            err = sock_last_error();
            continue;
        }
        if (connect(s, p->ai_addr, (socklen_t)p->ai_addrlen) == 0)
            break;
        err = sock_last_error();
        sock_close(s);
        s = SOCK_INVALID;
    }
    freeaddrinfo(res);
    if (s == SOCK_INVALID) {
        con_print(CLR_ERROR, "Не удалось подключиться к %s:%s — %s\n", host, port, net_strerror(err));
        con_print(CLR_INFO, "Проверьте, что сервер запущен, IP-адрес и порт указаны верно,\n"
                            "а на компьютере сервера порт открыт в брандмауэре.\n");
    }
    return s;
}

/* Регистрация имени на сервере. Сервер приглашает представиться (SRV_INVITE),
 * клиент отвечает "#NewName имя" и ждёт ответа. Если имя не принято
 * (например, занято) — спрашивает у пользователя другое.
 * Имя отправляется только после приглашения: если сервер переполнен, он
 * сразу закроет соединение, и клиент успеет прочитать причину. */
static int register_name(char *name, size_t name_size)
{
    char line[RECV_BUF_SIZE + 1];
    char cmd[INPUT_BUF_SIZE];
    int send_pending = 0;          /* имя готово к отправке */

    for (;;) {
        int r;
        /* Пока пользователь вводил другое имя, сервер мог уже что-то прислать
         * (например, что время на ввод имени истекло) и закрыть соединение.
         * Сначала показываем такие строки: отправка в закрытое соединение
         * вызвала бы RST, и Windows выбросила бы непрочитанный ответ. */
        if (send_pending && g_reader.len == 0 && !sock_readable_now(g_reader.sock)) {
            snprintf(cmd, sizeof(cmd), "#NewName %s", name);
            con_print(CLR_INFO, "> %s\n", cmd);
            if (send_line(g_reader.sock, cmd) != 0) {
                con_print(CLR_ERROR, "Ошибка отправки: %s\n", net_strerror(sock_last_error()));
                return -1;
            }
            send_pending = 0;
        }
        r = recv_line(&g_reader, line, sizeof(line));
        if (r <= 0) {
            con_print(CLR_ERROR, "Сервер закрыл соединение.\n");
            return -1;
        }
        show_line(line);
        if (str_starts_with(line, SRV_NAME_OK))
            return 0;
        if (str_starts_with(line, SRV_NAME_BAD)) {
            if (ask_name(name, name_size, "Введите другое имя: ") != 0)
                return -1;
            send_pending = 1;
        } else if (str_starts_with(line, SRV_INVITE)) {
            send_pending = 1;          /* ответ на приглашение сервера */
        }
    }
}

/* Поток приёма сообщений от сервера */
#ifdef _WIN32
static unsigned __stdcall receiver_thread(void *arg)
#else
static void *receiver_thread(void *arg)
#endif
{
    char line[RECV_BUF_SIZE + 1];
    (void)arg;

    while (recv_line(&g_reader, line, sizeof(line)) > 0) {
        show_line(line);
        forward_to_arduino(line);
    }

    if (!g_quit) {
#ifdef _WIN32
        if (console_owned()) {
            /* Клиент запущен двойным щелчком: если завершиться сразу, окно
             * закроется и сообщение никто не прочитает. Выходим через главный
             * поток, когда пользователь нажмёт Enter. */
            g_lost = 1;
            con_print(CLR_ERROR, "\nСоединение с сервером разорвано. Нажмите Enter для выхода.\n");
            return 0;
        }
#endif
        con_print(CLR_ERROR, "\nСоединение с сервером разорвано. Клиент завершает работу.\n");
        /* Главный поток сейчас ждёт ввода с клавиатуры, поэтому
         * завершаем весь процесс отсюда (ОС сама закроет сокет). */
#ifdef _WIN32
        ExitProcess(1);
#else
        _exit(1);
#endif
    }
    return 0;
}

int main(int argc, char **argv)
{
    char host[256], port_str[16];
    char name[NAME_MAX_BYTES + 1];
    char line[INPUT_BUF_SIZE];
    const char *host_arg = NULL, *com_name = NULL;
    sock_t sock;
    int send_error = 0, port = DEFAULT_PORT, port_given = 0;
    int baud = SERIAL_DEFAULT_BAUD, i;
#ifdef _WIN32
    HANDLE th;
#else
    pthread_t th;
#endif

    console_init();
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            con_print(CLR_DEFAULT,
                      "Использование: %s [адрес] [порт] [--com COM3] [--baud 9600]\n"
                      "               %s --list-com\n",
                      argv[0], argv[0]);
            return 0;
        } else if (strcmp(a, "--list-com") == 0) {
            con_print(CLR_NAME, "Доступные COM-порты:\n");
            serial_list();
            return 0;
        } else if (strcmp(a, "--com") == 0 && i + 1 < argc) {
            com_name = argv[++i];
        } else if (strcmp(a, "--baud") == 0 && i + 1 < argc) {
            char *end;
            long value = strtol(argv[++i], &end, 10);
            if (*argv[i] == '\0' || *end != '\0' || value < 300 || value > 4000000) {
                con_print(CLR_ERROR, "Неверная скорость «%s»\n", argv[i]);
                return 1;
            }
            baud = (int)value;
        } else if (a[0] != '-' && host_arg == NULL) {
            host_arg = a;
        } else if (a[0] != '-' && !port_given) {
            char *end;
            long value = strtol(a, &end, 10);
            if (*a == '\0' || *end != '\0' || value < 1 || value > 65535) {
                con_print(CLR_ERROR, "Неверный порт «%s» (нужно число 1..65535)\n", a);
                return 1;
            }
            port = (int)value;
            port_given = 1;
        } else {
            con_print(CLR_ERROR,
                      "Использование: %s [адрес] [порт] [--com COM3] [--baud 9600]\n",
                      argv[0]);
            return 1;
        }
    }
    snprintf(port_str, sizeof(port_str), "%d", port);

    con_print(CLR_NAME, "=== Клиент чата ===\n");

    /* Имя спрашиваем в самом начале, до подключения к серверу */
    if (ask_name(name, sizeof(name), "Введите ваше имя для чата (одно слово): ") != 0)
        return 0;

    if (host_arg != NULL) {
        snprintf(host, sizeof(host), "%s", host_arg);
    } else {
        for (;;) {
            char *s;
            int r;
            con_print(CLR_INFO, "IP-адрес сервера: ");
            r = read_line(line, sizeof(line));
            if (r == READ_EOF)
                return 0;
            if (r == READ_TOO_LONG) {
                con_print(CLR_ERROR, "Слишком длинная строка.\n");
                continue;
            }
            s = str_trim(line);
            if (*s != '\0' && strlen(s) < sizeof(host)) {
                snprintf(host, sizeof(host), "%s", s);
                break;
            }
        }
    }

    if (net_init() != 0) {
        con_print(CLR_ERROR, "Не удалось инициализировать сокеты\n");
        console_pause_if_owned();
        return 1;
    }
    if (com_name != NULL) {
        char err[300];
        serial_set_eol(EOL_LF);
        serial_set_translit(0);       /* Arduino-прошивка сама рисует русский UTF-8 */
        if (serial_open(com_name, baud, err, sizeof(err)) != 0) {
            con_print(CLR_ERROR, "COM-порт: %s\n", err);
            con_print(CLR_INFO, "Посмотреть порты: %s --list-com\n", argv[0]);
            net_cleanup();
            console_pause_if_owned();
            return 1;
        }
        con_print(CLR_NAME, "Arduino: %s, %d бод; сообщения выводятся ЗаБоРом.\n",
                  com_name, baud);
    }
    con_print(CLR_INFO, "Подключение к %s:%s...\n", host, port_str);
    sock = connect_to_server(host, port_str);
    if (sock == SOCK_INVALID) {
        serial_close();
        net_cleanup();
        console_pause_if_owned();
        return 1;
    }
    sock_enable_keepalive(sock);        /* заметить, если компьютер сервера пропал из сети */
    con_print(CLR_INFO, "Соединение установлено.\n");

    g_reader.sock = sock;
    g_reader.len = 0;
    if (register_name(name, sizeof(name)) != 0) {
        sock_close(sock);
        serial_close();
        net_cleanup();
        console_pause_if_owned();
        return 1;
    }

#ifdef _WIN32
    th = (HANDLE)_beginthreadex(NULL, 0, receiver_thread, NULL, 0, NULL);
    if (th == NULL) {
#else
    if (pthread_create(&th, NULL, receiver_thread, NULL) != 0) {
#endif
        con_print(CLR_ERROR, "Не удалось создать поток приёма\n");
        sock_close(sock);
        serial_close();
        net_cleanup();
        console_pause_if_owned();
        return 1;
    }

    con_print(CLR_INFO, "Пишите сообщения и нажимайте Enter. Личные сообщения выделены цветом.\n"
                        "#help — список команд, #exit — выход.\n");

    for (;;) {
        char *msg;
        int r = read_line(line, sizeof(line));
        if (g_lost || r == READ_EOF)
            break;
        if (r == READ_TOO_LONG) {
            con_print(CLR_ERROR, "Сообщение слишком длинное (максимум %d байт) — не отправлено.\n",
                      MSG_MAX_BYTES);
            continue;
        }
        msg = str_trim(line);
        if (*msg == '\0')
            continue;
        if (first_word_is(msg, "#exit") || first_word_is(msg, "#quit"))
            break;
        if (send_line(sock, msg) != 0) {
            con_print(CLR_ERROR, "Ошибка отправки: %s\n", net_strerror(sock_last_error()));
            send_error = 1;
            break;
        }
    }

    /* Корректное завершение: закрываем соединение и дожидаемся потока приёма */
    g_quit = 1;
#ifdef _WIN32
    shutdown(sock, SD_BOTH);
    if (WaitForSingleObject(th, 2000) == WAIT_TIMEOUT) {
        closesocket(sock);               /* прерывает зависший recv() */
        sock = SOCK_INVALID;
        WaitForSingleObject(th, 2000);
    }
    CloseHandle(th);
#else
    shutdown(sock, SHUT_RDWR);
    pthread_join(th, NULL);
#endif
    sock_close(sock);
    serial_close();
    net_cleanup();

    if (send_error) {
        console_pause_if_owned();
        return 1;
    }
    if (g_lost)
        return 1;               /* сообщение уже выведено, Enter уже нажат */
    con_print(CLR_INFO, "Вы вышли из чата.\n");
    return 0;
}
