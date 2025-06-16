#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "lib/kernel/stdio.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "userprog/pagedir.h"

static void syscall_handler (struct intr_frame *);
static void handle_halt (void);
static void handle_exit (struct intr_frame *f);
static void handle_wait (struct intr_frame *f);
static void handle_exec (struct intr_frame *f);
static void handle_write (struct intr_frame *f);
static void handle_close (struct intr_frame *f);
static void handle_read (struct intr_frame *f);
static void handle_open (struct intr_frame *f);
static void handle_seek (struct intr_frame *f);
static void handle_tell (struct intr_frame *f);
static void handle_filesize (struct intr_frame *f);
static void handle_remove (struct intr_frame *f);
static void handle_create (struct intr_frame *f);

static bool is_valid_user_ptr (const void *uaddr);
static bool is_valid_read_range (const void *start, size_t size);
static bool is_valid_string (const char *str);
static bool copy_from_user (void *kaddr, const void *uaddr, size_t size);
static bool copy_to_user (void *uaddr, const void *kaddr, size_t size);
static bool get_syscall_arg (struct intr_frame *f, int arg_num, void *dest, size_t size);

static struct lock filesys_lock;

void fs_lock_acquire (void) { lock_acquire (&filesys_lock); }
void fs_lock_release (void) { lock_release (&filesys_lock); }

const size_t putbuf_chunk_size = 512;

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
is_valid_read_range (const void *start, size_t size)
{
  if (size == 0)
    return true;
    
  const char *ptr = (const char *) start;
  // Check for pointer overflow: buf + size < buf
  if (ptr + size < ptr)
    return false;
    
  // Check that end address is still in user space
  if (!is_user_vaddr(ptr + size - 1))
    return false;

  // Validate every byte in the range is mapped
  for (size_t i = 0; i < size; i++)
  {
    if (!is_valid_user_ptr(ptr + i))
      return false;
  }  
  return true;
}

static bool
is_valid_string (const char *str)
{
  if (!is_valid_user_ptr(str))
    return false;
    
  // Check each character until null terminator
  const char *ptr = str;
  while (true)
  {
    if (!is_valid_user_ptr(ptr))
      return false;    

    if (*ptr == '\0')
      break;

    ptr++;
  }
  
  return true;
}

static bool
copy_from_user (void *kaddr_, const void *uaddr_, size_t size)
{
  if (size == 0)
    return true;

  uint8_t *dst = (uint8_t *) kaddr_;
  const uint8_t *src = (const uint8_t *) uaddr_;

  for (size_t i = 0; i < size; i++) 
  {
    if (!is_kernel_vaddr (dst + i))
      return false;

    if (!is_valid_user_ptr (src + i))
      return false;

    dst[i] = src[i];
  }

  return true;
}

static bool
 __attribute__((unused)) copy_to_user (void *uaddr_, const void *kaddr_, size_t size)
{
  if (size == 0)
    return true;

  uint8_t *dst = (uint8_t *) uaddr_;
  const uint8_t *src = (const uint8_t *) kaddr_;

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

/* 
   Safely extracts syscall argument from user stack.
   arg_num: 0 = syscall number, 1 = first arg, 2 = second arg, etc.
   Returns false if extraction fails (invalid address), true on success. 
*/
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
  lock_init(&filesys_lock);
}

static void
syscall_handler (struct intr_frame *f) 
{
  int syscall_num;
  if (!get_syscall_arg(f, 0, &syscall_num, sizeof(int)))
    thread_exit();
  
  switch (syscall_num)
  {
    case SYS_HALT:
      handle_halt();
      break;
    case SYS_EXIT:
      handle_exit(f);
      break;
    case SYS_WAIT:
      handle_wait(f);
      break;
    case SYS_EXEC:
      handle_exec(f);
      break;
    case SYS_WRITE:
      handle_write(f);
      break;
    case SYS_CLOSE:
      handle_close(f);
      break;
    case SYS_READ:
      handle_read(f);
      break;
    case SYS_OPEN:
      handle_open(f);
      break;
    case SYS_SEEK:
      handle_seek(f);
      break;
    case SYS_TELL:
      handle_tell(f);
      break;
    case SYS_FILESIZE:
      handle_filesize(f);
      break;
    case SYS_REMOVE:
      handle_remove(f);
      break;
    case SYS_CREATE:
      handle_create(f);
      break;
  }
}

static void
handle_wait(struct intr_frame *f)
{
  tid_t child_tid;
  
  if (!get_syscall_arg(f, 1, &child_tid, sizeof(tid_t)))
      thread_exit();
  
  int result = process_wait(child_tid);
  f->eax = result;
}

static void
handle_exec(struct intr_frame *f)
{
  const char *cmd_line;
  
  if (!get_syscall_arg(f, 1, &cmd_line, sizeof(char *)))
      thread_exit();
  
  /* Validate the string safely */
  if (!is_valid_string(cmd_line))
      thread_exit();
  
  f->eax = process_execute(cmd_line);
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
      cur->self_status->exit_code = exit_status;
    
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
  
  // Validate the entire buffer range [buf, buf+size)
  if (!is_valid_read_range(buf, size))
    thread_exit();
  
  // Check bounds first
  if (fd < 0 || fd >= FD_CNT)
  {
    f->eax = 0;
    return;
  }

  if (fd == 1)
    {
      size_t original_size = size;
      while (size > 0)
        {
          size_t chunk_size = (size > putbuf_chunk_size) ? putbuf_chunk_size : size;
          putbuf(buf, chunk_size);
          buf += chunk_size;
          size -= chunk_size;
        }
      f->eax = original_size;
    }
  else
    {
      struct file *file = thread_current()->fd_table[fd];
      if (file == NULL)
        {
          f->eax = 0;
        }
      else
        { 
          if (file == thread_current()->exec_file)
            {
              f->eax = 0;
              return;
            }
          fs_lock_acquire();
          size_t bytes_written = file_write(file, buf, size);
          f->eax = bytes_written;
          fs_lock_release();
        }
    }
}


static void
handle_read (struct intr_frame *f)
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
  
  if (!is_valid_read_range(buf, size)) 
      thread_exit();

  // Check bounds first
  if (fd < 0 || fd >= FD_CNT)
    {
      f->eax = -1;
      return;
    }

  if (fd == 0)
    {
      size_t bytes_read = 0;
      while (bytes_read < size)
        {
          int c = input_getc();
          if (c == -1)  /* Use -1 instead of EOF */
            break;
          buf[bytes_read] = c;
          bytes_read++;
        }
      f->eax = bytes_read;
    }
  else
    {
      struct file *file = thread_current()->fd_table[fd];
      if (file == NULL)
        {
          f->eax = -1;
        }
      else
        {
          fs_lock_acquire();
          size_t bytes_read = file_read(file, buf, size);
          f->eax = bytes_read;
          fs_lock_release();
        }
    }
}

static void
handle_open (struct intr_frame *f)
{
  const char *file_name;

  if (!get_syscall_arg(f, 1, &file_name, sizeof(char *)))
    thread_exit();
  
  // Validate the string safely
  if (!is_valid_string(file_name))
    thread_exit();
  
  fs_lock_acquire();
  struct file *file = filesys_open(file_name);
  fs_lock_release();
  
  if (file == NULL)
    {
      f->eax = -1;
      return;
    }
  
  struct thread *cur = thread_current();
  int fd = -1;
  for (int i = 2; i < FD_CNT; i++)
    {
      if (cur->fd_table[i] == NULL)
        {
          fd = i;
          break;
        }
    }

  if (fd == -1)
    {
      fs_lock_acquire();
      file_close(file);
      fs_lock_release();
      f->eax = -1;
      return;
    }
  
  cur->fd_table[fd] = file;
  f->eax = fd;
}

static void
handle_filesize (struct intr_frame *f)
{
  int fd;
  if (!get_syscall_arg(f, 1, &fd, sizeof(int)))
    thread_exit();
  
  // Check bounds first
  if (fd < 0 || fd >= FD_CNT)
    {
      f->eax = -1;
      return;
    }
  
  struct file *file = thread_current()->fd_table[fd];
  if (file == NULL) 
    {
      f->eax = -1;
      return;
    }
  
  fs_lock_acquire();
  f->eax = file_length(file);
  fs_lock_release();
}

static void
handle_seek (struct intr_frame *f)
{
  int fd;
  unsigned int position;

  if (!get_syscall_arg(f, 1, &fd, sizeof(int)) ||
      !get_syscall_arg(f, 2, &position, sizeof(unsigned int)))
    thread_exit();
  
  // Check bounds first
  if (fd < 0 || fd >= FD_CNT)
    {
      return;  // Seek on invalid fd is a no-op
    }
  
  struct file *file = thread_current()->fd_table[fd];
  if (file == NULL)
    {
      return;  // Seek on closed fd is a no-op
    }

  fs_lock_acquire();
  file_seek(file, position);
  fs_lock_release();
}

static void
handle_tell (struct intr_frame *f)
{
  int fd;
  if (!get_syscall_arg(f, 1, &fd, sizeof(int)))
    thread_exit();

  // Check bounds first
  if (fd < 0 || fd >= FD_CNT)
  {
    f->eax = -1;
    return;
  }

  struct file *file = thread_current()->fd_table[fd];
  if (file == NULL)
    {
      f->eax = -1;
      return;
    }
  fs_lock_acquire();
  f->eax = file_tell(file);
  fs_lock_release();
}

static void
handle_create (struct intr_frame *f)
{
  const char *file_name;
  unsigned initial_size;
  
  if (!get_syscall_arg(f, 1, &file_name, sizeof(char *)) ||
      !get_syscall_arg(f, 2, &initial_size, sizeof(unsigned)))
    thread_exit();

  // Validate the string safely without calling strlen() first
  if (!is_valid_string(file_name))
    thread_exit();
    
  // Check for empty string - should fail
  if (file_name[0] == '\0')
    {
      f->eax = 0;
      return;
    }

  fs_lock_acquire();
  bool success = filesys_create(file_name, initial_size);
  fs_lock_release();
  f->eax = success ? 1 : 0;  // Return 1 for success, 0 for failure
}

static void
handle_remove (struct intr_frame *f)
{
  const char *file_name;

  if (!get_syscall_arg(f, 1, &file_name, sizeof(char *)))
    thread_exit();
  
  // Validate the string safely without calling strlen() first
  if (!is_valid_string(file_name))
    thread_exit();
  
  fs_lock_acquire();
  bool success = filesys_remove(file_name);
  fs_lock_release();
  f->eax = success ? 1 : 0;  // Return 1 for success, 0 for failure
}

static void
handle_close (struct intr_frame *f)
{
  int fd;
  if (!get_syscall_arg(f, 1, &fd, sizeof(int)))
    thread_exit();
  
  // Check bounds - prevent array out of bounds access
  if (fd < 0 || fd >= FD_CNT)
    return;
  
  // Can't close stdin (0) or stdout (1)
  if (fd == 0 || fd == 1)
    return;
  
  struct thread *cur = thread_current();
  struct file *file = cur->fd_table[fd];
  
  // Handle already closed or never opened fd gracefully
  if (file == NULL)
    return;

  // Close the file and clear the fd_table entry
  fs_lock_acquire();
  file_close(file);
  fs_lock_release();
  
  cur->fd_table[fd] = NULL;
}