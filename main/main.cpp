#include "Arduino.h"
#include <src/Stepper.h>
#include <Adafruit_NeoPixel.h>

#include <stdio.h>
#include <inttypes.h>
#include "esp_spiffs.h"
#include "esp_random.h"
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include <time.h>

extern "C"
{
#include "llm.h"
#include "sound.h"
#include <ESP8266SAM.h>
}

const int stepsPerRevolution = 2048;  // change this to fit the number of steps per revolution
static const char *TAG = "MAIN";
char *text = nullptr;

// default parameters
char *checkpoint_path = "/data/tiny_dalek.bin"; // e.g. out/model.bin
char *tokenizer_path = "/data/tok512.bin";
float temperature = 0.25f;        // 0.0 = greedy deterministic. 1.0 = original. don't set higher
float topp = 0.9f;               // top-p in nucleus sampling. 1.0 = off. 0.9 works well, but slower
int steps = 128;                 // number of steps to run for
unsigned long long rng_seed = 0; // seed rng with time by default
int32_t stepper_pos = 0;         // initial position of stepper motor

TaskHandle_t stepperTask = NULL;
TaskHandle_t ledsTask = NULL;

// ULN2003 Motor Driver Pins
#define IN1 5
#define IN2 18
#define IN3 19
#define IN4 21

#define PIN       15
#define NUMPIXELS 16

Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);
#define DELAYVAL 50

// initialize the stepper library
Stepper myStepper(stepsPerRevolution, IN1, IN3, IN2, IN4);

Transformer transformer;
Tokenizer tokenizer;
Sampler sampler;

void init_stepper() {
  // set the speed at 5 rpm
  myStepper.setSpeed(10);
//   // initialize the serial port
//   Serial.begin(115200);
}

void run_stepper(void *param) {
    uint32_t random_number = *((int *)param);
    stepper_pos = random_number * 2;
    if (random_number % 2 == 0) {
        stepper_pos = -stepper_pos;
    }
    ESP_LOGI(TAG, "stepper_pos: %d", stepper_pos);
    while (true) {
        // step one revolution in one direction:
        myStepper.step(stepper_pos);
        delay(100);

        // step one revolution in the other direction:
        myStepper.step(-stepper_pos);
        delay(100);
        stepper_pos = 0;
    }
}

void init_leds() {
  pixels.begin();
  //pixels.setBrightness(122); // Set BRIGHTNESS to about 4% (max = 255)
}

uint32_t generate_random_number() {
    // generate random number
    uint32_t random_number = esp_random();
    // Random value between 0 and UINT32_MAX
    // scale it to 255
    random_number = (random_number % 255) + 1;
    ESP_LOGD(TAG, "Random number: %d\n", random_number);
    return random_number;
}

void run_leds(void *param) {
    while (true) {
        uint32_t random_number = generate_random_number();
        pixels.clear();
        pixels.show();
        delay(random_number);
        pixels.fill(pixels.Color(256 - random_number, random_number / 2, random_number));
        pixels.show();
        delay(random_number);
        ESP_LOGD(TAG, "LEDs done\n");
    }
}

void turn_off_leds() {
    pixels.clear();
    pixels.show();
}

/**
 * @brief Initializes the display
 *
 */
 void init_audio(void)
 {
    size_t bytes_written;
    int i2s_read_len = EXAMPLE_I2S_READ_LEN;
    uint8_t* i2s_write_buff = (uint8_t*) calloc(i2s_read_len, sizeof(char));

    example_i2s_init();
    example_set_file_play_mode();
    ESP_LOGI(TAG, "Audio initialized");
}

/**
 * @brief intializes SPIFFS storage
 *
 */
void init_storage(void)
{

    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/data",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = false};

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
}

bool output_audio(void *cbdata, int16_t* b) {
    size_t bytes_written;
    i2s_write((i2s_port_t)0, b, 2, &bytes_written, portMAX_DELAY);
    return true;
}

/**
 * @brief Outputs to display
 *
 * @param text The text to output
 */
void say_chunk(char *text)
{
    ESP8266SAM *sam = new ESP8266SAM(output_audio);
    sam->SetSpeed(120);
    sam->SetPitch(100);
    sam->SetThroat(100);
    sam->SetMouth(200);
    sam->Say(text);
    ESP_LOGI(TAG, "Audio output complete");
    //vTaskDelay(500 / portTICK_RATE_MS);
    delete sam;
}

void say_text(char *text)
{
        // say in chunks of 128 characters
    int len = strlen(text);
    int start = 0;
    int end = 64;
    while (start < len)
    {
        if (end > len)
        {
            end = len;
        }
        char *chunk = (char *)malloc(end - start + 1);
        memcpy(chunk, text + start, end - start);
        chunk[end - start] = '\0';
        ESP_LOGI(TAG, "Saying: %s", chunk);
        say_chunk(chunk);
        start = end;
        end += 64;
    }
}

void say_with_animation(uint32_t random_number) {
    // start stepper and leds using freertos threads
    xTaskCreate(run_stepper, "run_stepper", 4096, &random_number, 5, &stepperTask);
    xTaskCreate(run_leds, "run_leds", 4096, &random_number, 5, &ledsTask);
    say_text(text);
    vTaskDelete(ledsTask);
    turn_off_leds();
    while (stepper_pos != 0)
    {
        delay(10);
    }
    vTaskDelete(stepperTask);
}

/**
 * @brief Callbacks once generation is done
 *
 * @param tk_s The number of tokens per second generated
 */
void generate_complete_cb(char *generated_text, int ix, float tk_s)
{
    //truncate the generated text to ix
    text = (char *)malloc(ix);
    memcpy(text, generated_text, ix);

    ESP_LOGI(TAG, "Generated text: %s", text);
    ESP_LOGI(TAG, "Tokens per second: %.2f", tk_s);
    // say_text(text);
    // say_with_animation(text);
}

void init_llm(uint32_t random_number) {
    // parameter validation/overrides
    if (rng_seed <= 0)
        rng_seed = random_number;

    // build the Transformer via the model .bin file
    ESP_LOGI(TAG, "LLM Path is %s", checkpoint_path);
    build_transformer(&transformer, checkpoint_path);
    if (steps == 0 || steps > transformer.config.seq_len)
        steps = transformer.config.seq_len; // override to ~max length

    // build the Tokenizer via the tokenizer .bin file
    build_tokenizer(&tokenizer, tokenizer_path, transformer.config.vocab_size);

    // build the Sampler
    build_sampler(&sampler, transformer.config.vocab_size, temperature, topp, rng_seed);
}

void generate_text(uint32_t random_number)
{
    char *prompt = nullptr;

    if (random_number % 4 == 0) {
        prompt = (char*)malloc(4);
        prompt[0] = 'E';
        prompt[1] = 'X';
        prompt[2] = 'T';
        prompt[3] = '\0';
    }
    else {
        prompt = (char*)malloc(2);
        char letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        char letter = letters[random_number % 26];
        prompt[0] = letter;
        prompt[1] = '\0';
    }

    printf("Prompt is %s\n", prompt);

    // run!
    generate(&transformer, &tokenizer, &sampler, prompt, steps, &generate_complete_cb);
    free(prompt);
}

extern "C" void app_main()
{
    //initArduino();
    //Serial.begin(115200);
    pinMode(23, INPUT);

    uint32_t random_number = generate_random_number();

    init_leds();
    init_stepper();
    init_storage();
    init_llm(random_number);

    generate_text(random_number);

    while (true)
    {
        ESP_LOGI(TAG, "Waiting for PIR");
        while (true)
        {
            if (digitalRead(23) == HIGH)
            {
                break;
            }
            delay(50);
        }
        init_audio();
        say_with_animation(random_number);
        deinit_audio();
        random_number = generate_random_number();
        generate_text(random_number);
        // deinit the audio

    }

    //run_leds(nullptr);
    //run_stepper(nullptr);
}