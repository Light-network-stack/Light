#include "../../light_debug.h"
#include <light_log.h>
#include <syslog.h>
#include <stdio.h>
#include <stdarg.h>

static int g_light_log_dest = LOG_TO_SYSLOG; /* 0 - stdio, 1 - syslog */
static int g_light_log_level = LIGHT_LOG_NONE;
void light_log_init(int dest)
{
	g_light_log_dest = dest;
	switch(g_light_log_dest) {
		case LOG_TO_STDOUT:
    		break;
		case LOG_TO_SYSLOG:
			openlog("light", LOG_PID|LOG_CONS, LOG_LOCAL7);
    		break;
	}
}

void light_set_log_level(int log_level)
{
	printf("set log level = %d\n", log_level);
	g_light_log_level = log_level;
}

inline void light_log(int level, const char* format, ...)
{
#ifdef LIGHT_LOG
	if (level < g_light_log_level)
		return;
	va_list argptr;
	switch(g_light_log_dest) {
        case LOG_TO_STDOUT:
    		va_start(argptr, format);
			vfprintf(stdout, format, argptr);
    		printf("\n");
            va_end(argptr);
		    break;
        case LOG_TO_SYSLOG:
    		va_start(argptr, format);
			switch(level) {
				case LIGHT_LOG_DEBUG:
					vsyslog(LOG_DEBUG, format, argptr);
				break;
				case LIGHT_LOG_INFO:
					vsyslog(LOG_INFO, format, argptr);
				break;
				case LIGHT_LOG_WARNING:
					vsyslog(LOG_WARNING, format, argptr);
				case LIGHT_LOG_ERR:
					vsyslog(LOG_ERR,format, argptr);
				break;
				case LIGHT_LOG_CRIT:
					vsyslog(LOG_CRIT,format, argptr);
				break;
			}
            va_end(argptr);
		    break;
	}
#endif
}
