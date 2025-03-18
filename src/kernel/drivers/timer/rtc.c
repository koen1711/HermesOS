#include "rtc.h"

#include <hardware/io_ports/ports.h>

#include "hardware/interrupts/pic.h"

void select_register(const uint8_t reg) {
    ports_write_uint8(RTC_COMMAND_PORT, (reg & 0x7F) | NMI_DISABLE_BIT);
    io_wait();
}

uint8_t read_register(const uint8_t reg) {
    select_register(reg);
    return ports_read_uint8(RTC_DATA_PORT);
}


// Check if RTC updates are in progress
bool is_rtc_updating() {
    select_register(0x0A);
    return (ports_read_uint8(RTC_DATA_PORT) & 0x80) != 0;
}

// Check if RTC is in BCD mode
bool is_bcd_mode() {
    select_register(0x0B);
    uint8_t reg_b = ports_read_uint8(RTC_DATA_PORT);
    return !(reg_b & 0x04);  // Bit 2: 0 = BCD mode, 1 = Binary mode
}

// Convert BCD to binary if needed
uint8_t convert_rtc_value(uint8_t value) {
    if (is_bcd_mode()) {
        return ((value >> 4) * 10) + (value & 0x0F);
    }
    return value;
}


// Safe read with update check and BCD conversion
uint8_t safe_read_register(uint8_t reg) {
    // Wait until any update is complete
    while (is_rtc_updating()) {
        // Wait
    }

    uint8_t value = read_register(reg);
    return convert_rtc_value(value);
}

struct rtc_time rtc_get_time() {
    struct rtc_time time;
    time.seconds = safe_read_register(0x00);
    time.minutes = safe_read_register(0x02);
    time.hours = safe_read_register(0x04);
    return time;
}

struct rtc_date rtc_get_date() {
    struct rtc_date date;
    date.day = safe_read_register(0x07);
    date.month = safe_read_register(0x08);
    date.year = safe_read_register(0x09);
    return date;
}

struct rtc_datetime rtc_get_datetime() {
    struct rtc_datetime datetime;
    datetime.time = rtc_get_time();
    datetime.date = rtc_get_date();
    return datetime;
}

// Convert binary to BCD if needed
uint8_t prepare_rtc_value(uint8_t value) {
    if (is_bcd_mode()) {
        return ((value / 10) << 4) | (value % 10);
    }
    return value;
}

// Safe write with update check and BCD conversion
void safe_write_register(const uint8_t reg, const uint8_t value) {
    // Wait until any update is complete
    while (is_rtc_updating()) {
        // Wait
    }

    select_register(reg);
    ports_write_uint8(RTC_DATA_PORT, prepare_rtc_value(value));
}

void rtc_set_time(const struct rtc_time time) {
    safe_write_register(0x00, time.seconds);
    safe_write_register(0x02, time.minutes);
    safe_write_register(0x04, time.hours);
}

void rtc_set_date(const struct rtc_date date) {
    safe_write_register(0x07, date.day);
    safe_write_register(0x08, date.month);
    safe_write_register(0x09, date.year);
}

void rtc_set_datetime(const struct rtc_datetime datetime) {
    rtc_set_time(datetime.time);
    rtc_set_date(datetime.date);
}

void rtc_initialize() {
    // Set register B: enable 24h mode (bit 1) and binary mode (bit 2)
    select_register(0x0B);
    ports_write_uint8(RTC_DATA_PORT, 0x06); // Force 24h and binary mode

    // Clear any pending interrupts
    select_register(0x0C);
    ports_read_uint8(RTC_DATA_PORT);
}