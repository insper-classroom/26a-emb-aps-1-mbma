#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "blue.h"
#include "yellow.h"
#include "red.h"
#include "green.h"

#define AUDIO_PIN 14

const int LED_GREEN = 10;
const int LED_YELLOW = 11;
const int LED_BLUE = 12;
const int LED_RED = 13;

const int BUTTON_GREEN = 27;
const int BUTTON_YELLOW = 26;
const int BUTTON_BLUE = 22;
const int BUTTON_RED = 16;

typedef struct {
    volatile int wav_position;
    volatile int wav_data_lenght;
    const uint8_t* volatile audio_tocando;
} audio_ctx_t;

static audio_ctx_t g_audio = {0, 0, NULL};

void pwm_interrupt_handler() {
    pwm_clear_irq(pwm_gpio_to_slice_num(AUDIO_PIN));    
    if (g_audio.audio_tocando != NULL && g_audio.wav_position < (g_audio.wav_data_lenght << 3) - 1) { 
        pwm_set_gpio_level(AUDIO_PIN, g_audio.audio_tocando[g_audio.wav_position >> 3]);  
        g_audio.wav_position++;
    } else {
        pwm_set_gpio_level(AUDIO_PIN, 0);
    }
}

void escolhe_audio() {
    bool buttton_blue = gpio_get(BUTTON_BLUE) == 0;
    bool button_yellow = gpio_get(BUTTON_YELLOW) == 0;
    bool button_red = gpio_get(BUTTON_RED) == 0;
    bool button_green = gpio_get(BUTTON_GREEN) == 0;
    if (buttton_blue) {
        irq_set_enabled(PWM_IRQ_WRAP, false);
        g_audio.audio_tocando = WAV_DATA_BLUE;
        g_audio.wav_data_lenght = WAV_DATA_LENGTH_BLUE;
        g_audio.wav_position = 0;
        irq_set_enabled(PWM_IRQ_WRAP, true);
    } else if (button_yellow) {
        irq_set_enabled(PWM_IRQ_WRAP, false);
        g_audio.audio_tocando = WAV_DATA_YELLOW;
        g_audio.wav_data_lenght = WAV_DATA_LENGTH_YELLOW;
        g_audio.wav_position = 0;
        irq_set_enabled(PWM_IRQ_WRAP, true);
    } else if (button_red) {
        irq_set_enabled(PWM_IRQ_WRAP, false);
        g_audio.audio_tocando = WAV_DATA_RED;
        g_audio.wav_data_lenght = WAV_DATA_LENGTH_RED;
        g_audio.wav_position = 0;
        irq_set_enabled(PWM_IRQ_WRAP, true);
    } else if (button_green) {
        irq_set_enabled(PWM_IRQ_WRAP, false);
        g_audio.audio_tocando = WAV_DATA_GREEN;
        g_audio.wav_data_lenght = WAV_DATA_LENGTH_GREEN;
        g_audio.wav_position = 0;
        irq_set_enabled(PWM_IRQ_WRAP, true);
    }

}


int main() {
    stdio_init_all();
    set_sys_clock_khz(176000, true); 
    gpio_set_function(AUDIO_PIN, GPIO_FUNC_PWM);

    gpio_init(LED_GREEN);
    gpio_set_dir(LED_GREEN, GPIO_OUT);
    gpio_init(LED_YELLOW);
    gpio_set_dir(LED_YELLOW, GPIO_OUT);
    gpio_init(LED_BLUE);
    gpio_set_dir(LED_BLUE, GPIO_OUT);
    gpio_init(LED_RED);
    gpio_set_dir(LED_RED, GPIO_OUT);
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

    int audio_pin_slice = pwm_gpio_to_slice_num(AUDIO_PIN);
    pwm_clear_irq(audio_pin_slice);
    pwm_set_irq_enabled(audio_pin_slice, true);
    
    irq_set_exclusive_handler(PWM_IRQ_WRAP, pwm_interrupt_handler); 
    irq_set_enabled(PWM_IRQ_WRAP, true);

    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, 8.0f); 
    pwm_config_set_wrap(&config, 250); 
    pwm_init(audio_pin_slice, &config, true);

    pwm_set_gpio_level(AUDIO_PIN, 0);

    while(1){
        escolhe_audio();
        sleep_ms(20);
    }
}