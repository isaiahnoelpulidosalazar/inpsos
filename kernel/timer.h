#ifndef TIMER_H
#define TIMER_H
#include <stdint.h>

void     timer_init(void);          /* calibrates the TSC against the PIT, reads the RTC */
int64_t  timer_uptime_ms(void);     /* monotonic, ms since timer_init() */
int64_t  timer_unix_ms(void);       /* wall clock (UTC), ms since 1970-01-01 */
void     timer_sleep_ms(int64_t ms);

#endif
