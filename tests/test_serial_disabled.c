#include "common.h"

#include <stdio.h>

int main(void)
{
    /* Первая лабораторная: COM-порт ни разу не открывался. Эти вызовы должны
     * быть безопасны и на Windows, где синхронизацию нужно инициализировать. */
    if (serial_enabled()) {
        fputs("FAIL: COM-порт неожиданно включён\n", stderr);
        return 1;
    }
    serial_send_line("Иван: ПрИвЕт");
    serial_close();
    puts("OK: клиент безопасно работает без Arduino");
    return 0;
}
