#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "pico/multicore.h"

const uint ECHO_PIN = 5;
const uint TRIGGER_PIN = 2;
const int VELOCIDADE_SOM = 343;

#define CMD_START 1
#define CMD_STOP  2
#define CMD_PERIODO_BASE 100
#define ERRO_LEITURA 0xFFFFFFFF

static volatile uint64_t tempo_1 = 0;
static volatile uint64_t tempo_2 = 0;
static volatile uint64_t delta = 0;
static volatile int leitura_pronta = 0;
static volatile int falha_sensor = 0;
static volatile alarm_id_t alarm_id = 0;
static volatile int trigger_flag = 0;

int64_t alarm_callback(alarm_id_t id, void *user_data) {
    falha_sensor = 1;
    leitura_pronta = 1;
    return 0;
}

void echo_callback(uint gpio, uint32_t events) {
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

bool timer_callback(repeating_timer_t *t) {
    const bool *ativo = (const bool *)t->user_data;
    if (*ativo) {
        trigger_flag = 1;
    }
    return true;
}

void liga_trigger() {
    gpio_put(TRIGGER_PIN, 1);
    sleep_us(10);
    gpio_put(TRIGGER_PIN, 0);
}

void core1_entry() {

    bool sistema_ativo = false;
    int periodo = 3;

    gpio_init(TRIGGER_PIN);
    gpio_set_dir(TRIGGER_PIN, GPIO_OUT);

    gpio_init(ECHO_PIN);
    gpio_set_dir(ECHO_PIN, GPIO_IN);

    gpio_set_irq_enabled_with_callback(
        ECHO_PIN,
        GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
        true,
        &echo_callback
    );

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

        // trigger
        if (trigger_flag) {
            liga_trigger();
            trigger_flag = 0;
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

//  core 0 

int main() {
    stdio_init_all();

    multicore_launch_core1(core1_entry);

    bool sistema_ativo_local = false;
    int periodo_local = 3;
    uint32_t tick = 0;

    while (true) {

        int c = getchar_timeout_us(0);

        // UART
        if (c != PICO_ERROR_TIMEOUT) {

            if (c == 's') {
                sistema_ativo_local = true;
                multicore_fifo_push_blocking(CMD_START);
                printf("Sistema iniciado\n");

            } else if (c == 'p') {
                sistema_ativo_local = false;
                multicore_fifo_push_blocking(CMD_STOP);
                printf("Sistema parado\n");

            } else if (c >= '1' && c <= '9') {
                periodo_local = c - '0';
                multicore_fifo_push_blocking(CMD_PERIODO_BASE + periodo_local);
                printf("Novo periodo: %d s\n", periodo_local);
            }
        }

        // recebe do core 1 e dá os printizinhos
        if (multicore_fifo_rvalid() && sistema_ativo_local) {
            uint32_t dado = multicore_fifo_pop_blocking();

            tick += periodo_local;

            if (dado == ERRO_LEITURA) {
                printf("-%u s - Falha\n", tick);
            } else {
                printf("%u s - %u cm\n", tick, dado);
            }
        }
    }
}