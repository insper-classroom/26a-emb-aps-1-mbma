//  * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
//  * SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"

#include "blue.h"
#include "yellow.h"
#include "red.h"
#include "green.h"

#define AUDIO_PIN 14
#define SWITCH_PIN 17

#define MAX_LEVELS 10
#define BUTTON_COUNT 4
#define TIMEOUT_US 15000000u
#define DEBOUNCE_US 180000u

#define AUDIO_STEP 1
#define LED_OFF_MS_SEQ 240
#define LED_OFF_MS_PRESS 120

// Pinagem do projeto
static const uint LEDS[BUTTON_COUNT] = {10, 11, 12, 13};
static const uint BUTTONS[BUTTON_COUNT] = {27, 26, 22, 16};
static const char *COLOR_NAMES[BUTTON_COUNT] = {"GREEN", "YELLOW", "BLUE", "RED"};

// Áudios já existentes no projeto
static const uint8_t *const AUDIO_DATA[BUTTON_COUNT] = {
    WAV_DATA_GREEN,
    WAV_DATA_YELLOW,
    WAV_DATA_BLUE,
    WAV_DATA_RED
};

static const uint32_t AUDIO_LEN[BUTTON_COUNT] = {
    WAV_DATA_LENGTH_GREEN,
    WAV_DATA_LENGTH_YELLOW,
    WAV_DATA_LENGTH_BLUE,
    WAV_DATA_LENGTH_RED
};

// Estado do jogo
static uint8_t g_sequence[MAX_LEVELS];
static uint8_t g_level = 0;
static uint8_t g_player_pos = 0;

// Quando true, nenhum clique é aceito.
static volatile bool g_input_blocked = true;

static volatile bool g_game_over = false;
static volatile bool g_button_event[BUTTON_COUNT] = {false, false, false, false};
static volatile bool g_button_locked[BUTTON_COUNT] = {false, false, false, false};
static volatile uint32_t g_last_press_us[BUTTON_COUNT] = {0, 0, 0, 0};
static volatile uint32_t g_last_input_us = 0;

// Estado do áudio
static volatile const uint8_t *g_audio_data = NULL;
static volatile uint32_t g_audio_len = 0;
static volatile uint32_t g_audio_pos = 0;
static volatile bool g_audio_playing = false;
static int g_audio_slice = 0;

// Protótipos
static void audio_stop(void);
static void audio_start(int idx);
static void pwm_interrupt_handler(void);
static void wait_audio_finish(void);
static void blink_led_com_audio(int idx, uint32_t off_ms);
static void flush_button_events(void);
static bool all_buttons_released(void);

static int color_from_gpio(uint gpio) {
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (BUTTONS[i] == gpio) return i;
    }
    return -1;
}

static void all_leds_off(void) {
    for (int i = 0; i < BUTTON_COUNT; i++) {
        gpio_put(LEDS[i], 0);
    }
}

static void audio_stop(void) {
    irq_set_enabled(PWM_IRQ_WRAP, false);
    g_audio_playing = false;
    g_audio_data = NULL;
    g_audio_len = 0;
    g_audio_pos = 0;
    pwm_set_gpio_level(AUDIO_PIN, 0);
    irq_set_enabled(PWM_IRQ_WRAP, true);
}

static void audio_start(int idx) {
    audio_stop();
    g_audio_data = AUDIO_DATA[idx];
    g_audio_len = AUDIO_LEN[idx];
    g_audio_pos = 0;
    g_audio_playing = true;
}

static void wait_audio_finish(void) {
    while (g_audio_playing) {
        tight_loop_contents();
    }
}

static void blink_led_com_audio(int idx, uint32_t off_ms) {
    audio_start(idx);
    gpio_put(LEDS[idx], 1);

    wait_audio_finish();

    gpio_put(LEDS[idx], 0);
    sleep_ms(off_ms);
}

static void pwm_interrupt_handler(void) {
    pwm_clear_irq(g_audio_slice);

    if (g_audio_playing && g_audio_data != NULL && g_audio_pos < (g_audio_len << 3)) {
        pwm_set_gpio_level(AUDIO_PIN, g_audio_data[g_audio_pos >> 3]);
        g_audio_pos += AUDIO_STEP;
    } else {
        pwm_set_gpio_level(AUDIO_PIN, 0);
        g_audio_playing = false;
    }
}

static void flush_button_events(void) {
    uint32_t now = time_us_32();
    for (int i = 0; i < BUTTON_COUNT; i++) {
        g_button_event[i] = false;
        g_button_locked[i] = false;
        g_last_press_us[i] = now;
    }
}

static bool all_buttons_released(void) {
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (gpio_get(BUTTONS[i]) == 0) return false;
    }
    return true;
}

static void gpio_callback(uint gpio, uint32_t events) {
    int idx = color_from_gpio(gpio);
    if (idx < 0) return;

    uint32_t now = time_us_32();

    if (events & GPIO_IRQ_EDGE_FALL) {
        if (gpio_get(gpio) != 0) return;
        if (g_button_locked[idx]) return;
        if ((now - g_last_press_us[idx]) < DEBOUNCE_US) return;

        g_last_press_us[idx] = now;
        g_button_locked[idx] = true;

        if (!g_input_blocked) {
            g_button_event[idx] = true;
        }
    }

    if (events & GPIO_IRQ_EDGE_RISE) {
        if ((now - g_last_press_us[idx]) >= DEBOUNCE_US) {
            g_button_locked[idx] = false;
        }
    }
}

static void gpio_init_game(void) {
    for (int i = 0; i < BUTTON_COUNT; i++) {
        gpio_init(LEDS[i]);
        gpio_set_dir(LEDS[i], GPIO_OUT);
        gpio_put(LEDS[i], 0);

        gpio_init(BUTTONS[i]);
        gpio_set_dir(BUTTONS[i], GPIO_IN);
        gpio_pull_up(BUTTONS[i]);
    }

    gpio_init(SWITCH_PIN);
    gpio_set_dir(SWITCH_PIN, GPIO_IN);
    gpio_pull_up(SWITCH_PIN);

    gpio_set_irq_enabled_with_callback(
        BUTTONS[0],
        GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
        true,
        &gpio_callback
    );

    for (int i = 1; i < BUTTON_COUNT; i++) {
        gpio_set_irq_enabled(BUTTONS[i], GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true);
    }
}

static void audio_init(void) {
    gpio_set_function(AUDIO_PIN, GPIO_FUNC_PWM);
    g_audio_slice = pwm_gpio_to_slice_num(AUDIO_PIN);

    pwm_clear_irq(g_audio_slice);
    pwm_set_irq_enabled(g_audio_slice, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, pwm_interrupt_handler);
    irq_set_enabled(PWM_IRQ_WRAP, true);

    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, 8.0f);
    pwm_config_set_wrap(&config, 250);
    pwm_init(g_audio_slice, &config, true);

    pwm_set_gpio_level(AUDIO_PIN, 0);
}

static void game_reset(void) {
    g_input_blocked = true;

    g_level = 0;
    g_player_pos = 0;
    g_game_over = false;

    all_leds_off();
    audio_stop();

    sleep_ms(80);
    flush_button_events();
    g_last_input_us = time_us_32();
}

static void sequence_add_step(void) {
    if (g_level < MAX_LEVELS) {
        g_sequence[g_level] = (uint8_t)(rand() % BUTTON_COUNT);
        g_level++;
    }
}

static void sequence_show(void) {
    g_input_blocked = true;

    all_leds_off();
    sleep_ms(250);

    printf("Sequencia: ");
    for (uint8_t i = 0; i < g_level; i++) {
        printf("%s ", COLOR_NAMES[g_sequence[i]]);
    }
    printf("\n");

    for (uint8_t i = 0; i < g_level; i++) {
        int idx = g_sequence[i];
        blink_led_com_audio(idx, LED_OFF_MS_SEQ);
    }

    all_leds_off();
    flush_button_events();
}

static void handle_player_step(int idx) {
    g_last_input_us = time_us_32();

    blink_led_com_audio(idx, LED_OFF_MS_PRESS);

    if (idx == g_sequence[g_player_pos]) {
        g_player_pos++;
        printf("OK: %s\n", COLOR_NAMES[idx]);

        if (g_player_pos >= g_level) {
            printf("Level %u complete!\n", g_level);
        }
    } else {
        printf("Wrong key: %s\n", COLOR_NAMES[idx]);
        g_game_over = true;
    }
}

static void wait_player_repeat(void) {
    g_player_pos = 0;
    g_last_input_us = time_us_32();

    flush_button_events();

    while (!all_buttons_released()) {
        tight_loop_contents();
    }
    sleep_ms(20);

    g_input_blocked = false;

    while (!g_game_over && g_player_pos < g_level) {
        for (int i = 0; i < BUTTON_COUNT; i++) {
            if (g_button_event[i]) {
                g_button_event[i] = false;
                handle_player_step(i);
                if (g_game_over) break;
            }
        }

        if (!g_game_over && (time_us_32() - g_last_input_us) > TIMEOUT_US) {
            printf("Time out!\n");
            g_game_over = true;
        }

        tight_loop_contents();
    }

    g_input_blocked = true;
}

static void game_over_fx(void) {
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < BUTTON_COUNT; i++) gpio_put(LEDS[i], 1);
        sleep_ms(120);
        all_leds_off();
        sleep_ms(120);
    }
}

static void victory_fx(void) {
    for (int k = 0; k < 6; k++) {
        for (int i = 0; i < BUTTON_COUNT; i++) gpio_put(LEDS[i], 1);
        sleep_ms(80);
        all_leds_off();
        sleep_ms(80);
    }
}

int main(void) {
    stdio_init_all();
    set_sys_clock_khz(176000, true);
    srand(time_us_32());

    gpio_init_game();
    audio_init();

    printf("Genius / Simon Says - Raspberry Pi Pico\n");

    while (true) {
        printf("Aguardando switch ligar...\n");
        while (gpio_get(SWITCH_PIN) == 1) {
            tight_loop_contents();
        }

        printf("Switch ligado! Iniciando jogo...\n");
        sleep_ms(500);

        game_reset();
        printf("Starting new game\n");

        while (!g_game_over && g_level < MAX_LEVELS) {
            sequence_add_step();
            printf("Level %u\n", g_level);
            sequence_show();
            wait_player_repeat();

            if (!g_game_over) {
                sleep_ms(250);
                flush_button_events();
            }
        }

        if (g_game_over) {
            printf("Game over. Reached level %u\n", g_level);
            game_over_fx();
        } else {
            printf("You win!\n");
            victory_fx();
        }

        sleep_ms(1500);
    }
}