#ifndef _TINIT_MNT_H
#define _TINIT_MNT_H

#include "common.h"

extern int mnt_mount_all(void);
extern void mnt_umount_all(int flags);

#endif /* _TINIT_MNT_H */
