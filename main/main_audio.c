#define AUDIO_PIN 26

const int BTN_latido = 2;
const int BTN_botucatu = 3;
const int BTN_pausa = 4;

typedef struct {
    volatile int wav_position;
    volatile int wav_data_lenght;
    const uint8_t* volatile audio_tocando;
    volatile bool pausado;
} audio_ctx_t;

static audio_ctx_t g_audio = {0, 0, NULL, false};

void pwm_interrupt_handler() {
    pwm_clear_irq(pwm_gpio_to_slice_num(AUDIO_PIN));    
    if (!g_audio.pausado && g_audio.audio_tocando != NULL && g_audio.wav_position < (g_audio.wav_data_lenght << 3) - 1) { 
        pwm_set_gpio_level(AUDIO_PIN, g_audio.audio_tocando[g_audio.wav_position >> 3]);  
        g_audio.wav_position++;
    } else {
        pwm_set_gpio_level(AUDIO_PIN, 0);
    }
}

void escolhe_audio() {
    static bool ultimo_latido = true;
    static bool ultimo_botucatu = true;
    static bool ultimo_pausa = true;

    bool atual_latido = gpio_get(BTN_latido);
    bool atual_botucatu = gpio_get(BTN_botucatu);
    bool atual_pausa = gpio_get(BTN_pausa);

    if ((atual_latido == 0) && ultimo_latido == 1) {
        irq_set_enabled(PWM_IRQ_WRAP, false);
        g_audio.audio_tocando = WAV_DATA;
        g_audio.wav_data_lenght = WAV_DATA_LENGTH;
        g_audio.wav_position = 0;
        g_audio.pausado = false;
        irq_set_enabled(PWM_IRQ_WRAP, true);
    } else if ((atual_botucatu == 0) && ultimo_botucatu == 1) {
        irq_set_enabled(PWM_IRQ_WRAP, false);
        g_audio.audio_tocando = WAV_DATA_VOZ;
        g_audio.wav_data_lenght = WAV_DATA_LENGTH_VOZ;
        g_audio.wav_position = 0;
        g_audio.pausado = false;
        irq_set_enabled(PWM_IRQ_WRAP, true);
    } else if ((atual_pausa == 0) && ultimo_pausa == 1) {
        if (g_audio.audio_tocando != NULL) {
            g_audio.pausado = !g_audio.pausado;
            irq_set_enabled(PWM_IRQ_WRAP, !g_audio.pausado);
            if(g_audio.pausado) {
                pwm_set_gpio_level(AUDIO_PIN, 0);
            }
        }
    }
    ultimo_latido = atual_latido;
    ultimo_botucatu = atual_botucatu;
    ultimo_pausa = atual_pausa;
}

int main() {
    stdio_init_all();
    set_sys_clock_khz(176000, true); 
    gpio_set_function(AUDIO_PIN, GPIO_FUNC_PWM);

    gpio_init(BTN_latido);
    gpio_set_dir(BTN_latido, GPIO_IN);
    gpio_pull_up(BTN_latido);

    gpio_init(BTN_botucatu);
    gpio_set_dir(BTN_botucatu, GPIO_IN);
    gpio_pull_up(BTN_botucatu);

    gpio_init(BTN_pausa);
    gpio_set_dir(BTN_pausa, GPIO_IN);
    gpio_pull_up(BTN_pausa);

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