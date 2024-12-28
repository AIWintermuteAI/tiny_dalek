#include "Arduino.h"
#include <ESP32Servo.h>
#include <Adafruit_NeoPixel.h>

#include <stdio.h>
#include <inttypes.h>
#include "esp_spiffs.h"
#include "esp_random.h"
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include <time.h>

#include "wifi_hal.hpp"

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
int32_t servo_pos = 0;         // initial position of servo motor

TaskHandle_t servoTask = NULL;
TaskHandle_t ledsTask = NULL;

#define DELAYPERDEGREE 5
#define SAFETYMARGIN 110

#define PIN       15
#define NUMPIXELS 16

Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);
#define DELAYVAL 50

// initialize the servo library
Servo neck_servo;

Transformer transformer;
Tokenizer tokenizer;
Sampler sampler;

// Motor A
#define motor1Pin1 15
#define motor1Pin2 2
// Motor B
#define motor2Pin1 0
#define motor2Pin2 4

void init_motors() {
  // set all the motor control pins to outputs
  pinMode(motor1Pin1, OUTPUT);
  pinMode(motor1Pin2, OUTPUT);
  pinMode(motor2Pin1, OUTPUT);
  pinMode(motor2Pin2, OUTPUT);
}

void run_motors(uint8_t direction) {
    switch (direction) {
        case 0:
            // Motor A
            digitalWrite(motor1Pin1, HIGH);
            digitalWrite(motor1Pin2, LOW);
            // Motor B
            digitalWrite(motor2Pin1, LOW);
            digitalWrite(motor2Pin2, HIGH);
            break;
        case 1:
            // Motor A
            digitalWrite(motor1Pin1, LOW);
            digitalWrite(motor1Pin2, HIGH);
            // Motor B
            digitalWrite(motor2Pin1, HIGH);
            digitalWrite(motor2Pin2, LOW);
            break;
        case 2:
            // Motor A
            digitalWrite(motor1Pin1, LOW);
            digitalWrite(motor1Pin2, HIGH);
            // Motor B
            digitalWrite(motor2Pin1, LOW);
            digitalWrite(motor2Pin2, HIGH);
            break;
        case 3:
            // Motor A
            digitalWrite(motor1Pin1, HIGH);
            digitalWrite(motor1Pin2, LOW);
            // Motor B
            digitalWrite(motor2Pin1, HIGH);
            digitalWrite(motor2Pin2, LOW);
            break;
        default:
            // Motor A
            digitalWrite(motor1Pin1, LOW);
            digitalWrite(motor1Pin2, LOW);
            // Motor B
            digitalWrite(motor2Pin1, LOW);
            digitalWrite(motor2Pin2, LOW);
    }
}

uint32_t generate_random_number(uint32_t scale = 255) {
    // generate random number
    uint32_t random_number = esp_random();
    // Random value between 0 and UINT32_MAX
    // scale it
    random_number = (random_number % scale) + 1;
    ESP_LOGD(TAG, "Random number: %d\n", random_number);
    return random_number;
}

void init_servo() {
    // use servo instead
    neck_servo.attach(5);
    neck_servo.write(90);
}

uint32_t calculate_delay(uint32_t cur_pos, uint32_t target_pos)
{
  ESP_LOGI(TAG, "cur_pos: %d, target_pos: %d", cur_pos, target_pos);
  return ( cur_pos > target_pos? ((cur_pos - target_pos) * DELAYPERDEGREE * SAFETYMARGIN) / 100 : ((target_pos - cur_pos) * DELAYPERDEGREE * SAFETYMARGIN) / 100);
}

void rotate_servo(uint8_t angle) {
    ESP_LOGI(TAG, "servo_pos: %d", servo_pos);
    uint32_t servo_delay = calculate_delay(90, servo_pos);
    ESP_LOGI(TAG, "Delay: %d", servo_delay);
    neck_servo.write(angle);
    delay(servo_delay);
}

void run_servo(void *param) {
    uint8_t rot_cnt = 0;
    while (true) {
        while (rot_cnt < 3) {
            servo_pos = generate_random_number(180);
            ESP_LOGI(TAG, "servo_pos: %d", servo_pos);
            uint32_t servo_delay = calculate_delay(90, servo_pos);
            ESP_LOGI(TAG, "Delay: %d", servo_delay);
            neck_servo.write(servo_pos);
            delay(servo_delay);
            neck_servo.write(90);
            delay(servo_delay);
            servo_pos = 90;
            rot_cnt++;
        }
        delay(10);
    }
}

void init_leds() {
  pixels.begin();
  //pixels.setBrightness(122); // Set BRIGHTNESS to about 4% (max = 255)
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
    // start servo and leds using freertos threads
    xTaskCreate(run_servo, "run_servo", 4096, &random_number, 5, &servoTask);
    xTaskCreate(run_leds, "run_leds", 4096, &random_number, 5, &ledsTask);
    say_text(text);
    vTaskDelete(ledsTask);
    turn_off_leds();
    while (servo_pos != 90)
    {
        delay(10);
    }
    vTaskDelete(servoTask);
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
    Serial.begin(115200);

    uint32_t random_number = generate_random_number();

    init_leds();
    init_servo();
    init_storage();
    init_llm(random_number);
    init_wifi();
    init_motors();

    generate_text(random_number);

    while (true)
    {

        while (!connection_status) {
            connection_status = connect();
        }

        response_t response = {0, ' '};

        bool data_received = get_response(&response);
        if (!data_received) {
            continue;
        }

        switch (response.code) {
            case 'F':
                Serial.println("Forward");
                run_motors(0);
                break;
            case 'B':
                Serial.println("Backward");
                run_motors(1);
                break;
            case 'R':
                Serial.println("Rotate Right");
                run_motors(2);
                break;
            case 'L':
                Serial.println("Rotate Left");
                run_motors(3);
                break;
            case 'S':
                Serial.println("Stop");
                run_motors(4);
                break;
            case 'W':
                Serial.println("Speak");
                init_audio();
                say_with_animation(random_number);
                deinit_audio();
                random_number = generate_random_number();
                generate_text(random_number);
                break;
            case 'M':
                Serial.println("LEDs on");
                run_leds(nullptr);
                break;
            case 'm':
                Serial.println("LEDs off");
                turn_off_leds();
                break;
            case 'J':
                rotate_servo(response.number);
                break;
            case '\n':
            case '\r':
            case '.':
            case ' ':
                break;
            default:
                Serial.println("Invalid command");
                break;
        }
        delay(10);
    }
}