/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */


#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "pico/multicore.h"

// botoes e leds
const int LED_GREEN = 10;
const int LED_YELLOW = 11;
const int LED_BLUE = 12;
const int LED_RED = 13;
const int BUTTON_GREEN = 27;
const int BUTTON_YELLOW = 26;
const int BUTTON_BLUE = 22;
const int BUTTON_RED = 16;


// struct de comunicacao entre cores 
typedef struct {
    int falha;
    int info1;
    int info2;
    int info3;
} Comunicacao;
volatile Comunicacao comunicacao;

// id de alarme 
static volatile alarm_id_t alarm_id = 0;


// alarme de erro dentro da interrupção (callback) do core1
int64_t alarm_callback(alarm_id_t id, void *user_data) {
    falha = 1;
    return 0;
}


// funcao de interrupçao (callback) do core 1 
void echo_callback(uint gpio, uint32_t events) {
    // calcula tempo entre subida e descida de PORTA LOGICA
    if (events & GPIO_IRQ_EDGE_RISE) {
        tempo_1 = to_us_since_boot(get_absolute_time());
        falha_sensor = 0;
        alarm_id = add_alarm_in_us(30000, alarm_callback, NULL, false);
    } 
    else if (events & GPIO_IRQ_EDGE_FALL) {
        tempo_2 = to_us_since_boot(get_absolute_time());
        if (alarm_id > 0) cancel_alarm(alarm_id);

        delta = tempo_2 - tempo_1;
        leitura_pronta = 1;
    }
}


// timer do callback dentro do core1
bool timer_callback(repeating_timer_t *t) {
    const bool *ativo = (const bool *)t->user_data;
    if (*ativo) {
        trigger_flag = 1;
    }
    return true;
}





// CORE 1 /////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void core1_entry() {

    // inicializa LEDs como saida
    gpio_init(LED_GREEN);
    gpio_set_dir(LED_GREEN, GPIO_OUT);
    gpio_init(LED_YELLOW);
    gpio_set_dir(LED_YELLOW, GPIO_OUT);
    gpio_init(LED_BLUE);
    gpio_set_dir(LED_BLUE, GPIO_OUT);
    gpio_init(LED_RED);
    gpio_set_dir(LED_RED, GPIO_OUT);

    // inicializa botoes como entrada
    gpio_init(BUTTON_GREEN);
    gpio_set_dir(BUTTON_GREEN, GPIO_IN);
    gpio_pull_up(BUTTON_GREEN);
    gpio_init(BUTTON_YELLOW);
    gpio_set_dir(BUTTON_YELLOW, GPIO_IN);
    gpio_pull_up(BUTTON_YELLOW);
    gpio_init(BUTTON_BLUE);
    gpio_set_dir(BUTTON_BLUE, GPIO_IN);
    gpio_pull_up(BUTTON_BLUE);
    gpio_init(BUTTON_RED);
    gpio_set_dir(BUTTON_RED, GPIO_IN);
    gpio_pull_up(BUTTON_RED);

    // definindo funcao de interrupção do core1
    gpio_set_irq_enabled_with_callback(
        pinoA_DEFINIRR_R,///-------------------------------> pino a definir
        GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
        true,
        &echo_callback
    );

    // cria timer e o adiciona
    repeating_timer_t timer;
    add_repeating_timer_ms(periodo * 1000, timer_callback, &sistema_ativo, &timer);

    while (true) {

        // recebe comandos do core 0
        if (multicore_fifo_rvalid()) {
            uint32_t cmd = multicore_fifo_pop_blocking();

            if (cmd == CMD_START) sistema_ativo = true;
            else if (cmd == CMD_STOP) sistema_ativo = false;
            else if (cmd >= CMD_PERIODO_BASE) {
                periodo = cmd - CMD_PERIODO_BASE;

                cancel_repeating_timer(&timer);
                add_repeating_timer_ms(periodo * 1000, timer_callback, &sistema_ativo, &timer);
            }
        }


        // leitura pronta
        if (leitura_pronta && sistema_ativo) {
            if (falha_sensor) {
                multicore_fifo_push_blocking(ERRO_LEITURA);
            } else {
                float distancia = ((delta / 1000000.0f) * VELOCIDADE_SOM) / 2.0f;
                uint32_t cm = (uint32_t)(distancia * 100.0f);
                multicore_fifo_push_blocking(cm);
            }
            leitura_pronta = 0;
        }
    }
}

//  CORE 0 /////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int main() {
    stdio_init_all();

    // cria core1
    multicore_launch_core1(core1_entry);

    while (true) {

        // interação no terminal - UART
        int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT) {
            if (cond1) {
                multicore_fifo_push_blocking(DADO1);
                printf("Sistema iniciado\n");

            } else if (cond2) {
                multicore_fifo_push_blocking(DADO2);
                printf("Sistema parado\n");

            } else if (cond3) {
                multicore_fifo_push_blocking(DADO3);
                printf("Novo periodo: %d s\n", periodo_local);
            }
        }

        // recebe do core 1 e dá os printizinhos
        if (multicore_fifo_rvalid() && sistema_ativo_local) {
            uint32_t dado = multicore_fifo_pop_blocking();
            if (dado == ERRO_LEITURA) {
                printf("Falha\n");

            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////