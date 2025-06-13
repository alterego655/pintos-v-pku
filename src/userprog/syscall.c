#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "devices/shutdown.h"
#include "lib/kernel/stdio.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"

static void syscall_handler (struct intr_frame *);
static void handle_halt (void);
static void handle_exit (struct intr_frame *f);
static void handle_wait (struct intr_frame *f);
static void handle_exec (struct intr_frame *f);
static void handle_write (struct intr_frame *f);


static bool is_valid_user_ptr (const void *uaddr);
static bool copy_from_user (void *kaddr, const void *uaddr, size_t size);
static bool copy_to_user (void *uaddr, const void *kaddr, size_t size);
static bool get_syscall_arg (struct intr_frame *f, int arg_num, void *dest, size_t size);

static bool
is_valid_user_ptr (const void *uaddr)
{
  if (uaddr == NULL || !is_user_vaddr(uaddr))
    return false;

  if (pagedir_get_page(thread_current()->pagedir, uaddr) == NULL)
    return false;
  
  return true;
}

static bool
copy_from_user (void *kaddr_, const void *uaddr_, size_t size)
{
  /* 1) Cast once to byte pointers. */
  uint8_t *dst = (uint8_t *) kaddr_;
  const uint8_t *src = (const uint8_t *) uaddr_;

  if (size == 0)
    return true;

  /* 3) Copy byte-by-byte, validating *every* user & kernel address. */
  for (size_t i = 0; i < size; i++) 
  {
    /* Validate destination (optional if caller already owns a kernel buffer). */
    if (!is_kernel_vaddr (dst + i))
      return false;

    /* Validate source using your helper (checks <PHYS_BASE> + pagedir). */
    if (!is_valid_user_ptr (src + i))
      return false;

    /* Finally perform the copy. */
    dst[i] = src[i];
  }

  return true;
}

static bool
 __attribute__((unused)) copy_to_user (void *uaddr_, const void *kaddr_, size_t size)
{
  uint8_t *dst = (uint8_t *) uaddr_;
  const uint8_t *src = (const uint8_t *) kaddr_;
  
  if (size == 0)
    return true;

  for (size_t i = 0; i < size; i++)
  {
    if (!is_valid_user_ptr (dst + i))
      return false;
    if (!is_kernel_vaddr (src + i))
      return false;
    dst[i] = src[i];
  }

  return true;
}

/** Safely extracts syscall argument from user stack.
    arg_num: 0 = syscall number, 1 = first arg, 2 = second arg, etc.
    Returns false if extraction fails (invalid address), true on success. */
static bool
get_syscall_arg (struct intr_frame *f, int arg_num, void *dest, size_t size)
{
  void *arg_addr = (uint8_t *)f->esp + (arg_num * sizeof(uint32_t));
  return copy_from_user(dest, arg_addr, size);
}

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  int syscall_num;
  if (!get_syscall_arg(f, 0, &syscall_num, sizeof(int)))
    thread_exit();
  
  switch (syscall_num)
  {
    case SYS_EXIT:
      handle_exit(f);
      break;
    case SYS_HALT:
      handle_halt();
      break;
    case SYS_WRITE:
      handle_write(f);
      break;
    case SYS_WAIT:
      handle_wait(f);
      break;
    case SYS_EXEC:
      handle_exec(f);
      break;
  }
}

static void
handle_wait(struct intr_frame *f)
{
  tid_t child_tid;
  
  if (!get_syscall_arg(f, 1, &child_tid, sizeof(tid_t)))
    {
      thread_exit();
    }
  
  int result = process_wait(child_tid);
  f->eax = result;
}

static void
handle_exec(struct intr_frame *f)
{
  const char *cmd_line;
  
  if (!get_syscall_arg(f, 1, &cmd_line, sizeof(char *)))
    {
      thread_exit();
    }
  
  /* Validate that the command line string is accessible */
  if (!is_valid_user_ptr(cmd_line))
    {
      thread_exit();
    }
  
  /* Validate the entire string is accessible */
  const char *p = cmd_line;
  while (is_valid_user_ptr(p) && *p != '\0')
    p++;
  
  if (!is_valid_user_ptr(p)) /* Check final null terminator */
    {
      thread_exit();
    }
  
  tid_t tid = process_execute(cmd_line);
  f->eax = tid;
}

static void
handle_exit (struct intr_frame *f)
{
  struct thread *cur = thread_current();
  int exit_status;

  if (!get_syscall_arg(f, 1, &exit_status, sizeof(int)))
    thread_exit();
  
  /* Mark self_status as exited if we have one (child process) */
  if (cur->self_status != NULL)
    {
      cur->self_status->exit_code = exit_status;
      cur->self_status->child_exited = true;
      sema_up (&cur->self_status->exit_sema); /* Wake up waiting parent */
    }
    
  thread_exit();
}

static void
handle_halt (void)
{
  shutdown_power_off();
}

static void
handle_write (struct intr_frame *f)
{
  int fd;
  char *buf;
  size_t size;
  
  if (!get_syscall_arg(f, 1, &fd, sizeof(int)) ||
      !get_syscall_arg(f, 2, &buf, sizeof(char *)) ||
      !get_syscall_arg(f, 3, &size, sizeof(size_t)))
    {
      thread_exit();
    }

  if (fd == 1)
  {
    putbuf(buf, size);
    f->eax = size;  // Return number of bytes written
  }
  else
  {
    f->eax = 0;     // Return 0 for unsupported file descriptors
  }
}
