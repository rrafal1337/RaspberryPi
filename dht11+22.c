// DHT11 program was found on
// https://www.circuitbasics.com/how-to-set-up-the-dht11-humidity-sensor-on-the-raspberry-pi/
// Improved with help of ChatGPT to support DHT11 and DHT22 as well configurable data pin.
// Surprisingly ChatGPT assumed 80 bits of data for DHT22 and didn't find own mistake.
// Corrected manually to 40 bits and work fine.
// Output is in line protocol format to use in influxdata telegraf.
// https://docs.influxdata.com/influxdb/cloud/reference/syntax/line-protocol/
// Program for Raspberry Pi board. 
// Compiling: gcc dht11+22.c -o dht11+22 -l wiringPi

#include <wiringPi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>

#define MAXTIMINGS 85

int dht_dat[5] = {0, 0, 0, 0, 0}; // Data array to hold received values
char hostbuffer[256];
int maxRetries = 7;
int retries = 0;

// Function to read from the sensor (DHT11 or DHT22)
void read_dht_dat(int DHTPIN, int sensor_type)
{
    uint8_t laststate = HIGH;
    uint8_t counter = 0;
    uint8_t j = 0, i;
    dht_dat[0] = dht_dat[1] = dht_dat[2] = dht_dat[3] = dht_dat[4] = 0;

    // Starting transmission
    pinMode(DHTPIN, OUTPUT);
    digitalWrite(DHTPIN, LOW);
    delay(18);
    digitalWrite(DHTPIN, HIGH);
    delayMicroseconds(40);
    pinMode(DHTPIN, INPUT);

    // Reading incoming data
    for (i = 0; i < MAXTIMINGS; i++)
    {
        counter = 0;
        while (digitalRead(DHTPIN) == laststate)
        {
            counter++;
            delayMicroseconds(1);
            if (counter == 255)
            {
                break;
            }
        }
        laststate = digitalRead(DHTPIN);

        if (counter == 255)
            break;

        if ((i >= 4) && (i % 2 == 0))
        {
            dht_dat[j / 8] <<= 1;
            if (counter > 16)
                dht_dat[j / 8] |= 1;
            j++;
        }
    }

    if ((j >= 40) && // required 40 bits for both types and checking checksum
        (dht_dat[4] == ((dht_dat[0] + dht_dat[1] + dht_dat[2] + dht_dat[3]) & 0xFF)))
    {

        // Handle DHT11 (8-bit) or DHT22 (16-bit)
        float temperature, humidity;

        if (sensor_type == 11)
        { // DHT11
            humidity = dht_dat[0];
            temperature = dht_dat[2];
        }
        else if (sensor_type == 22)
        { // DHT22
            humidity = (dht_dat[0] << 8) + dht_dat[1];
            temperature = (dht_dat[2] << 8) + dht_dat[3];
            if (dht_dat[2] & 0x80) // Check if temperature is negative
                temperature = -temperature;
            // Handle the decimal part for DHT22 (1 decimal point)
            temperature /= 10.0;
            humidity /= 10.0;
        }

        gethostname(hostbuffer, sizeof(hostbuffer));
        printf("Weather,host=%s,pinnum=%d humidity=%.1f,temperature=%.1f\n",
               hostbuffer, DHTPIN, humidity, temperature);
    }
    else
    {
        if (retries < maxRetries)
        {
            retries++;
            delay(3000);
            read_dht_dat(DHTPIN, sensor_type); // Retry if the data is incorrect
        }
    }
}

int main(int argc, char *argv[])
{
    if (argc != 5)
    {
        fprintf(stderr, "Usage: %s -dhtpin <pin_number> -sensor <11|22>\n", argv[0]);
        exit(1);
    }

    int DHTPIN = -1;
    int sensor_type = 0; // Default to 0 (invalid)

    // Parse the arguments
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-dhtpin") == 0)
        {
            DHTPIN = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-sensor") == 0)
        {
            sensor_type = atoi(argv[++i]);
            if (sensor_type != 11 && sensor_type != 22)
            {
                fprintf(stderr, "Invalid sensor type. Use 11 for DHT11 or 22 for DHT22.\n");
                exit(1);
            }
        }
        else
        {
            fprintf(stderr, "Invalid argument: %s\n", argv[i]);
            exit(1);
        }
    }

    if (DHTPIN == -1 || sensor_type == 0)
    {
        fprintf(stderr, "Usage: %s -dhtpin <pin_number> -sensor <11|22>\n", argv[0]);
        exit(1);
    }

    if (wiringPiSetup() == -1)
    {
        exit(1);
    }

    read_dht_dat(DHTPIN, sensor_type);

    return 0;
}
