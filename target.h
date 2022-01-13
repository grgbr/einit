#ifndef _TINIT_TARGET_H
#define _TINIT_TARGET_H

struct tinit_sigchan;
struct upoll;

extern int
tinit_target_start(const char *           dir_path,
                   const char *           name,
                   struct tinit_sigchan * chan,
                   const struct upoll *   poller);

extern void
tinit_target_stop(struct tinit_sigchan * chan);

extern int
tinit_target_switch(const char * dir_path, const char * name);

#endif /* _TINIT_TARGET_H */
