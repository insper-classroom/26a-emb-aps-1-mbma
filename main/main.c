//  * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
//  * SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"

#include "blue.h"
#include "yellow.h"
#include "red.h"
#include "green.h"
#include "win.h"
#include "ga.h"

// Pinos
#define AUDIO_PIN  14
#define SWITCH_PIN 17

// Parametros do jogo
#define MAX_LEVELS       7
#define BUTTON_COUNT     4
#define TIMEOUT_US       15000000u // 15s
#define DEBOUNCE_US      250000u // 250ms 
#define AUDIO_STEP       1 // velocidade voz
#define LED_OFF_MS_SEQ   240
#define LED_OFF_MS_PRESS 120
#define CMD_AUDIO_STOP 0xFFFFFFFFu // para o audio imediatamente
#define CMD_AUDIO_WIN  0xFFFFFFFEu // toca som de vitoria
#define CMD_AUDIO_LOSE 0xFFFFFFFDu // toca som de derrota
#define RESP_AUDIO_DONE 0x00000001u // audio terminou

// Tabelas de audio (0=GREEN, 1=YELLOW, 2=BLUE, 3=RED)
static const uint8_t *const AUDIO_DATA[BUTTON_COUNT] = {
    WAV_DATA_GREEN, WAV_DATA_YELLOW, WAV_DATA_BLUE, WAV_DATA_RED
};
static const uint32_t AUDIO_LEN[BUTTON_COUNT] = {
    WAV_DATA_LENGTH_GREEN, WAV_DATA_LENGTH_YELLOW,
    WAV_DATA_LENGTH_BLUE,  WAV_DATA_LENGTH_RED
};

// Lista leds, botões e cores
static const uint LEDS[BUTTON_COUNT]    = {10, 11, 12, 13};
static const uint BUTTONS[BUTTON_COUNT] = {27, 26, 22, 16};
static const char *COLOR_NAMES[BUTTON_COUNT] = {"GREEN", "YELLOW", "BLUE", "RED"};

// Ordem fisica dos LEDs para exibicao binaria da pontuacao
static const uint8_t ORDEM_PONTUACAO[BUTTON_COUNT] = {1, 3, 0, 2};

// Estado do jogo  (apenas Core 0 escreve)
static uint8_t g_sequence[MAX_LEVELS];
static uint8_t g_level       = 0;
static uint8_t g_player_pos  = 0;
static uint8_t g_final_score = 0;

static volatile bool     g_input_blocked          = true;
static volatile bool     g_game_over              = false;
static volatile bool     g_button_event[BUTTON_COUNT]  = {false};
static volatile bool     g_button_locked[BUTTON_COUNT] = {false};
static volatile uint32_t g_last_press_us[BUTTON_COUNT] = {0};
static volatile uint32_t g_last_input_us           = 0;

// Estado do audio  (apenas Core 1 escreve - PWM)
static volatile const uint8_t *g_audio_data    = NULL;
static volatile uint32_t       g_audio_len     = 0;
static volatile uint32_t       g_audio_pos     = 0;
static volatile bool           g_audio_playing = false;
static int                     g_audio_slice   = 0;

// CORE 1 — gerenciamento de audio via PWM
static void pwm_interrupt_handler(void) {
    pwm_clear_irq(g_audio_slice);

    if (g_audio_playing && g_audio_data != NULL && g_audio_pos < (g_audio_len << 3)) {
        pwm_set_gpio_level(AUDIO_PIN, g_audio_data[g_audio_pos >> 3] / 8);
        g_audio_pos += AUDIO_STEP;
    } else {
        pwm_set_gpio_level(AUDIO_PIN, 0);
        if (g_audio_playing) {
            g_audio_playing = false;
            // Avisa o Core 0 que o audio terminou
            multicore_fifo_push_blocking(RESP_AUDIO_DONE);
        }
    }
}

static void core1_audio_play(const uint8_t *data, uint32_t len) {
    irq_set_enabled(PWM_IRQ_WRAP, false);
    g_audio_playing = false;
    g_audio_data    = data;
    g_audio_len     = len;
    g_audio_pos     = 0;
    g_audio_playing = true;
    irq_set_enabled(PWM_IRQ_WRAP, true);
}

static void core1_audio_stop(void) {
    irq_set_enabled(PWM_IRQ_WRAP, false);
    g_audio_playing = false;
    g_audio_data    = NULL;
    g_audio_len     = 0;
    g_audio_pos     = 0;
    pwm_set_gpio_level(AUDIO_PIN, 0);
    irq_set_enabled(PWM_IRQ_WRAP, true);
}

void core1_entry(void) {
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

    //espera comandos do Core 0 via FIFO
    while (true) {
        uint32_t cmd = multicore_fifo_pop_blocking();

        if (cmd == CMD_AUDIO_STOP) {
            core1_audio_stop();

        } else if (cmd == CMD_AUDIO_WIN) {
            core1_audio_play(WAV_DATA_WIN, WAV_DATA_LENGTH_WIN);

        } else if (cmd == CMD_AUDIO_LOSE) {
            core1_audio_play(WAV_DATA_GA, WAV_DATA_LENGTH_GA);

        } else if (cmd < BUTTON_COUNT) {
            core1_audio_play(AUDIO_DATA[cmd], AUDIO_LEN[cmd]);
        }
    }
}

// CORE 0 — logica do jogo e botoes

// Envia comando e bloqueia ate o Core 1 sinalizar que o audio terminou
static void audio_play_and_wait(uint32_t cmd) {
    multicore_fifo_push_blocking(cmd);
    uint32_t resp;
    do {
        resp = multicore_fifo_pop_blocking();
    } while (resp != RESP_AUDIO_DONE);
}

// Para o audio sem esperar resposta
static void audio_stop_request(void) {
    multicore_fifo_push_blocking(CMD_AUDIO_STOP);
}

// GPIO / botoes
static int color_from_gpio(uint gpio) {
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (BUTTONS[i] == gpio) return i;
    }
    return -1;
}
static void all_leds_off(void) {
    for (int i = 0; i < BUTTON_COUNT; i++) gpio_put(LEDS[i], 0);
}
static void flush_button_events(void) {
    uint32_t now = time_us_32();
    for (int i = 0; i < BUTTON_COUNT; i++) {
        g_button_event[i]  = false;
        g_button_locked[i] = false;
        g_last_press_us[i] = now;
    }
}

static void gpio_callback(uint gpio, uint32_t events) {
    int idx = color_from_gpio(gpio);
    if (idx < 0) return;

    uint32_t now = time_us_32();

    if (events & GPIO_IRQ_EDGE_FALL) {
        if (g_button_locked[idx]) return;
        if ((now - g_last_press_us[idx]) < DEBOUNCE_US) return;

        g_last_press_us[idx] = now;
        g_button_locked[idx] = true;

        if (!g_input_blocked) {
            g_button_event[idx] = true;
        }
    }

    if (events & GPIO_IRQ_EDGE_RISE) {
        // So desbloqueia apos o debounce, evitando bounce no release
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
        BUTTONS[0], GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true, &gpio_callback);

    for (int i = 1; i < BUTTON_COUNT; i++) {
        gpio_set_irq_enabled(BUTTONS[i], GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true);
    }
}

// Logica do jogo

// Acende LED, pede audio ao Core 1, aguarda terminar, apaga LED
static void blink_led_com_audio(int idx, uint32_t off_ms) {
    gpio_put(LEDS[idx], 1);
    audio_play_and_wait((uint32_t)idx);
    gpio_put(LEDS[idx], 0);
    sleep_ms(off_ms);
}

static void game_reset(void) {
    g_input_blocked = true;
    g_level         = 0;
    g_player_pos    = 0;
    g_game_over     = false;
    g_final_score   = 0;

    all_leds_off();
    audio_stop_request();

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
    for (uint8_t i = 0; i < g_level; i++) printf("%s ", COLOR_NAMES[g_sequence[i]]);
    printf("\n");

    for (uint8_t i = 0; i < g_level; i++) {
        blink_led_com_audio(g_sequence[i], LED_OFF_MS_SEQ);
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
            printf("Nivel %u completo!\n", g_level);
            g_final_score = g_level;
        }
    } else {
        printf("Errou: %s\n", COLOR_NAMES[idx]);
        g_game_over = true;
    }
}

static void wait_player_repeat(void) {
    g_player_pos    = 0;
    g_last_input_us = time_us_32();
    g_input_blocked = false;

    while (!g_game_over && g_player_pos < g_level) {
        for (int i = 0; i < BUTTON_COUNT; i++) {
            if (g_button_event[i]) {
                // Confirmacao: descarta se o pino ja voltou para alto
                if (gpio_get(BUTTONS[i]) != 0) {
                    g_button_event[i] = false;
                    continue;
                }
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

static void show_score_blink(uint8_t score) {
    printf("Pontuacao final: %u nivel(is)\n", score);

    // Etapa 1: exibe a pontuacao em binario nos LEDs por 3 segundos
    for (int i = 0; i < BUTTON_COUNT; i++) {
        gpio_put(LEDS[ORDEM_PONTUACAO[i]], (score >> i) & 1);
    }
    sleep_ms(3000);
    all_leds_off();
    sleep_ms(400);

    // Etapa 2: pisca a quantidade de niveis alcancados sequencialmente
    for (uint8_t n = 0; n < score; n++) {
        int idx_led = ORDEM_PONTUACAO[n % BUTTON_COUNT];
        gpio_put(LEDS[idx_led], 1);
        sleep_ms(300);
        gpio_put(LEDS[idx_led], 0);
        sleep_ms(150);
    }
    all_leds_off();
}

// main — Core 0
int main(void) {
    stdio_init_all();
    set_sys_clock_khz(176000, true);
    srand(time_us_32());

    multicore_launch_core1(core1_entry);

    gpio_init_game();

    sleep_ms(2000);
    printf("Genius\n");
    while (true) {
        printf("Aguardando interruptor ligar\n");
        while (gpio_get(SWITCH_PIN) == 1) tight_loop_contents();

        printf("Interruptor ON! Jogo começçando\n");
        sleep_ms(500);

        game_reset();

        while (!g_game_over && g_level < MAX_LEVELS) {
            sequence_add_step();
            printf("Nivel %u\n", g_level);
            sequence_show();
            wait_player_repeat();

            if (!g_game_over) {
                sleep_ms(250);
                flush_button_events();
            }
        }

        if (g_game_over) {
            printf("Game over. Pontuação: %u\n", g_final_score);
            audio_play_and_wait(CMD_AUDIO_LOSE);
            game_over_fx();
            show_score_blink(g_final_score);
        } else {
            printf("Vc venceu! Pontuação: %u\n", g_final_score);
            audio_play_and_wait(CMD_AUDIO_WIN);
            victory_fx();
            show_score_blink(g_final_score);
        }

        sleep_ms(1500);
    }
}