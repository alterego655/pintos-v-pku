#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);
void fs_lock_acquire (void);
void fs_lock_release (void);

#endif /**< userprog/syscall.h */
