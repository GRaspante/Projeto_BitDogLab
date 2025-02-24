#include <stdio.h>
#include <math.h>
#include <string.h>  
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/i2c.h"
#include "ssd1306.h"  
#include "neopixel.c"
#include "ssd1306_i2c.c"

// Pino e canal do microfone no ADC.
#define MIC_CHANNEL 2
#define MIC_PIN (26 + MIC_CHANNEL)

// Parâmetros e macros do ADC.
#define ADC_CLOCK_DIV 96.f
#define SAMPLES 200 // Número de amostras que serão feitas do ADC.
#define ADC_ADJUST(x) (x * 3.3f / (1 << 12u) - 1.65f) // Ajuste do valor do ADC para Volts.
#define ADC_MAX 3.3f
#define ADC_STEP (3.3f / 5.5f) // Intervalos de volume do microfone.

// Pino e número de LEDs da matriz de LEDs.
#define LED_PIN 7
#define LED_COUNT 25

// Definições do display SSD1306
#define SSD1306_WIDTH 128
#define SSD1306_HEIGHT 64
#define SSD1306_I2C_ADDR 0x3C
#define SSD1306_I2C i2c1

// Buffer de amostras do ADC.
uint16_t adc_buffer[SAMPLES];

// SSD1306
ssd1306_t ssd1306;

// Buffer para enviar os dados ao display
uint8_t display_buffer[SSD1306_WIDTH * SSD1306_HEIGHT / 8];

// Canal e configurações do DMA
uint dma_channel;
dma_channel_config dma_cfg;

// Regiao da barra
struct render_area frame_area;

void sample_mic();
float mic_power();
uint8_t get_intensity(float v);
void update_display(float intensity);

int main() {
    stdio_init_all();   

    // Preparação da matriz de LEDs.
    npInit(LED_PIN, LED_COUNT); 

    // Inicialização do I2C
    i2c_init(SSD1306_I2C, 400 * 1000);  // 400 kHz I2C clock speed
    gpio_set_function(14, GPIO_FUNC_I2C);  // SDA pin
    gpio_set_function(15, GPIO_FUNC_I2C);  // SCL pin
    gpio_pull_up(14);
    gpio_pull_up(15);

    // Inicialização do display SSD1306
    ssd1306_init();

    // Preparar área de renderização para o display
    frame_area.start_column = 0;
    frame_area.end_column = SSD1306_WIDTH - 1;
    frame_area.start_page = 0;
    frame_area.end_page = ssd1306_n_pages - 1;

    calculate_render_area_buffer_length(&frame_area);

    // Limpa o display inteiro
    memset(display_buffer, 0, ssd1306_buffer_length);
    render_on_display(display_buffer, &frame_area);

    // Preparação do ADC.

    adc_gpio_init(MIC_PIN);
    adc_init();
    adc_select_input(MIC_CHANNEL);

    adc_fifo_setup(
        true,  // Habilitar FIFO
        true,  // Habilitar request de dados do DMA
        1,     // Threshold para ativar request DMA é 1 leitura do ADC
        false, // Não usar bit de erro
        false  // Não fazer downscale das amostras para 8-bits, manter 12-bits.
    );

    adc_set_clkdiv(ADC_CLOCK_DIV);

    // Tomando posse de canal do DMA.
    dma_channel = dma_claim_unused_channel(true);

    // Configurações do DMA.
    dma_cfg = dma_channel_get_default_config(dma_channel);

    channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_16); // Tamanho da transferência é 16-bits
    channel_config_set_read_increment(&dma_cfg, false); // Desabilita incremento do ponteiro de leitura
    channel_config_set_write_increment(&dma_cfg, true); // Habilita incremento do ponteiro de escrita
    channel_config_set_dreq(&dma_cfg, DREQ_ADC); // Usamos a requisição de dados do ADC

    // Amostragem de teste.
    sample_mic();

 
    while (true) {
        // Realiza uma amostragem do microfone.
        sleep_ms(10);
        sample_mic();

        // Pega a potência média da amostragem do microfone.
        float avg = mic_power();
        avg = 2.f * fabs(ADC_ADJUST(avg)); // Ajusta para intervalo de 0 a 3.3V. (apenas magnitude, sem sinal)

        uint intensity = get_intensity(avg); // Calcula intensidade a ser mostrada na matriz de LEDs.

        // Atualiza o display com a intensidade do som.
        update_display(intensity);

        // Limpa a matriz de LEDs.
        npClear();

        // A depender da intensidade do som, acende LEDs específicos.
         switch (intensity) {
      case 1:
        npSetLED(12, 0, 255, 0); // Verde
        break;
      case 2:
        npSetLED(12, 0, 255, 0);
        npSetLED(7, 0, 255, 0);
        npSetLED(17, 0, 255, 0);
        break;
      case 3:
        npSetLED(12, 255, 255, 0); // Amarelo
        npSetLED(7, 255, 255, 0);
        npSetLED(17, 255, 255, 0);
        npSetLED(2, 255, 255, 0);
        npSetLED(22, 255, 255, 0);
        break;
      case 4:       
        npSetLED(12, 255, 0, 0); // Vermelho
        npSetLED(7, 255, 0, 0);
        npSetLED(17, 255, 0, 0);
        npSetLED(2, 255, 0, 0);
        npSetLED(22, 255, 0, 0);
        npSetLED(5, 255, 0, 0);
        npSetLED(19, 255, 0, 0);
        npSetLED(20, 255, 0, 0);
        npSetLED(21, 255, 0, 0);
        npSetLED(23, 255, 0, 0);
        npSetLED(24, 255, 0, 0);        
        break;
    }
        // Atualiza a matriz.
        npWrite();
               
    }
}

/**
 * Realiza as leituras do ADC e armazena os valores no buffer.
 */
void sample_mic() {
    adc_fifo_drain(); // Limpa o FIFO do ADC.
    adc_run(false); // Desliga o ADC (se estiver ligado) para configurar o DMA.

    dma_channel_configure(dma_channel, &dma_cfg,
        adc_buffer, // Escreve no buffer.
        &(adc_hw->fifo), // Lê do ADC.
        SAMPLES, // Faz SAMPLES amostras.
        true // Liga o DMA.
    );

    // Liga o ADC e espera acabar a leitura.
    adc_run(true);
    dma_channel_wait_for_finish_blocking(dma_channel);

    // Acabou a leitura, desliga o ADC de novo.
    adc_run(false);
}

/**
 * Calcula a potência média das leituras do ADC. (Valor RMS)
 */
float mic_power() {
    float avg = 0.f;

    for (uint i = 0; i < SAMPLES; ++i)
        avg += adc_buffer[i] * adc_buffer[i];

    avg /= SAMPLES;
    return sqrt(avg);
}

/**
 * Calcula a intensidade do volume registrado no microfone, de 0 a 3, usando a tensão.
 */
uint8_t get_intensity(float v) {
    uint count = 0;

    while ((v -= ADC_STEP / 25) > 0.f)
        ++count;

    return count;
}

/**
 * Atualiza o display com a intensidade do som.
 */
void update_display(float intensity) {
    // Limpa o buffer do display.
    memset(display_buffer, 0, ssd1306_buffer_length);

    // Desenha uma barra horizontal proporcional à intensidade do som.
    int bar_width = (int)(intensity * SSD1306_WIDTH / 8); // Normaliza a intensidade para a largura do display.
    printf("%d.\n" , bar_width);
    if (bar_width >= 144){
        bar_width = 100;
    }
    for (int x = 0; x < bar_width * 0.9; x++) {
        for (int y = SSD1306_HEIGHT - 10; y < SSD1306_HEIGHT; y++) {
            ssd1306_set_pixel(display_buffer, x, y, true); // Desenha a barra.
        }
    }
    // Atualiza o display.
    render_on_display(display_buffer, &frame_area);    
}