/*
 * AHT20 + BMP280 reader for Linux I2C.
 *
 * No WiringPi dependency: uses the Linux i2c-dev interface.
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -std=c11 aht20+bmp280.c -o aht20+bmp280 -lm
 *
 * Examples:
 *   ./aht20-bmp280 -sensor aht20 -bus 1
 *   ./aht20-bmp280 -sensor bmp280 -bus 0
 *
 * Output: InfluxDB Line Protocol, one measurement per invocation.
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DEFAULT_I2C_BUS 0
#define MAX_ATTEMPTS 3
#define RETRY_DELAY_US 200000

#define AHT20_ADDR 0x38
#define BMP280_ADDR_LOW 0x76
#define BMP280_ADDR_HIGH 0x77

static char hostname[256] = "unknown";

/* ---------- Common I2C helpers ---------- */

static int select_i2c_device(int fd, int address)
{
    if (ioctl(fd, I2C_SLAVE, address) < 0)
    {
        return -1;
    }
    return 0;
}

static int i2c_write_bytes(int fd, const uint8_t *data, size_t length)
{
    ssize_t written = write(fd, data, length);
    if (written < 0 || (size_t)written != length)
    {
        return -1;
    }
    return 0;
}

static int i2c_read_bytes(int fd, uint8_t *data, size_t length)
{
    ssize_t received = read(fd, data, length);
    if (received < 0 || (size_t)received != length)
    {
        return -1;
    }
    return 0;
}

static int i2c_write_register(int fd, uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    return i2c_write_bytes(fd, data, sizeof(data));
}

static int i2c_read_registers(int fd, uint8_t reg,
                              uint8_t *data, size_t length)
{
    if (i2c_write_bytes(fd, &reg, 1) < 0)
    {
        return -1;
    }
    return i2c_read_bytes(fd, data, length);
}

static void print_i2c_error(const char *operation)
{
    fprintf(stderr, "ERROR: %s: %s\n", operation, strerror(errno));
}

/* ---------- AHT20 ---------- */

static int aht20_initialize(int fd)
{
    uint8_t status;
    const uint8_t init_command[3] = {0xBE, 0x08, 0x00};

    usleep(40000);

    if (i2c_read_bytes(fd, &status, 1) < 0)
    {
        print_i2c_error("AHT20 status read");
        return -1;
    }

    /* Status bit 3 indicates that calibration has been performed. */
    if ((status & 0x08) == 0)
    {
        if (i2c_write_bytes(fd, init_command, sizeof(init_command)) < 0)
        {
            print_i2c_error("AHT20 initialization command");
            return -1;
        }
        usleep(10000);

        if (i2c_read_bytes(fd, &status, 1) < 0)
        {
            print_i2c_error("AHT20 status read after initialization");
            return -1;
        }

        if ((status & 0x08) == 0)
        {
            fprintf(stderr, "ERROR: AHT20 did not become calibrated\n");
            return -1;
        }
    }

    return 0;
}

static int aht20_read(int fd, double *temperature, double *humidity)
{
    const uint8_t measure_command[3] = {0xAC, 0x33, 0x00};
    uint8_t data[7];
    uint32_t raw_humidity;
    uint32_t raw_temperature;

    /* AHT20 requires the complete three-byte command in one transaction. */
    if (i2c_write_bytes(fd, measure_command, sizeof(measure_command)) < 0)
    {
        print_i2c_error("AHT20 measurement command");
        return -1;
    }

    /* The measurement normally takes about 80 ms. */
    usleep(80000);

    /* Read the complete frame: status + 5 data bytes + CRC byte. */
    if (i2c_read_bytes(fd, data, sizeof(data)) < 0)
    {
        print_i2c_error("AHT20 data read");
        return -1;
    }

    if (data[0] & 0x80)
    {
        fprintf(stderr, "WARNING: AHT20 is still busy\n");
        return -1;
    }

    raw_humidity =
        ((uint32_t)data[1] << 12) |
        ((uint32_t)data[2] << 4) |
        ((uint32_t)data[3] >> 4);

    raw_temperature =
        (((uint32_t)data[3] & 0x0F) << 16) |
        ((uint32_t)data[4] << 8) |
        (uint32_t)data[5];

    *humidity = (double)raw_humidity * 100.0 / 1048576.0;
    *temperature = (double)raw_temperature * 200.0 / 1048576.0 - 50.0;

    if (!isfinite(*temperature) || !isfinite(*humidity) ||
        *temperature < -40.0 || *temperature > 85.0 ||
        *humidity < 0.0 || *humidity > 100.0)
    {
        fprintf(stderr, "WARNING: AHT20 returned out-of-range data\n");
        return -1;
    }

    return 0;
}

/* ---------- BMP280 ---------- */

struct bmp280_calibration
{
    uint16_t dig_T1;
    int16_t dig_T2, dig_T3;
    uint16_t dig_P1;
    int16_t dig_P2, dig_P3, dig_P4, dig_P5;
    int16_t dig_P6, dig_P7, dig_P8, dig_P9;
};

static uint16_t read_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static int16_t read_s16_le(const uint8_t *data)
{
    return (int16_t)read_u16_le(data);
}

static int bmp280_initialize(int fd, struct bmp280_calibration *cal)
{
    uint8_t chip_id;
    uint8_t raw[24];

    if (i2c_read_registers(fd, 0xD0, &chip_id, 1) < 0)
    {
        print_i2c_error("BMP280 chip ID read");
        return -1;
    }

    if (chip_id != 0x58)
    {
        fprintf(stderr, "ERROR: device at BMP280 address has chip ID 0x%02X, expected 0x58\n",
                chip_id);
        return -1;
    }

    if (i2c_read_registers(fd, 0x88, raw, sizeof(raw)) < 0)
    {
        print_i2c_error("BMP280 calibration read");
        return -1;
    }

    cal->dig_T1 = read_u16_le(&raw[0]);
    cal->dig_T2 = read_s16_le(&raw[2]);
    cal->dig_T3 = read_s16_le(&raw[4]);
    cal->dig_P1 = read_u16_le(&raw[6]);
    cal->dig_P2 = read_s16_le(&raw[8]);
    cal->dig_P3 = read_s16_le(&raw[10]);
    cal->dig_P4 = read_s16_le(&raw[12]);
    cal->dig_P5 = read_s16_le(&raw[14]);
    cal->dig_P6 = read_s16_le(&raw[16]);
    cal->dig_P7 = read_s16_le(&raw[18]);
    cal->dig_P8 = read_s16_le(&raw[20]);
    cal->dig_P9 = read_s16_le(&raw[22]);

    if (cal->dig_P1 == 0)
    {
        fprintf(stderr, "ERROR: BMP280 calibration data is invalid\n");
        return -1;
    }

    /*
     * Configure while in sleep mode:
     * config = 0 (filter off, standby setting irrelevant in forced mode)
     * ctrl_meas = 0x25 (temperature x1, pressure x1, forced mode)
     * A fresh conversion is triggered for each read.
     */
    if (i2c_write_register(fd, 0xF4, 0x00) < 0 ||
        i2c_write_register(fd, 0xF5, 0x00) < 0)
    {
        print_i2c_error("BMP280 configuration");
        return -1;
    }

    return 0;
}

static int bmp280_read(int fd, const struct bmp280_calibration *c,
                       double *temperature, double *pressure_hpa)
{
    uint8_t raw[6];
    int32_t adc_p, adc_t;
    double var1, var2, t_fine, p;

    /* Forced mode: temperature x1, pressure x1. */
    if (i2c_write_register(fd, 0xF4, 0x25) < 0)
    {
        print_i2c_error("BMP280 measurement trigger");
        return -1;
    }

    /* x1 oversampling conversion takes less than 10 ms; allow margin. */
    usleep(20000);

    if (i2c_read_registers(fd, 0xF7, raw, sizeof(raw)) < 0)
    {
        print_i2c_error("BMP280 measurement read");
        return -1;
    }

    adc_p = ((int32_t)raw[0] << 12) |
            ((int32_t)raw[1] << 4) |
            ((int32_t)raw[2] >> 4);

    adc_t = ((int32_t)raw[3] << 12) |
            ((int32_t)raw[4] << 4) |
            ((int32_t)raw[5] >> 4);

    if (adc_t == 0x80000 || adc_p == 0x80000)
    {
        fprintf(stderr, "WARNING: BMP280 measurement is unavailable\n");
        return -1;
    }

    /* Bosch compensation formula, using double precision. */
    var1 = ((double)adc_t / 16384.0 - (double)c->dig_T1 / 1024.0) * (double)c->dig_T2;
    var2 = (((double)adc_t / 131072.0 - (double)c->dig_T1 / 8192.0) *
            ((double)adc_t / 131072.0 - (double)c->dig_T1 / 8192.0)) *
           (double)c->dig_T3;

    t_fine = var1 + var2;
    *temperature = t_fine / 5120.0;

    var1 = t_fine / 2.0 - 64000.0;
    var2 = var1 * var1 * (double)c->dig_P6 / 32768.0;
    var2 += var1 * (double)c->dig_P5 * 2.0;
    var2 = var2 / 4.0 + (double)c->dig_P4 * 65536.0;

    var1 = ((double)c->dig_P3 * var1 * var1 / 524288.0 +
            (double)c->dig_P2 * var1) /
           524288.0;
    var1 = (1.0 + var1 / 32768.0) * (double)c->dig_P1;

    if (var1 == 0.0)
    {
        fprintf(stderr, "WARNING: BMP280 pressure compensation failed\n");
        return -1;
    }

    p = (1048576.0 - (double)adc_p - var2 / 4096.0) * 6250.0 / var1;

    var1 = (double)c->dig_P9 * p * p / 2147483648.0;
    var2 = p * (double)c->dig_P8 / 32768.0;
    p += (var1 + var2 + (double)c->dig_P7) / 16.0;

    *pressure_hpa = p / 100.0;

    if (!isfinite(*temperature) || !isfinite(*pressure_hpa) ||
        *temperature < -40.0 || *temperature > 85.0 ||
        *pressure_hpa < 300.0 || *pressure_hpa > 1200.0)
    {
        fprintf(stderr, "WARNING: BMP280 returned out-of-range data\n");
        return -1;
    }

    return 0;
}

/* ---------- Output and main ---------- */

static void print_usage(const char *program)
{
    fprintf(stderr, "Usage: %s -sensor <aht20|bmp280> [-bus N]\n", program);
}

int main(int argc, char **argv)
{
    const char *sensor = NULL;
    int bus_number = DEFAULT_I2C_BUS;
    int fd = -1;
    int address = 0;
    char device_path[64];
    int i;

    if (gethostname(hostname, sizeof(hostname) - 1) < 0)
    {
        perror("gethostname");
        return 1;
    }
    hostname[sizeof(hostname) - 1] = '\0';

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-sensor") == 0 && i + 1 < argc)
        {
            sensor = argv[++i];
        }
        else if (strcmp(argv[i], "-bus") == 0 && i + 1 < argc)
        {
            char *end = NULL;
            long value = strtol(argv[++i], &end, 10);
            if (*end != '\0' || value < 0 || value > 255)
            {
                fprintf(stderr, "ERROR: invalid I2C bus number\n");
                return 2;
            }
            bus_number = (int)value;
        }
        else
        {
            print_usage(argv[0]);
            return 2;
        }
    }

    if (sensor == NULL ||
        (strcmp(sensor, "aht20") != 0 && strcmp(sensor, "bmp280") != 0))
    {
        print_usage(argv[0]);
        return 2;
    }

    if (strcmp(sensor, "aht20") == 0)
    {
        address = AHT20_ADDR;
    }
    else
    {
        /* Try both common BMP280 addresses below. */
        address = BMP280_ADDR_HIGH;
    }

    snprintf(device_path, sizeof(device_path), "/dev/i2c-%d", bus_number);
    fd = open(device_path, O_RDWR);
    if (fd < 0)
    {
        print_i2c_error(device_path);
        return 1;
    }

    if (strcmp(sensor, "aht20") == 0)
    {
        double temperature = 0.0, humidity = 0.0;
        int success = 0;
        int attempt;

        if (select_i2c_device(fd, address) < 0)
        {
            print_i2c_error("select AHT20");
            close(fd);
            return 1;
        }

        for (attempt = 1; attempt <= MAX_ATTEMPTS; attempt++)
        {
            if (aht20_initialize(fd) == 0 &&
                aht20_read(fd, &temperature, &humidity) == 0)
            {
                success = 1;
                break;
            }
            if (attempt < MAX_ATTEMPTS)
                usleep(RETRY_DELAY_US);
        }

        if (success)
        {
            printf("Weather,host=%s,sensor_type_name=aht20 humidity=%.2f,temperature=%.2f\n",
                   hostname, humidity, temperature);
        }
        else
        {
            fprintf(stderr, "ERROR: AHT20 failed after %d attempts\n",
                    MAX_ATTEMPTS);
        }

        close(fd);
        return success ? 0 : 1;
    }

    /* BMP280: probe address 0x77 first, then 0x76. */
    {
        struct bmp280_calibration cal;
        double temperature = 0.0, pressure = 0.0;
        int success = 0;
        int attempt;
        int addresses[2] = {BMP280_ADDR_HIGH, BMP280_ADDR_LOW};
        int found = 0;
        int j;

        for (j = 0; j < 2; j++)
        {
            if (select_i2c_device(fd, addresses[j]) == 0)
            {
                uint8_t chip_id;
                if (i2c_read_registers(fd, 0xD0, &chip_id, 1) == 0 &&
                    chip_id == 0x58)
                {
                    address = addresses[j];
                    found = 1;
                    break;
                }
            }
        }

        if (!found)
        {
            fprintf(stderr, "ERROR: BMP280 not found at 0x76 or 0x77\n");
            close(fd);
            return 1;
        }

        if (select_i2c_device(fd, address) < 0)
        {
            print_i2c_error("select BMP280");
            close(fd);
            return 1;
        }

        for (attempt = 1; attempt <= MAX_ATTEMPTS; attempt++)
        {
            if (bmp280_initialize(fd, &cal) == 0 &&
                bmp280_read(fd, &cal, &temperature, &pressure) == 0)
            {
                success = 1;
                break;
            }
            if (attempt < MAX_ATTEMPTS)
                usleep(RETRY_DELAY_US);
        }

        if (success)
        {
            printf("Weather,host=%s,sensor_type_name=bmp280 pressure=%.2f,temperature=%.2f\n",
                   hostname, pressure, temperature);
        }
        else
        {
            fprintf(stderr, "ERROR: BMP280 failed after %d attempts\n",
                    MAX_ATTEMPTS);
        }

        close(fd);
        return success ? 0 : 1;
    }
}
