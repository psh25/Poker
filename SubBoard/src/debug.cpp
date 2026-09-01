#include "debug.h"

#include <stdarg.h>
#include <stdio.h>

static void dbg_write(const char *s) {
    Serial.print(s);     // USB CDC
    Serial0.print(s);    // UART0 / CH340（GPIO43/44）
}

void dbg_init(void) {
    Serial.begin(115200);
    Serial0.begin(115200);
    delay(50);
}

void dbg_print(const char *s) {
    dbg_write(s);
}

void dbg_println(const char *s) {
    dbg_write(s);
    dbg_write("\r\n");
}

void dbg_println(void) {
    dbg_write("\r\n");
}

void dbg_printf(const char *fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    dbg_write(buf);
}

