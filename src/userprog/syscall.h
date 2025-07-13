#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdbool.h>

void syscall_init (void);
void fs_lock_acquire (void);
void fs_lock_release (void);
bool fs_lock_held_by_current_thread (void);

#endif /**< userprog/syscall.h */
