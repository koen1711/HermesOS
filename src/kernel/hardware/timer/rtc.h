#ifndef RTC_H
#define RTC_H

#define RTC_COMMAND_PORT 0x70
#define RTC_DATA_PORT 0x71

#define NMI_DISABLE_BIT 0x80

typedef struct {
    unsigned char seconds;
    unsigned char minutes;
    unsigned char hours;
} rtc_time;

typedef struct {
    unsigned char day;
    unsigned char month;
    unsigned char year;
} rtc_date;

typedef struct {
    rtc_time time;
    rtc_date date;
} rtc_datetime;

void rtc_initialize();

void rtc_initialize();

rtc_datetime rtc_get_datetime();
rtc_time rtc_get_time();
rtc_date rtc_get_date();

void rtc_set_datetime(rtc_datetime datetime);
void rtc_set_time(rtc_time time);
void rtc_set_date(rtc_date date);

#endif //RTC_H
