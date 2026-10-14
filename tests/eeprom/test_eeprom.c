#include "kernel.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int warning_count;

void xbox_log(int level, const char* subsystem, const char* fmt, ...)
{
    (void)subsystem;
    (void)fmt;
    if (level == XBOX_LOG_WARN)
        ++warning_count;
}

static int check(int condition, const char* message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        return 1;
    }
    return 0;
}

static ULONG checksum(const unsigned char* bytes, size_t length)
{
    uint64_t sum = 0;
    size_t i;
    for (i = 0; i + 4 <= length; i += 4) {
        ULONG word;
        memcpy(&word, bytes + i, 4);
        sum += word;
        if (sum > 0xFFFFFFFFu)
            sum = (sum & 0xFFFFFFFFu) + 1u;
    }
    return ~(ULONG)sum;
}

static ULONG read_u32(const unsigned char* bytes)
{
    ULONG value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

int main(void)
{
    unsigned char eeprom[256];
    ULONG type = 0, length = 0;
    unsigned int failures = 0;

    memset(eeprom, 0xA5, sizeof(eeprom));
    NTSTATUS status = xbox_ExQueryNonVolatileSetting(
        0xFFFF, &type, eeprom, sizeof(eeprom), &length);

    failures += check(status == STATUS_SUCCESS, "full EEPROM query succeeds");
    failures += check(type == 3, "full EEPROM is reported as REG_BINARY");
    failures += check(length == sizeof(eeprom), "full EEPROM reports 256 bytes");
    failures += check(eeprom[0x90] == 1, "default EEPROM contains English language setting");
    failures += check(eeprom[0x94] == (XC_VIDEO_FLAGS_WIDESCREEN | XC_VIDEO_FLAGS_HDTV),
                      "default EEPROM contains the modeled video flags");
    failures += check(eeprom[0x98] == 0, "default EEPROM contains stereo audio setting");
    failures += check(read_u32(eeprom + 0x30) == checksum(eeprom + 0x34, 0x2C),
                      "factory checksum matches virtual factory section");
    failures += check(read_u32(eeprom + 0x60) == checksum(eeprom + 0x64, 0x5C),
                      "user checksum matches virtual settings section");
    failures += check(warning_count == 0, "full query does not emit an unhandled-index warning");

    /* A PAL disc loads its official text bank for the console language. */
    {
        ULONG language = 0;
        failures += check(_putenv("RECOMP_XBOX_LANGUAGE=5") == 0, "set Spanish console language");
        status = xbox_ExQueryNonVolatileSetting(XC_LANGUAGE, &type, &language, sizeof(language), &length);
        failures += check(status == STATUS_SUCCESS && language == 5, "XC_LANGUAGE follows RECOMP_XBOX_LANGUAGE");
        xbox_ExQueryNonVolatileSetting(0xFFFF, &type, eeprom, sizeof(eeprom), &length);
        failures += check(eeprom[0x90] == 5, "full EEPROM image agrees with XC_LANGUAGE");
        failures += check(read_u32(eeprom + 0x60) == checksum(eeprom + 0x64, 0x5C),
                          "user checksum covers the selected language");
        failures += check(warning_count == 0, "a valid language does not warn");

        failures += check(_putenv("RECOMP_XBOX_LANGUAGE=12") == 0, "set an invalid console language");
        xbox_ExQueryNonVolatileSetting(XC_LANGUAGE, &type, &language, sizeof(language), &length);
        failures += check(language == 1 && warning_count == 1, "an invalid language falls back to English with a warning");
    }
    return failures ? 1 : 0;
}
