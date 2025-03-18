#ifndef RTC_H
#define RTC_H

#define RTC_COMMAND_PORT 0x70
#define RTC_DATA_PORT 0x71

#define NMI_DISABLE_BIT 0x80

struct rtc_time {
    unsigned char seconds;
    unsigned char minutes;
    unsigned char hours;
};

struct rtc_date {
    unsigned char day;
    unsigned char month;
    unsigned char year;
};

struct rtc_datetime {
    struct rtc_time time;
    struct rtc_date date;
};

void rtc_initialize();

void rtc_initialize();

struct rtc_datetime rtc_get_datetime();
struct rtc_time rtc_get_time();
struct rtc_date rtc_get_date();

void rtc_set_datetime(struct rtc_datetime datetime);
void rtc_set_time(struct rtc_time time);
void rtc_set_date(struct rtc_date date);

#endif //RTC_H
