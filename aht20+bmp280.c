// Generated with help of ChatGPT to support BMP280 and AHT20 on I2C.
// Corrected manually to 40 bits and work fine fot both DHT11 and DHT22.
// Output is in line protocol format to use in influxdata telegraf.
// https://docs.influxdata.com/influxdb/cloud/reference/syntax/line-protocol/
// Program for Raspberry Pi board.
// Compiling: gcc aht20+bmp280.c -o aht20+bmp280 -l wiringPi

#include <stdio.h>
#include <wiringPi.h>
#include <wiringPiI2C.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// I2C addresses
#define AHT20_ADDR 0x38
#define BMP280_ADDR 0x77

// AHT20 Commands
#define AHT20_INIT_CMD 0xE1
#define AHT20_READ_CMD 0xAC
#define AHT20_STATUS_BUSY 0x80

// BMP280 Registers
#define BMP280_TEMP_PRESS_CALIB 0x88
#define BMP280_TEMP_DATA 0xFA
#define BMP280_PRESSURE_DATA 0xF7
#define BMP280_CONTROL 0xF4

// Output variables
char hostbuffer[256];
char sensor_type_name[8] = "unknown"; // Initialize to a default value

// Retry limit
int maxRetries = 7;
int retries = 0;

// Handlers
int bmp280_fd, aht20_fd;

// BMP280 Calibration Data
uint16_t dig_T1;
int16_t dig_T2, dig_T3;
// BMP280 Pressure Calibration Data
uint16_t dig_P1;
int16_t dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;

// Function to initialize BMP280 sensor and read calibration data
void initBMP280(int fd)
{
    uint8_t calib[24];
    for (int i = 0; i < 24; i++)
    {
        calib[i] = wiringPiI2CReadReg8(fd, BMP280_TEMP_PRESS_CALIB + i);
    }

    // Extract calibration data
    dig_T1 = (calib[1] << 8) | calib[0];
    dig_T2 = (calib[3] << 8) | calib[2];
    dig_T3 = (calib[5] << 8) | calib[4];

    // Extract pressure calibration data
    dig_P1 = (calib[7] << 8) | calib[6];
    dig_P2 = (calib[9] << 8) | calib[8];
    dig_P3 = (calib[11] << 8) | calib[10];
    dig_P4 = (calib[13] << 8) | calib[12];
    dig_P5 = (calib[15] << 8) | calib[14];
    dig_P6 = (calib[17] << 8) | calib[16];
    dig_P7 = (calib[19] << 8) | calib[18];
    dig_P8 = (calib[21] << 8) | calib[20];
    dig_P9 = (calib[23] << 8) | calib[22];

    // Configure BMP280 for temperature and pressure
    wiringPiI2CWriteReg8(fd, BMP280_CONTROL, 0x3F); // Normal mode, oversampling x4
}

void resetAHT20(int fd)
{
    wiringPiI2CWrite(fd, 0xBA); // Soft reset command
    usleep(20000);              // Wait 20ms for the reset to complete
    // printf("AHT20: Sensor reset completed.\n");
}

void initAHT20(int fd)
{
    uint8_t initCommand[] = {AHT20_INIT_CMD, 0x08, 0x00};

    // Send initialization command (0xE1)
    wiringPiI2CWrite(fd, AHT20_INIT_CMD);
    usleep(50000); // Wait 50ms for initialization

    // Check if the sensor is calibrated (Status bit 3 should be set)
    uint8_t status = wiringPiI2CRead(fd);
    if (!(status & 0x08))
    {
        // printf("AHT20: Sensor not calibrated. Initialization failed.\n");
        return;
    }

    // printf("AHT20: Sensor initialized and calibrated successfully.\n");
}

// Function to read AHT20 sensor
void readAHT20(int fd)
{
    uint8_t data[6] = {0};

    // Send the measurement command (0xAC)
    wiringPiI2CWriteReg8(fd, AHT20_READ_CMD, 0x33);
    wiringPiI2CWriteReg8(fd, 0x00, 0x00); // Dummy data for AHT20 command
    usleep(100000);                       // Wait 80ms for the measurement to complete

    // Check status byte to ensure data is ready
    uint8_t status = wiringPiI2CRead(fd);
    if (status & AHT20_STATUS_BUSY)
    {
        // printf("AHT20 is busy\n");
        return;
    }

    // Read 6 bytes of data
    for (int i = 0; i < 6; i++)
    {
        data[i] = wiringPiI2CRead(fd);
    }

    // Debug raw values
    // printf("Raw Data: %02X %02X %02X %02X %02X %02X\n", data[0], data[1], data[2], data[3], data[4], data[5]);

    // Extract and calculate values as before
    uint32_t raw_humidity = ((data[1] << 12) | (data[2] << 4) | (data[3] >> 4));
    uint32_t raw_temperature = (((data[3] & 0x0F) << 16) | (data[4] << 8) | data[5]);
    // Debug raw Humidity and Temperature
    // printf("Raw Humidity: %d\n", raw_humidity);
    // printf("Raw Temperature: %d\n", raw_temperature);

    float humidity = (raw_humidity * 100.0) / 1048576.0;                // Convert to percentage
    float temperature = ((raw_temperature * 200.0) / 1048576.0) - 50.0; // Convert to Celsius

    if ((temperature >= -40 && temperature <= 100 && humidity >= 0 && humidity <= 100))
    {
        gethostname(hostbuffer, sizeof(hostbuffer));
        printf("Weather,host=%s,sensor_type_name=%s humidity=%.2f,temperature=%.2f\n", hostbuffer, sensor_type_name, humidity, temperature);
    }
    else
    {
        // Retry if values are out of range
        if (retries < maxRetries)
        {
            retries++;
            delay(3000);
            initAHT20(aht20_fd);
            readAHT20(aht20_fd);
        }
    }
}

// Function to read BMP280 sensor
void readBMP280(int fd)
{
    uint8_t tempData[3], pressureData[3];

    // Read temperature data
    for (int i = 0; i < 3; i++)
    {
        tempData[i] = wiringPiI2CReadReg8(fd, BMP280_TEMP_DATA + i);
    }

    // Read pressure data
    for (int i = 0; i < 3; i++)
    {
        pressureData[i] = wiringPiI2CReadReg8(fd, BMP280_PRESSURE_DATA + i);
    }

    // Combine raw data
    int32_t adc_T = (tempData[0] << 12) | (tempData[1] << 4) | (tempData[2] >> 4);
    int32_t adc_P = (pressureData[0] << 12) | (pressureData[1] << 4) | (pressureData[2] >> 4);

    // Compensate temperature using calibration
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)dig_T1 << 1))) * ((int32_t)dig_T2)) >> 11;
    int32_t var2 = (((((adc_T >> 4) - ((int32_t)dig_T1)) * ((adc_T >> 4) - ((int32_t)dig_T1))) >> 12) *
                    ((int32_t)dig_T3)) >>
                   14;
    int32_t t_fine = var1 + var2;
    float temperature = (t_fine * 5 + 128) >> 8;

    // Pressure compensation
    var1 = (((int32_t)t_fine) >> 1) - (int32_t)64000;
    var2 = (((var1 >> 2) * (var1 >> 2)) >> 11) * ((int32_t)dig_P6);
    var2 = var2 + ((var1 * ((int32_t)dig_P5)) << 1);
    var2 = (var2 >> 2) + (((int32_t)dig_P4) << 16);
    var1 = (((dig_P3 * (((var1 >> 2) * (var1 >> 2)) >> 13)) >> 3) + ((((int32_t)dig_P2) * var1) >> 1)) >> 18;
    var1 = ((((32768 + var1)) * ((int32_t)dig_P1)) >> 15);
    if (var1 == 0)
    {
        // printf("BMP280 - Invalid pressure data\n");
        return; // avoid division by zero
    }
    int32_t pressure = (((uint32_t)(((int32_t)1048576) - adc_P) - (var2 >> 12))) * 3125;
    if (pressure < 0x80000000)
    {
        pressure = (pressure << 1) / ((uint32_t)var1);
    }
    else
    {
        pressure = (pressure / (uint32_t)var1) * 2;
    }
    var1 = (((int32_t)dig_P9) * ((int32_t)(((pressure >> 3) * (pressure >> 3)) >> 13))) >> 12;
    var2 = (((int32_t)(pressure >> 2)) * ((int32_t)dig_P8)) >> 13;
    pressure = (int32_t)((int32_t)pressure + ((var1 + var2 + dig_P7) >> 4));
    pressure = pressure / 100.0;
    temperature = temperature / 100.0;
    if ((temperature >= -40 && temperature <= 100 && pressure >= 300 && pressure <= 1300))
    {
        gethostname(hostbuffer, sizeof(hostbuffer));
        printf("Weather,host=%s,sensor_type_name=%s pressure=%d,temperature=%.1f\n", hostbuffer, sensor_type_name, pressure, temperature);
    }
    else
    {
        // Retry if values are out of range
        if (retries < maxRetries)
        {
            retries++;
            delay(3000);
            initBMP280(bmp280_fd);
            readBMP280(bmp280_fd);
        }
    }
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s -sensor <bmp280|aht20>\n", argv[0]);
        exit(1);
    }

    int sensor_type = 0; // Default to 0 (invalid)

    // Parse the arguments
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-sensor") == 0)
        {
            i++;
            if (strcmp(argv[i], "bmp280") == 0)
            {
                sensor_type = 280;
                strcpy(sensor_type_name, "bmp280");
            }
            else if (strcmp(argv[i], "aht20") == 0)
            {
                sensor_type = 20;
                strcpy(sensor_type_name, "aht20");
            }
            else
            {
                fprintf(stderr, "Invalid sensor type. Use bmp280 or aht20.\n");
                exit(1);
            }
        }
        else
        {
            fprintf(stderr, "Invalid argument: %s\n", argv[i]);
            exit(1);
        }
    }

    if (sensor_type == 0)
    {
        fprintf(stderr, "Usage: %s -sensor <bmp280|aht20>\n", argv[0]);
        exit(1);
    }
    if (wiringPiSetup() == -1)
    {
        exit(1);
    }

    if (sensor_type == 280)
    { // BMP280

        strcpy(sensor_type_name, "bmp280");
        bmp280_fd = wiringPiI2CSetup(BMP280_ADDR);
        if (bmp280_fd == -1)
            return 1;
        initBMP280(bmp280_fd);
        readBMP280(bmp280_fd);
    }
    else if (sensor_type == 20)
    { // AHT20
        strcpy(sensor_type_name, "aht20");
        aht20_fd = wiringPiI2CSetup(AHT20_ADDR);
        if (aht20_fd == -1)
            return 1;
        initAHT20(aht20_fd);
        readAHT20(aht20_fd);
    }

    return 0;
}
