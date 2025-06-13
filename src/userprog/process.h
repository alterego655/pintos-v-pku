#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

/* Child status management functions */
struct child_status *child_status_create (void);
void child_status_destroy (struct child_status *cs);
struct child_status *child_status_find (struct thread *parent, tid_t child_tid);
void child_status_init (struct thread *t);
void child_status_cleanup (struct thread *t);

#endif /**< userprog/process.h */
