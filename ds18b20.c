// DS18B20 program
// Output is in line protocol format to use in influxdata telegraf.
// https://docs.influxdata.com/influxdb/cloud/reference/syntax/line-protocol/
// Program for Raspberry Pi board.
// Compiling: gcc ds18b20.c -o ds18b20

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#define MAX_PATH 256
#define MAX_RETRIES 7
#define RETRY_DELAY 3

char hostbuffer[256];

// Function to find DS18B20 sensor by serial number or get first available
int find_sensor(char *device_path, const char *serial)
{
    DIR *dir;
    struct dirent *entry;
    char base_path[] = "/sys/bus/w1/devices/";
    int found = 0;

    dir = opendir(base_path);
    if (dir == NULL)
    {
        fprintf(stderr, "Error: Cannot open %s. Make sure w1-gpio and w1-therm modules are loaded.\n", base_path);
        fprintf(stderr, "Run: sudo modprobe w1-gpio && sudo modprobe w1-therm\n");
        return 0;
    }

    while ((entry = readdir(dir)) != NULL)
    {
        // DS18B20 devices start with "28-"
        if (strncmp(entry->d_name, "28-", 3) == 0)
        {
            if (serial == NULL || strcmp(entry->d_name, serial) == 0)
            {
                snprintf(device_path, MAX_PATH, "%s%s/w1_slave", base_path, entry->d_name);
                found = 1;
                break;
            }
        }
    }

    closedir(dir);
    return found;
}

// Function to read temperature from DS18B20
int read_ds18b20(const char *device_path, float *temperature, int retries)
{
    FILE *fp;
    char line[256];
    char *temp_str;
    int temp_raw;
    int crc_ok = 0;

    fp = fopen(device_path, "r");
    if (fp == NULL)
    {
        if (retries < MAX_RETRIES)
        {
            sleep(RETRY_DELAY);
            return read_ds18b20(device_path, temperature, retries + 1);
        }
        fprintf(stderr, "Error: Cannot open device file %s\n", device_path);
        return 0;
    }

    // Read first line to check CRC
    if (fgets(line, sizeof(line), fp) != NULL)
    {
        if (strstr(line, "YES") != NULL)
        {
            crc_ok = 1;
        }
    }

    // Read second line to get temperature
    if (crc_ok && fgets(line, sizeof(line), fp) != NULL)
    {
        temp_str = strstr(line, "t=");
        if (temp_str != NULL)
        {
            temp_str += 2; // Skip "t="
            temp_raw = atoi(temp_str);
            *temperature = temp_raw / 1000.0;

            fclose(fp);

            // Validate temperature range (-55°C to 125°C for DS18B20)
            if (*temperature >= -55.0 && *temperature <= 125.0)
            {
                return 1;
            }
            else
            {
                // Retry if temperature is out of valid range
                if (retries < MAX_RETRIES)
                {
                    sleep(RETRY_DELAY);
                    return read_ds18b20(device_path, temperature, retries + 1);
                }
                fprintf(stderr, "Error: Temperature out of valid range: %.1f\n", *temperature);
                return 0;
            }
        }
    }

    fclose(fp);

    // Retry if CRC check failed or temperature not found
    if (retries < MAX_RETRIES)
    {
        sleep(RETRY_DELAY);
        return read_ds18b20(device_path, temperature, retries + 1);
    }

    fprintf(stderr, "Error: Failed to read valid temperature after %d retries\n", MAX_RETRIES);
    return 0;
}

int main(int argc, char *argv[])
{
    char device_path[MAX_PATH];
    float temperature;
    const char *serial = NULL;
    int pin_num = -1; // no default, must be provided via -pin

    // If no arguments provided, show error and help
    if (argc == 1)
    {
        fprintf(stderr, "Error: argument is required.\n");
        fprintf(stderr, "Usage: %s -pin <gpio_pin> [-serial <28-xxxx>]\n", argv[0]);
        fprintf(stderr, "  -pin: GPIO pin number (required)\n");
        fprintf(stderr, "  -serial: Specific DS18B20 serial number (optional, e.g., 28-0123456789ab)\n");
        fprintf(stderr, "\nMake sure the following modules are loaded:\n");
        fprintf(stderr, "  sudo modprobe w1-gpio\n");
        fprintf(stderr, "  sudo modprobe w1-therm\n");
        fprintf(stderr, "For Raspberry Pi you can add dtoverlay=w1-gpio,gpiopin=<gpio_pin> in config.txt\n");
        return 1;
    }

    // Parse arguments
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-serial") == 0 && i + 1 < argc)
        {
            serial = argv[++i];
        }
        else if (strcmp(argv[i], "-pin") == 0 && i + 1 < argc)
        {
            pin_num = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            printf("Usage: %s -pin <gpio_pin> [-serial <28-xxxx>]\n", argv[0]);
            printf("  -pin: GPIO pin number (required)\n");
            printf("  -serial: Specific DS18B20 serial number (optional, e.g., 28-0123456789ab)\n");
            printf("\nMake sure the following modules are loaded:\n");
            printf("  sudo modprobe w1-gpio\n");
            printf("  sudo modprobe w1-therm\n");
            printf("For Raspberry Pi you can add dtoverlay=w1-gpio,gpiopin=<gpio_pin> in config.txt\n");
            exit(0);
        }
        else
        {
            fprintf(stderr, "Invalid argument: %s\n", argv[i]);
            fprintf(stderr, "Use -h or --help for usage information\n");
            exit(1);
        }
    }

    // Require -pin argument
    if (pin_num < 0)
    {
        fprintf(stderr, "Error: -pin argument is required.\n");
        fprintf(stderr, "Usage: %s -pin <gpio_pin> [-serial <28-xxxx>]\n", argv[0]);
        exit(1);
    }

    // Find the sensor
    if (!find_sensor(device_path, serial))
    {
        if (serial != NULL)
        {
            fprintf(stderr, "Error: DS18B20 sensor with serial %s not found\n", serial);
        }
        else
        {
            fprintf(stderr, "Error: No DS18B20 sensor found\n");
        }
        fprintf(stderr, "Make sure the sensor is connected and kernel modules are loaded.\n");
        exit(1);
    }

    // Read temperature with retry logic
    if (read_ds18b20(device_path, &temperature, 0))
    {
        gethostname(hostbuffer, sizeof(hostbuffer));
        printf("Weather,host=%s,pinnum=%d,sensor_type_name=ds18b20 temperature=%.1f\n",
               hostbuffer, pin_num, temperature);
    }
    else
    {
        fprintf(stderr, "Failed to read temperature from DS18B20\n");
        exit(1);
    }

    return 0;
}