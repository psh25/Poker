#include "debug.h"

#include <stdarg.h>
#include <stdio.h>

static void dbg_write(const char *s) {
    // 先写 CH340（UART0），再写 USB CDC：
    // USB CDC 在异常情况下可能长时间阻塞（等待主机取数据），
    // 先写 CH340 能保证日志不会因为 USB 异常而全部丢失。
    Serial0.print(s);    // UART0 / CH340（GPIO43/44）
    Serial.print(s);     // USB CDC
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

