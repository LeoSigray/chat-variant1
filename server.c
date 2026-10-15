/*
 * server.c — сервер чата.
 *
 * Однопоточный сервер на select(): одновременно обслуживает до MAX_CLIENTS
 * клиентов. Слушает на всех сетевых интерфейсах (INADDR_ANY), поэтому
 * клиенты могут подключаться с других компьютеров сети.
 *
 * Команды клиента:
 *   #NewName <имя>             — задать/сменить имя (одно слово, уникальное)
 *   #users                     — список пользователей
 *   #ToUser <имя> <сообщение>  — личное сообщение
 *   #LastMsg <имя>             — последнее сообщение пользователя (вариант 1)
 *   #help                      — справка
 *   #exit                      — выйти
 * Любой другой текст рассылается всем как "имя: текст",
 * личное сообщение получатель видит как "@имя: текст".
 *
 * Вариант 1 — "последнее сообщение пользователя": сервер запоминает для
 * каждого клиента последнее сообщение, которое тот отправил в общий чат
 * (личные сообщения не запоминаются), и по #LastMsg <имя> выдаёт его
 * вместе со временем отправки.
 *
 * Запуск: server [порт]      (по умолчанию 5777)
 */
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#ifndef _WIN32
#  include <sys/select.h>
#  include <ifaddrs.h>
#  include <net/if.h>
#endif

#ifndef NAME_TIMEOUT_SEC
#  define NAME_TIMEOUT_SEC 120  /* сколько ждать #NewName от нового клиента, с */
#endif

/* Ограничение скорости приёма от одного клиента (алгоритм "token bucket"):
 * в среднем RATE_BYTES_PER_SEC байт/с, кратковременно — до RATE_BURST_BYTES
 * (вставка большого текста). Каждая строка дополнительно "стоит" LINE_COST
 * байт, ведь её рассылают всем: иначе поток коротких строк "a" размножался бы
 * в тысячи сообщений в секунду. Пока лимит исчерпан, сервер не читает сокет
 * клиента, и TCP сам притормаживает отправителя. Так один "флудер" не может
 * завалить сообщениями остальных, а сервер никогда не ждёт на отправке. */
#define RATE_BYTES_PER_SEC 4096
#define RATE_BURST_BYTES   16384
#define LINE_COST          256
#define RECV_CHUNK         512            /* максимум байт за одно чтение */
#define SEND_BUF_BYTES     (256 * 1024)   /* буфер отправки каждому клиенту */

typedef struct {
    int      active;                    /* слот занят                        */
    int      closing;                   /* отключить в конце итерации цикла  */
    time_t   connected_at;              /* время подключения                 */
    sock_t   sock;
    char     ip[IP_STR_LEN];            /* адрес клиента (только для журнала сервера) */
    unsigned port;                      /* порт клиента (только для журнала) */
    int      has_name;                  /* клиент уже представился           */
    char     name[NAME_MAX_BYTES + 1];
    char     key[NAME_MAX_BYTES + 1];   /* имя в нижнем регистре             */
    char     skel[NAME_MAX_BYTES + 1];  /* "скелет" имени (см. name_skeleton) */
    int      has_last_msg;              /* клиент уже писал в общий чат      */
    char     last_msg[MSG_MAX_BYTES + 1]; /* его последнее сообщение в общий чат */
    time_t   last_msg_at;               /* когда оно отправлено              */
    char     inbuf[MSG_MAX_BYTES + 1];  /* накопитель входящей строки        */
    size_t   inlen;
    int      overflow;                  /* текущая строка слишком длинная    */
    long     budget;                    /* сколько байт можно принять сейчас */
    unsigned long long budget_ms;       /* время последнего пополнения       */
} client_t;

static client_t g_clients[MAX_CLIENTS];
static volatile sig_atomic_t g_stop = 0;

static void log_msg(const char *fmt, ...) CHAT_PRINTF(1, 2);
static void client_send(client_t *c, const char *fmt, ...) CHAT_PRINTF(2, 3);
static int  broadcast(const client_t *except, const char *fmt, ...) CHAT_PRINTF(2, 3);

/* ---------------------------------------------------------------------- */

/* Монотонное время в миллисекундах (не зависит от перевода часов). */
static unsigned long long now_ms(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)(ts.tv_nsec / 1000000);
#endif
}

/* Пополнить бюджет приёма клиента пропорционально прошедшему времени. */
static void refill_budget(client_t *c, unsigned long long now)
{
    unsigned long long add = (now - c->budget_ms) * RATE_BYTES_PER_SEC / 1000;
    if (add == 0)
        return;
    c->budget_ms = now;
    if (add > RATE_BURST_BYTES || c->budget + (long)add > RATE_BURST_BYTES)
        c->budget = RATE_BURST_BYTES;
    else
        c->budget += (long)add;
}

static void log_msg(const char *fmt, ...)
{
    char stamp[16] = "--:--:--";
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    va_list ap;

    if (tm)
        strftime(stamp, sizeof(stamp), "%H:%M:%S", tm);
    con_print(CLR_SYSTEM, "[%s] ", stamp);
    va_start(ap, fmt);
    con_vprint(CLR_DEFAULT, fmt, ap);
    va_end(ap);
    con_write(CLR_DEFAULT, "\n", 1);
}

static int clients_online(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].active && !g_clients[i].closing)
            n++;
    return n;
}

/* Отправка не удалась: клиент отключается в конце итерации.
 * Сокеты клиентов неблокирующие, поэтому клиент, который перестал читать
 * данные (переполнил буфер отправки), не может "подвесить" весь сервер. */
static void send_failed(client_t *c)
{
    int err = sock_last_error();
    if (sock_would_block(err))
        log_msg("%s:%u не успевает принимать данные — отключаем", c->ip, c->port);
    else
        log_msg("Не удалось отправить данные %s:%u (%s) — отключаем", c->ip, c->port, net_strerror(err));
    c->closing = 1;
}

/* Отправить одну строку клиенту. При ошибке клиент помечается на отключение. */
static void client_send(client_t *c, const char *fmt, ...)
{
    char buf[MSG_MAX_BYTES * 2];
    va_list ap;

    if (!c->active || c->closing)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (send_line(c->sock, buf) != 0)
        send_failed(c);
}

/* Разослать строку всем представившимся клиентам, кроме except.
 * Возвращает число получателей. */
static int broadcast(const client_t *except, const char *fmt, ...)
{
    char buf[MSG_MAX_BYTES * 2];
    va_list ap;
    int i, count = 0;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for (i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &g_clients[i];
        if (!c->active || c->closing || !c->has_name || c == except)
            continue;
        if (send_line(c->sock, buf) != 0)
            send_failed(c);
        else
            count++;
    }
    return count;
}

/* Поиск пользователя по имени (кроме exclude). Сначала ищем точное
 * совпадение без учёта регистра, затем — внешне неотличимое имя
 * ("ВОВ" кириллицей и "BOB" латиницей). */
static client_t *find_by_name(const char *name, int include_closing, const client_t *exclude)
{
    char key[NAME_MAX_BYTES + 1], skel[NAME_MAX_BYTES + 1];
    int i, pass;

    name_fold(name, key, sizeof(key));
    name_skeleton(name, skel, sizeof(skel));
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &g_clients[i];
            if (!c->active || !c->has_name || c == exclude || (c->closing && !include_closing))
                continue;
            if (strcmp(pass == 0 ? c->key : c->skel, pass == 0 ? key : skel) == 0)
                return c;
        }
    }
    return NULL;
}

/* ------------------------------ Команды ------------------------------- */

static void send_help(client_t *c)
{
    client_send(c, SRV_PREFIX "Доступные команды:");
    client_send(c, "  #NewName <имя>             — сменить имя (одно слово)");
    client_send(c, "  #users                     — список пользователей в чате");
    client_send(c, "  #ToUser <имя> <сообщение>  — личное сообщение пользователю");
    client_send(c, "  #LastMsg <имя>             — последнее сообщение пользователя");
    client_send(c, "  #help                      — эта справка");
    client_send(c, "  #exit                      — выйти из чата");
    client_send(c, "  Любой другой текст получат все участники чата.");
}

static void cmd_newname(client_t *c, const char *name)
{
    char err[200];
    char key[NAME_MAX_BYTES + 1], skel[NAME_MAX_BYTES + 1];
    client_t *other;

    if (*name == '\0') {
        client_send(c, SRV_NAME_BAD "укажите имя: #NewName <имя>");
        return;
    }
    if (name_check(name, err, sizeof(err)) != 0) {
        client_send(c, SRV_NAME_BAD "%s. Попробуйте ещё раз: #NewName <имя>", err);
        return;
    }
    if (c->has_name && strcmp(c->name, name) == 0) {
        client_send(c, SRV_NAME_BAD "«%s» — это уже ваше текущее имя", name);
        return;
    }
    name_fold(name, key, sizeof(key));
    name_skeleton(name, skel, sizeof(skel));
    other = find_by_name(name, 1, c);
    if (other != NULL) {
        if (strcmp(other->key, key) == 0)
            client_send(c, SRV_NAME_BAD "имя «%s» уже занято. Выберите другое: #NewName <имя>", name);
        else
            client_send(c, SRV_NAME_BAD "имя «%s» неотличимо на экране от занятого имени «%s». "
                        "Выберите другое: #NewName <имя>", name, other->name);
        return;
    }

    if (!c->has_name) {
        snprintf(c->name, sizeof(c->name), "%s", name);
        snprintf(c->key, sizeof(c->key), "%s", key);
        snprintf(c->skel, sizeof(c->skel), "%s", skel);
        c->has_name = 1;
        client_send(c, SRV_NAME_OK "%s", c->name);
        send_help(c);
        log_msg("%s:%u представился как «%s»", c->ip, c->port, c->name);
        broadcast(c, SRV_PREFIX "К чату присоединился пользователь %s", c->name);
    } else {
        char old[NAME_MAX_BYTES + 1];
        snprintf(old, sizeof(old), "%s", c->name);
        snprintf(c->name, sizeof(c->name), "%s", name);
        snprintf(c->key, sizeof(c->key), "%s", key);
        snprintf(c->skel, sizeof(c->skel), "%s", skel);
        client_send(c, SRV_NAME_CHANGED "%s", c->name);
        log_msg("«%s» (%s:%u) сменил имя на «%s»", old, c->ip, c->port, c->name);
        broadcast(c, SRV_PREFIX "Пользователь %s сменил имя на %s", old, c->name);
    }
}

static void cmd_users(client_t *c)
{
    int i, named = 0, anonymous = 0, num = 0;

    for (i = 0; i < MAX_CLIENTS; i++) {
        const client_t *u = &g_clients[i];
        if (!u->active || u->closing)
            continue;
        if (u->has_name)
            named++;
        else
            anonymous++;
    }
    client_send(c, SRV_PREFIX "Пользователи в чате (%d):", named);
    for (i = 0; i < MAX_CLIENTS; i++) {
        const client_t *u = &g_clients[i];
        if (!u->active || u->closing || !u->has_name)
            continue;
        client_send(c, "  %2d. %s%s", ++num, u->name, u == c ? "  (это вы)" : "");
    }
    if (named == 0)
        client_send(c, "  (пока никто не представился)");
    if (anonymous > 0)
        client_send(c, "  Подключений без имени: %d", anonymous);
}

static void cmd_touser(client_t *c, char *args)
{
    char *name = args, *msg = args;
    client_t *to;

    if (!c->has_name) {
        client_send(c, SRV_PREFIX "Сначала представьтесь: #NewName <имя>");
        return;
    }
    while (*msg && !is_space_char(*msg))
        msg++;
    if (*msg)
        *msg++ = '\0';
    msg = str_trim(msg);
    if (*name == '\0' || *msg == '\0') {
        client_send(c, SRV_PREFIX "Формат личного сообщения: #ToUser <имя> <сообщение>");
        return;
    }
    to = find_by_name(name, 0, NULL);
    if (to == NULL) {
        client_send(c, SRV_PREFIX "Пользователь «%s» не найден. Список пользователей: #users", name);
        return;
    }
    client_send(to, "@%s: %s", c->name, msg);
    if (to->closing) {
        client_send(c, SRV_PREFIX "Не удалось доставить сообщение пользователю %s", to->name);
        return;
    }
    if (to != c)
        client_send(c, SRV_PREFIX "Личное сообщение для %s доставлено", to->name);
    log_msg("Личное сообщение: %s -> %s", c->name, to->name);
}

/* Вариант 1: #LastMsg <имя> — последнее сообщение пользователя в общий чат. */
static void cmd_lastmsg(client_t *c, const char *args)
{
    const client_t *u;
    char stamp[16] = "--:--:--";
    struct tm *tm;

    if (!c->has_name) {
        client_send(c, SRV_PREFIX "Сначала представьтесь: #NewName <имя>");
        return;
    }
    if (*args == '\0' || strchr(args, ' ') != NULL) {
        client_send(c, SRV_PREFIX "Формат команды: #LastMsg <имя>");
        return;
    }
    u = find_by_name(args, 0, NULL);
    if (u == NULL) {
        client_send(c, SRV_PREFIX "Пользователь «%s» не найден. Список пользователей: #users", args);
        return;
    }
    if (!u->has_last_msg) {
        client_send(c, SRV_PREFIX "Пользователь %s ещё ничего не писал в общий чат", u->name);
        return;
    }
    tm = localtime(&u->last_msg_at);
    if (tm)
        strftime(stamp, sizeof(stamp), "%H:%M:%S", tm);
    client_send(c, SRV_PREFIX "Последнее сообщение пользователя %s (%s): %s",
                u->name, stamp, u->last_msg);
}

/* Обработка одной полученной от клиента строки. */
static void process_line(client_t *c, char *line)
{
    sanitize_text(line);
    line = str_trim(line);
    if (*line == '\0')
        return;

    if (line[0] == '#') {
        char *cmd = line, *args = line;
        while (*args && !is_space_char(*args))
            args++;
        if (*args)
            *args++ = '\0';
        args = str_trim(args);

        if (ascii_casecmp(cmd, "#NewName") == 0)
            cmd_newname(c, args);
        else if (ascii_casecmp(cmd, "#users") == 0)
            cmd_users(c);
        else if (ascii_casecmp(cmd, "#ToUser") == 0)
            cmd_touser(c, args);
        else if (ascii_casecmp(cmd, "#LastMsg") == 0 || ascii_casecmp(cmd, "#last") == 0)
            cmd_lastmsg(c, args);
        else if (ascii_casecmp(cmd, "#help") == 0)
            send_help(c);
        else if (ascii_casecmp(cmd, "#exit") == 0 || ascii_casecmp(cmd, "#quit") == 0) {
            client_send(c, SRV_PREFIX "До свидания!");
            c->closing = 1;
        } else
            client_send(c, SRV_PREFIX "Неизвестная команда «%s». Список команд: #help", cmd);
        return;
    }

    if (!c->has_name) {
        client_send(c, SRV_PREFIX "Сначала представьтесь: #NewName <имя>");
        return;
    }
    /* вариант 1: запоминаем последнее сообщение пользователя в общий чат
     * (даже если, кроме него, в чате никого нет) */
    snprintf(c->last_msg, sizeof(c->last_msg), "%s", line);
    c->last_msg_at = time(NULL);
    c->has_last_msg = 1;

    log_msg("%s: %s", c->name, line);
    if (broadcast(c, "%s: %s", c->name, line) == 0)
        client_send(c, SRV_PREFIX "Кроме вас в чате никого нет — сообщение никто не получил");
}

/* ------------------------- Работа с сокетами -------------------------- */

/* Закрыть соединение, которому отказали в подключении, так, чтобы клиент
 * успел прочитать причину. Если закрыть сокет, в котором остались
 * непрочитанные данные клиента, TCP пошлёт RST вместо обычного FIN,
 * и клиент потеряет наш ответ. Поэтому сначала завершаем отправку и
 * коротко (не дольше ~0,3 с) вычитываем то, что клиент успел прислать. */
static void reject_close(sock_t s)
{
    char tmp[512];
    int rounds;
#ifdef _WIN32
    DWORD tv = 100;
    shutdown(s, SD_SEND);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    for (rounds = 0; rounds < 3; rounds++)
        if (recv(s, tmp, (int)sizeof(tmp), 0) <= 0)
            break;
#else
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    shutdown(s, SHUT_WR);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    for (rounds = 0; rounds < 3; rounds++)
        if (recv(s, tmp, sizeof(tmp), 0) <= 0)
            break;
#endif
    sock_close(s);
}

static void accept_client(sock_t listen_sock)
{
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    char ip[IP_STR_LEN];
    unsigned port;
    client_t *c = NULL;
    int i;
    sock_t s = accept(listen_sock, (struct sockaddr *)&addr, &alen);

    if (s == SOCK_INVALID) {
        log_msg("Ошибка accept: %s", net_strerror(sock_last_error()));
        return;
    }
    if (inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip)) == NULL)
        snprintf(ip, sizeof(ip), "?");
    port = ntohs(addr.sin_port);

    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].active) {
            c = &g_clients[i];
            break;
        }
    }
#ifndef _WIN32
    if (s >= FD_SETSIZE)            /* select() не умеет работать с такими дескрипторами */
        c = NULL;
#endif
    if (c == NULL) {
        send_line(s, SRV_PREFIX "Сервер переполнен, попробуйте подключиться позже");
        reject_close(s);
        log_msg("Отказ в подключении %s:%u — нет свободных мест", ip, port);
        return;
    }

    memset(c, 0, sizeof(*c));
    c->active = 1;
    c->sock = s;
    c->port = port;
    c->connected_at = time(NULL);
    snprintf(c->ip, sizeof(c->ip), "%s", ip);
    c->budget = RATE_BURST_BYTES;
    c->budget_ms = now_ms();
    sock_set_nonblocking(s);        /* медленный клиент не остановит сервер */
    sock_enable_keepalive(s);       /* выключенный компьютер клиента будет обнаружен */
    {
        int sndbuf = SEND_BUF_BYTES;    /* запас на случай всплеска сообщений */
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof(sndbuf));
    }

    log_msg("Новое подключение: %s:%u (всего клиентов: %d)", c->ip, c->port, clients_online());
    client_send(c, SRV_PREFIX "Добро пожаловать в чат!");
    client_send(c, SRV_INVITE);
}

/* Отключить тех, кто так и не представился за NAME_TIMEOUT_SEC секунд. */
static void drop_silent_clients(void)
{
    time_t now = time(NULL);
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &g_clients[i];
        if (c->active && !c->closing && !c->has_name &&
            difftime(now, c->connected_at) > NAME_TIMEOUT_SEC) {
            client_send(c, SRV_PREFIX "Время на ввод имени (%d с) истекло, соединение закрыто",
                        NAME_TIMEOUT_SEC);
            c->closing = 1;
        }
    }
}

static void handle_client_data(client_t *c)
{
    char buf[RECV_CHUNK];
    long i, n, want = (long)sizeof(buf);

    if (want > c->budget)           /* не больше, чем разрешает лимит скорости */
        want = c->budget;
#ifdef _WIN32
    n = recv(c->sock, buf, (int)want, 0);
#else
    n = (long)recv(c->sock, buf, (size_t)want, 0);
    if (n < 0 && errno == EINTR)
        return;
#endif
    if (n > 0)
        c->budget -= n;
    if (n < 0 && sock_would_block(sock_last_error()))
        return;                     /* данных пока нет (сокет неблокирующий) */
    if (n <= 0) {
        if (n < 0)
            log_msg("Ошибка приёма от %s:%u: %s", c->ip, c->port, net_strerror(sock_last_error()));
        c->closing = 1;
        return;
    }

    /* Собираем строки: данные по TCP могут прийти частями или слитно. */
    for (i = 0; i < n && !c->closing; i++) {
        char ch = buf[i];
        if (ch == '\n') {
            c->budget -= LINE_COST;             /* "цена" рассылки строки */
            if (c->overflow) {
                client_send(c, SRV_PREFIX "Слишком длинное сообщение (максимум %d байт) — не отправлено",
                            MSG_MAX_BYTES);
                c->overflow = 0;
            } else {
                c->inbuf[c->inlen] = '\0';
                process_line(c, c->inbuf);
            }
            c->inlen = 0;
        } else if (c->inlen < MSG_MAX_BYTES) {
            c->inbuf[c->inlen++] = (ch == '\0') ? ' ' : ch;
        } else {
            c->overflow = 1;
        }
    }
    /* "долг" ограничен: клиент молчит не дольше ~4 с, и его отключение
     * или #exit не останутся незамеченными надолго */
    if (c->budget < -RATE_BURST_BYTES)
        c->budget = -RATE_BURST_BYTES;
}

/* Закрыть соединения, помеченные на отключение, и сообщить об уходе. */
static void reap_clients(void)
{
    int i, found;
    do {
        found = 0;
        for (i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &g_clients[i];
            if (!c->active || !c->closing)
                continue;
            found = 1;
            sock_close(c->sock);
            c->active = 0;
            if (c->has_name) {
                log_msg("Отключился «%s» (%s:%u), клиентов: %d", c->name, c->ip, c->port, clients_online());
                broadcast(NULL, SRV_PREFIX "Пользователь %s покинул чат", c->name);
            } else {
                log_msg("Отключился %s:%u (без имени), клиентов: %d", c->ip, c->port, clients_online());
            }
        }
    } while (found);    /* рассылка могла пометить на отключение кого-то ещё */
}

/* Показать адреса этого компьютера, по которым к серверу могут подключиться. */
static void print_local_addresses(int port)
{
#ifdef _WIN32
    char host[256];
    struct addrinfo hints, *res = NULL, *p;

    if (gethostname(host, sizeof(host)) != 0)
        return;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0)
        return;
    for (p = res; p != NULL; p = p->ai_next) {
        char ip[IP_STR_LEN];
        struct sockaddr_in *sa = (struct sockaddr_in *)p->ai_addr;
        if (inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip)) != NULL)
            con_print(CLR_DEFAULT, "    %s:%d\n", ip, port);
    }
    freeaddrinfo(res);
#else
    struct ifaddrs *list, *p;

    if (getifaddrs(&list) != 0)
        return;
    for (p = list; p != NULL; p = p->ifa_next) {
        char ip[IP_STR_LEN];
        if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_INET || (p->ifa_flags & IFF_LOOPBACK))
            continue;
        if (inet_ntop(AF_INET, &((struct sockaddr_in *)(void *)p->ifa_addr)->sin_addr, ip, sizeof(ip)))
            con_print(CLR_DEFAULT, "    %s:%d   (%s)\n", ip, port, p->ifa_name);
    }
    freeifaddrs(list);
#endif
}

#ifdef _WIN32
static BOOL WINAPI on_console_ctrl(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_stop = 1;
        return TRUE;
    }
    return FALSE;
}
#else
static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}
#endif

int main(int argc, char **argv)
{
    int port = DEFAULT_PORT, i;
    sock_t ls;
    struct sockaddr_in addr;

    console_init();
    console_disable_quickedit();
    if (argc > 1) {
        char *end;
        long p = strtol(argv[1], &end, 10);
        if (*end != '\0' || p <= 0 || p > 65535) {
            con_print(CLR_ERROR, "Использование: %s [порт]   (порт 1..65535, по умолчанию %d)\n",
                      argv[0], DEFAULT_PORT);
            return 1;
        }
        port = (int)p;
    }
    if (net_init() != 0) {
        con_print(CLR_ERROR, "Не удалось инициализировать сокеты\n");
        console_pause_if_owned();
        return 1;
    }

#ifdef _WIN32
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
#else
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_signal;          /* без SA_RESTART: select() прервётся */
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        signal(SIGPIPE, SIG_IGN);           /* запись в закрытый сокет не должна убивать сервер */
    }
#endif

    ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == SOCK_INVALID) {
        con_print(CLR_ERROR, "Ошибка socket: %s\n", net_strerror(sock_last_error()));
        net_cleanup();
        console_pause_if_owned();
        return 1;
    }
    {
        int on = 1;
#ifdef _WIN32
        /* запрещаем другим процессам занять тот же порт */
        setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, sizeof(on));
#else
        /* позволяет сразу перезапустить сервер на том же порту */
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   /* все сетевые интерфейсы */
    addr.sin_port = htons((unsigned short)port);
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        con_print(CLR_ERROR, "Не удалось занять порт %d: %s\n", port, net_strerror(sock_last_error()));
        con_print(CLR_INFO, "Возможно, сервер уже запущен. Другой порт: server <порт>\n");
        sock_close(ls);
        net_cleanup();
        console_pause_if_owned();
        return 1;
    }
    if (listen(ls, SOMAXCONN) != 0) {
        con_print(CLR_ERROR, "Ошибка listen: %s\n", net_strerror(sock_last_error()));
        sock_close(ls);
        net_cleanup();
        console_pause_if_owned();
        return 1;
    }

    con_print(CLR_NAME, "=== Сервер чата запущен на порту %d ===\n", port);
    con_print(CLR_DEFAULT, "Клиенты с других компьютеров могут подключаться по адресам:\n");
    print_local_addresses(port);
    con_print(CLR_INFO, "Разрешите входящие TCP-подключения на порт %d в брандмауэре.\n", port);
    con_print(CLR_INFO, "Для остановки сервера нажмите Ctrl+C.\n\n");

    while (!g_stop) {
        fd_set rfds;
        struct timeval tv;
        sock_t maxfd = ls;
        unsigned long long now;
        int rc, throttled;

        FD_ZERO(&rfds);
        FD_SET(ls, &rfds);
        now = now_ms();
        throttled = 0;
        for (i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &g_clients[i];
            if (!c->active || c->closing)
                continue;
            refill_budget(c, now);
            if (c->budget <= 0) {       /* лимит исчерпан — пока не читаем */
                throttled = 1;
                continue;
            }
            FD_SET(c->sock, &rfds);
            if (c->sock > maxfd)
                maxfd = c->sock;
        }
        /* раз в секунду проверяем флаг остановки и тайм-ауты;
         * чаще — если кто-то ждёт пополнения лимита скорости */
        tv.tv_sec = throttled ? 0 : 1;
        tv.tv_usec = throttled ? 100000 : 0;

        rc = select((int)(maxfd + 1), &rfds, NULL, NULL, &tv);
        if (rc < 0) {
#ifndef _WIN32
            if (errno == EINTR)
                continue;
#endif
            log_msg("Ошибка select: %s", net_strerror(sock_last_error()));
            break;
        }
        if (rc > 0) {
            if (FD_ISSET(ls, &rfds))
                accept_client(ls);
            for (i = 0; i < MAX_CLIENTS; i++) {
                client_t *c = &g_clients[i];
                if (c->active && !c->closing && FD_ISSET(c->sock, &rfds))
                    handle_client_data(c);
            }
        }
        drop_silent_clients();
        reap_clients();
    }

    log_msg("Сервер останавливается...");
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active) {
            client_send(&g_clients[i], SRV_PREFIX "Сервер завершает работу. До свидания!");
            sock_close(g_clients[i].sock);
            g_clients[i].active = 0;
        }
    }
    sock_close(ls);
    net_cleanup();
    return 0;
}
