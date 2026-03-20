/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"

const int LED_GREEN = 10;
const int LED_YELLOW = 11;
const int LED_BLUE = 12;
const int LED_RED = 13;

const int BUTTON_GREEN = 27;
const int BUTTON_YELLOW = 26;
const int BUTTON_BLUE = 22;
const int BUTTON_RED = 16;

int main() {
    stdio_init_all();
    while (true) {
        printf("Hello, world!\n");
        sleep_ms(1000);
    }
}
