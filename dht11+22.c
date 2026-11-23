#define _DEFAULT_SOURCE

/*
 * DHT11 / DHT22 reader for Raspberry Pi using WiringPi.
 *
 * Output: InfluxDB Line Protocol
 * Build:  gcc dht11+22.c -o dht11+22 -lwiringPi
 *
 * Usage:
 *   ./dht11+22 -dhtpin <wiringPi_pin> -sensor <dht11|dht22>
 *
 */

#include <wiringPi.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_ATTEMPTS 7
#define RETRY_DELAY_MS 2000

/*
 * DHT timings are nominally:
 *   response: LOW ~80 us, HIGH ~80 us
 *   data bit: LOW ~50 us, then HIGH ~26-28 us (0) or ~70 us (1)
 *
 * Linux is not a real-time operating system, so the limits deliberately
 * allow some scheduling/polling jitter. micros() is a 32-bit counter;
 * unsigned subtraction also handles its wraparound.
 */
#define RESPONSE_TIMEOUT_US 150U
#define DATA_TIMEOUT_US 150U
#define RESPONSE_MIN_US 40U
#define RESPONSE_MAX_US 120U
#define BIT_LOW_MIN_US 15U
#define BIT_LOW_MAX_US 100U
#define BIT_HIGH_MIN_US 10U
#define BIT_HIGH_MAX_US 120U
#define BIT_THRESHOLD_US is_not_used

static const char *sensor_type_name(int sensor_type)
{
    return sensor_type == 11 ? "dht11" : "dht22";
}

/*
 * Wait until the pin reaches 'level'.
 * On success, optionally return the elapsed time in microseconds.
 * Returns 0 on success and -1 on timeout.
 */
static int wait_for_level(int pin, int level, uint32_t timeout_us,
                          uint32_t *elapsed_us)
{
    uint32_t start = micros();

    while (digitalRead(pin) != level)
    {
        if ((uint32_t)(micros() - start) >= timeout_us)
            return -1;
    }

    if (elapsed_us != NULL)
        *elapsed_us = (uint32_t)(micros() - start);

    return 0;
}

/* Read one complete 40-bit frame into data[]. */
static int read_dht_frame(int pin, uint8_t data[5])
{
    uint32_t duration;
    unsigned int bit;

    memset(data, 0, 5);

    /* Send the host start signal. */
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    delay(18);
    digitalWrite(pin, HIGH);
    delayMicroseconds(30);

    /* Release the bus and wait for the sensor's response. */
    pinMode(pin, INPUT);

    if (wait_for_level(pin, LOW, RESPONSE_TIMEOUT_US, NULL) != 0)
        return -1;

    if (wait_for_level(pin, HIGH, RESPONSE_TIMEOUT_US, &duration) != 0)
        return -1;
    if (duration < RESPONSE_MIN_US || duration > RESPONSE_MAX_US)
        return -1;

    if (wait_for_level(pin, LOW, RESPONSE_TIMEOUT_US, &duration) != 0)
        return -1;
    if (duration < RESPONSE_MIN_US || duration > RESPONSE_MAX_US)
        return -1;

    /* The response HIGH has ended; the data stream starts LOW. */
    for (bit = 0; bit < 40; ++bit)
    {
        /* Measure the LOW part of this data bit. */
        if (wait_for_level(pin, HIGH, DATA_TIMEOUT_US, &duration) != 0)
            return -1;
        if (duration < BIT_LOW_MIN_US || duration > BIT_LOW_MAX_US)
            return -1;

        /* Measure the HIGH part; its duration encodes the bit value. */
        if (wait_for_level(pin, LOW, DATA_TIMEOUT_US, &duration) != 0)
            return -1;
        if (duration < BIT_HIGH_MIN_US || duration > BIT_HIGH_MAX_US)
            return -1;

        data[bit / 8] <<= 1;
        if (duration > 40U)
            data[bit / 8] |= 1U;
    }

    return 0;
}

static int decode_reading(const uint8_t data[5], int sensor_type,
                          float *temperature, float *humidity)
{
    uint8_t checksum =
        (uint8_t)(data[0] + data[1] + data[2] + data[3]);

    if (checksum != data[4])
        return -1;

    if (sensor_type == 11)
    {
        /*
         * DHT11: integer and decimal bytes for humidity and temperature.
         * The sign bit convention is supported by some DHT11 variants.
         */
        *humidity = (float)data[0] + (float)data[1] / 10.0f;

        if ((data[2] & 0x80U) != 0U)
            *temperature = -((float)(data[2] & 0x7FU) +
                             (float)data[3] / 10.0f);
        else
            *temperature = (float)data[2] + (float)data[3] / 10.0f;

        if (*temperature < 0.0f || *temperature > 80.0f ||
            *humidity < 0.0f || *humidity > 100.0f)
            return -1;
    }
    else if (sensor_type == 22)
    {
        uint16_t raw_humidity =
            (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
        uint16_t raw_temperature =
            (uint16_t)(((uint16_t)data[2] << 8) | data[3]);

        *humidity = (float)raw_humidity / 10.0f;

        if ((raw_temperature & 0x8000U) != 0U)
        {
            raw_temperature &= 0x7FFFU;
            *temperature = -(float)raw_temperature / 10.0f;
        }
        else
        {
            *temperature = (float)raw_temperature / 10.0f;
        }

        if (*temperature < -40.0f || *temperature > 80.0f ||
            *humidity < 0.0f || *humidity > 100.0f)
            return -1;
    }
    else
    {
        return -1;
    }

    return 0;
}

static int read_dht(int pin, int sensor_type,
                    float *temperature, float *humidity)
{
    uint8_t data[5];
    int attempt;

    for (attempt = 1; attempt <= MAX_ATTEMPTS; ++attempt)
    {
        if (read_dht_frame(pin, data) == 0 &&
            decode_reading(data, sensor_type, temperature, humidity) == 0)
        {
            return 0;
        }

        if (attempt < MAX_ATTEMPTS)
            delay(RETRY_DELAY_MS);
    }

    return -1;
}

static int parse_pin(const char *text, int *pin)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0' ||
        value < 0 || value > 31)
        return -1;

    *pin = (int)value;
    return 0;
}

int main(int argc, char *argv[])
{
    int pin = -1;
    int sensor_type = 0;
    char hostname[256];
    float temperature;
    float humidity;
    int i;

    if (argc != 5)
    {
        fprintf(stderr,
                "Usage: %s -dhtpin <pin_number> -sensor <dht11|dht22>\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "-dhtpin") == 0)
        {
            if (i + 1 >= argc || parse_pin(argv[++i], &pin) != 0)
            {
                fprintf(stderr, "Invalid WiringPi pin number (expected 0-31).\n");
                return EXIT_FAILURE;
            }
        }
        else if (strcmp(argv[i], "-sensor") == 0)
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "Missing sensor type.\n");
                return EXIT_FAILURE;
            }

            ++i;
            if (strcmp(argv[i], "dht11") == 0)
                sensor_type = 11;
            else if (strcmp(argv[i], "dht22") == 0)
                sensor_type = 22;
            else
            {
                fprintf(stderr, "Invalid sensor type. Use dht11 or dht22.\n");
                return EXIT_FAILURE;
            }
        }
        else
        {
            fprintf(stderr, "Invalid argument: %s\n", argv[i]);
            return EXIT_FAILURE;
        }
    }

    if (pin < 0 || sensor_type == 0)
    {
        fprintf(stderr,
                "Usage: %s -dhtpin <pin_number> -sensor <dht11|dht22>\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    if (wiringPiSetup() == -1)
    {
        fprintf(stderr, "wiringPiSetup() failed.\n");
        return EXIT_FAILURE;
    }

    if (read_dht(pin, sensor_type, &temperature, &humidity) != 0)
    {
        fprintf(stderr, "Failed to read %s on WiringPi pin %d after %d attempts.\n",
                sensor_type_name(sensor_type), pin, MAX_ATTEMPTS);
        return EXIT_FAILURE;
    }

    if (gethostname(hostname, sizeof(hostname)) != 0)
    {
        perror("gethostname");
        return EXIT_FAILURE;
    }
    hostname[sizeof(hostname) - 1] = '\0';

    printf("Weather,host=%s,pinnum=%d,sensor_type_name=%s "
           "humidity=%.1f,temperature=%.1f\n",
           hostname, pin, sensor_type_name(sensor_type),
           humidity, temperature);

    return EXIT_SUCCESS;
}