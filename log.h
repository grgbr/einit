#ifndef _TINIT_LOG_H
#define _TINIT_LOG_H

#include "common.h"

extern void
tinit_parse_stdlog_arg(char * __restrict arg, size_t len);

extern void
tinit_parse_mqlog_arg(char * __restrict arg, size_t len);

extern void
tinit_preinit_logs(void);

extern void
tinit_postinit_logs(void);

extern void
tinit_prefini_logs(void);

extern void
tinit_postfini_logs(void);

#endif /* _TINIT_LOG_H */
