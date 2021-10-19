#ifndef __LIGHT_LOG_H__
#define __LIGHT_LOG_H__

#define LOG_TO_STDOUT 0
#define LOG_TO_SYSLOG 1

enum
{
	LIGHT_LOG_DEBUG,
	LIGHT_LOG_INFO,
	LIGHT_LOG_WARNING,
	LIGHT_LOG_ERR,
	LIGHT_LOG_CRIT,
	LIGHT_LOG_NONE
};

void light_log_init(int);
extern inline void light_log(int, const char *, ...);
void light_set_log_level(int);

#endif
