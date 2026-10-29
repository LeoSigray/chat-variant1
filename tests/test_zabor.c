#include "common.h"

#include <stdio.h>
#include <string.h>

static int check(const char *input, const char *expected)
{
    char output[512];
    zabor_utf8(input, output, sizeof(output));
    if (strcmp(output, expected) != 0) {
        fprintf(stderr, "FAIL: %s\n want: %s\n  got: %s\n", input, expected, output);
        return 1;
    }
    return 0;
}

static int check_line(const char *input, int expected_result, const char *expected)
{
    char output[512] = "не изменено";
    int result = zabor_chat_line(input, output, sizeof(output));
    if (result != expected_result || (result && strcmp(output, expected) != 0)) {
        fprintf(stderr, "FAIL line: %s\n want result=%d text=%s\n got result=%d text=%s\n",
                input, expected_result, expected, result, output);
        return 1;
    }
    return 0;
}

int main(void)
{
    int failed = 0;
    failed += check("привет мир", "ПрИвЕт МиР");
    failed += check("ёжик ABC xyz", "ЁжИк AbC xYz");
    failed += check("123, тест!", "123, ТеСт!");
    failed += check("", "");
    failed += check_line("Иван: привет мир", 1, "Иван: ПрИвЕт МиР");
    failed += check_line("@Иван: секрет", 1, "@Иван: СеКрЕт");
    failed += check_line("[Сервер] Пользователи в чате (2):", 0, "");
    failed += check_line("  1. Иван", 0, "");
    failed += check_line("строка без имени", 0, "");
    if (failed == 0)
        puts("OK: преобразование ЗаБоРом работает");
    return failed != 0;
}
