#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "lib/kernel/stdio.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "vm/frame.h"

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
static void handle_mmap (struct intr_frame *f);
static void handle_munmap (struct intr_frame *f);

static bool is_valid_user_ptr (const void *uaddr);
static bool is_valid_read_range (const void *start, size_t size);
static bool is_valid_string (const char *str);
static bool copy_from_user (void *kaddr, const void *uaddr, size_t size);
static bool copy_to_user (void *uaddr, const void *kaddr, size_t size);
static bool get_syscall_arg (struct intr_frame *f, int arg_num, void *dest, size_t size);

/* VM-aware helper functions */
static bool ensure_page_loaded (const void *uaddr);
static void pin_user_pages_for_copy (const void *start, size_t size);
static void unpin_user_pages_for_copy (const void *start, size_t size);

static struct lock filesys_lock;

void fs_lock_acquire (void) { lock_acquire (&filesys_lock); }
void fs_lock_release (void) { lock_release (&filesys_lock); }
bool fs_lock_held_by_current_thread (void) { return lock_held_by_current_thread (&filesys_lock); }

const size_t putbuf_chunk_size = 512;

static bool
is_valid_user_ptr (const void *uaddr)
{
  if (uaddr == NULL || !is_user_vaddr(uaddr))
    return false;

  /* Check if page is present in hardware page table */
  void *kpage = pagedir_get_page(thread_current()->pagedir, uaddr);
  if (kpage != NULL) {
    return true;  /* Page is loaded and mapped */
  }
  
  /* Page not present - check if it's valid in SPT */
  struct spt_entry *entry = spt_lookup(&thread_current()->spt, uaddr);
  return entry != NULL;  /* Valid if entry exists in SPT */
}

static bool
is_valid_read_range (const void *start, size_t size)
{
  if (size == 0)
    return true;
    
  const char *ptr = (const char *) start;
  
  /* Check for overflow */
  if (ptr + size < ptr)
    return false;
    
  /* Check that end address is still in user space */
  if (!is_user_vaddr(ptr + size - 1))
    return false;

  /* Check and load each page in the range */
  void *page_start = pg_round_down(ptr);
  void *page_end = pg_round_down(ptr + size - 1);
  
  for (void *page = page_start; page <= page_end; page += PGSIZE) {
    if (!is_valid_user_ptr(page)) {
      return false;  /* Invalid address */
    }
    
    if (!ensure_page_loaded(page)) {
      return false;  /* Failed to load page */
    }
  }
  
  return true;
}

static bool
is_valid_string (const char *str)
{
  if (!is_valid_user_ptr(str))
    return false;
    
  const char *ptr = str;
  void *current_page = pg_round_down(ptr);
  
  /* Ensure initial page is loaded */
  if (!ensure_page_loaded(current_page))
    return false;
    
  while (true) {
    /* Load new page if we've crossed a boundary */
    void *ptr_page = pg_round_down(ptr);
    if (ptr_page != current_page) {
      if (!is_valid_user_ptr(ptr) || !ensure_page_loaded(ptr)) {
        return false;
      }
      current_page = ptr_page;
    }
    
    /* Check current character */
    if (*ptr == '\0') {
      break;  /* Found null terminator */
    }
    
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

  /* Validate destination is in kernel space */
  if (!is_kernel_vaddr(dst) || !is_kernel_vaddr(dst + size - 1))
    return false;

  /* Validate and load all source pages */
  if (!is_valid_read_range(src, size))
    return false;

  /* Pin all pages during copy to prevent eviction */
  pin_user_pages_for_copy(src, size);

  /* Safe to copy - all pages loaded and pinned */
  memcpy(dst, src, size);

  /* Unpin pages */
  unpin_user_pages_for_copy(src, size);

  return true;
}

static bool
 __attribute__((unused)) copy_to_user (void *uaddr_, const void *kaddr_, size_t size)
{
  if (size == 0)
    return true;

  uint8_t *dst = (uint8_t *) uaddr_;
  const uint8_t *src = (const uint8_t *) kaddr_;

  /* Validate source is in kernel space */
  if (!is_kernel_vaddr(src) || !is_kernel_vaddr(src + size - 1))
    return false;

  /* Validate and load all destination pages */
  if (!is_valid_read_range(dst, size))
    return false;

  /* Pin all pages during copy */
  pin_user_pages_for_copy(dst, size);

  /* Safe to copy */
  memcpy(dst, src, size);

  /* Unpin pages */
  unpin_user_pages_for_copy(dst, size);

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

/* Load a page if it's valid but not present */
static bool
ensure_page_loaded (const void *uaddr)
{
  /* Check if already loaded */
  if (pagedir_get_page(thread_current()->pagedir, uaddr) != NULL) {
    return true;
  }
  
  /* Look up in SPT */
  struct spt_entry *entry = spt_lookup(&thread_current()->spt, uaddr);
  if (entry == NULL) {
    return false;  /* Invalid address */
  }
  
  /* Load the page */
  return spt_load_page(entry);
}

static void
pin_user_pages_for_copy (const void *start, size_t size)
{
  void *page_start = pg_round_down(start);
  void *page_end = pg_round_down((char *)start + size - 1);
  
  for (void *page = page_start; page <= page_end; page += PGSIZE) {
    /* Get the frame for this page */
    void *kpage = pagedir_get_page(thread_current()->pagedir, page);
    ASSERT(kpage != NULL);  /* Should be loaded by now */
    
    struct frame_entry *fe = frame_lookup(kpage);
    if (fe != NULL) {
      frame_pin(fe);
    }
  }
}

/* Unpin user pages after copy operation */
static void
unpin_user_pages_for_copy (const void *start, size_t size)
{
  void *page_start = pg_round_down(start);
  void *page_end = pg_round_down((char *)start + size - 1);
  
  for (void *page = page_start; page <= page_end; page += PGSIZE) {
    void *kpage = pagedir_get_page(thread_current()->pagedir, page);
    if (kpage != NULL) {
      struct frame_entry *fe = frame_lookup(kpage);
      if (fe != NULL) {
        frame_unpin(fe);
      }
    }
  }
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
  /* Save user ESP for potential kernel faults during syscall */
  struct thread *cur = thread_current();
  if (cur->is_user_process) {
    cur->user_esp = f->esp;
  }
  
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
    case SYS_MMAP:
      handle_mmap(f);
      break;
    case SYS_MUNMAP:
      handle_munmap(f);
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
  
  /* Validate and load all buffer pages */
  if (!is_valid_read_range(buf, size))
    thread_exit();
  
  /* Check bounds first */
  if (fd < 0 || fd >= FD_CNT)
  {
    f->eax = 0;
    return;
  }

  if (fd == 1)
    {
      /* Pin buffer during output */
      pin_user_pages_for_copy(buf, size);
      
      size_t original_size = size;
      char *original_buf = buf;
      while (size > 0)
        {
          size_t chunk_size = (size > putbuf_chunk_size) ? putbuf_chunk_size : size;
          putbuf(buf, chunk_size);
          buf += chunk_size;
          size -= chunk_size;
        }
      
      unpin_user_pages_for_copy(original_buf, original_size);
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
          
          /* Pin buffer during file I/O */
          pin_user_pages_for_copy(buf, size);
          
          fs_lock_acquire();
          size_t bytes_written = file_write(file, buf, size);
          fs_lock_release();
          
          unpin_user_pages_for_copy(buf, size);
          f->eax = bytes_written;
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
  
  /* Validate and load all buffer pages */
  if (!is_valid_read_range(buf, size)) 
      thread_exit();

  /* Check bounds first */
  if (fd < 0 || fd >= FD_CNT)
    {
      f->eax = -1;
      return;
    }

  if (fd == 0)
    {
      /* Pin buffer during input to prevent eviction */
      pin_user_pages_for_copy(buf, size);
      
      size_t bytes_read = 0;
      while (bytes_read < size)
        {
          int c = input_getc();
          if (c == -1)  /* Use -1 instead of EOF */
            break;
          buf[bytes_read] = c;
          bytes_read++;
        }
      
      unpin_user_pages_for_copy(buf, size);
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
          /* Pin buffer during file I/O */
          pin_user_pages_for_copy(buf, size);
          
          fs_lock_acquire();
          size_t bytes_read = file_read(file, buf, size);
          fs_lock_release();
          
          unpin_user_pages_for_copy(buf, size);
          f->eax = bytes_read;
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

/* Helper functions for memory mapping */
static struct mmap_entry *mmap_lookup(mapid_t mapid);
static bool mmap_is_valid_addr(void *addr, size_t length);
static void mmap_unmap_pages(struct mmap_entry *mmap);

static void
handle_mmap (struct intr_frame *f)
{
  int fd;
  void *addr;
  
  if (!get_syscall_arg(f, 1, &fd, sizeof(int)) ||
      !get_syscall_arg(f, 2, &addr, sizeof(void *)))
    {
      thread_exit();
    }
  
  /* Validate arguments */
  if (fd <= 1 || fd >= FD_CNT || addr == NULL || 
      !is_user_vaddr(addr) || pg_ofs(addr) != 0)
    {
      f->eax = MAP_FAILED;
      return;
    }
  
  /* Get file from file descriptor table */
  struct thread *cur = thread_current();
  struct file *file = cur->fd_table[fd];
  if (file == NULL)
    {
      f->eax = MAP_FAILED;
      return;
    }
  
  /* Get file size */
  fs_lock_acquire();
  off_t file_size = file_length(file);
  fs_lock_release();
  
  if (file_size == 0)
    {
      f->eax = MAP_FAILED;
      return;
    }
  
  /* Calculate number of pages needed */
  size_t page_count = (file_size + PGSIZE - 1) / PGSIZE;
  
  /* Check if mapping would overlap existing pages */
  if (!mmap_is_valid_addr(addr, page_count * PGSIZE))
    {
      f->eax = MAP_FAILED;
      return;
    }
  
  /* Create separate file reference for this mapping */
  fs_lock_acquire();
  struct file *mapped_file = file_reopen(file);
  fs_lock_release();
  
  if (mapped_file == NULL)
    {
      f->eax = MAP_FAILED;
      return;
    }
  
  /* Create mapping entry */
  struct mmap_entry *mmap = malloc(sizeof(struct mmap_entry));
  if (mmap == NULL)
    {
      fs_lock_acquire();
      file_close(mapped_file);
      fs_lock_release();
      f->eax = MAP_FAILED;
      return;
    }
  
  /* Initialize mapping */
  mmap->mapid = cur->next_mapid++;
  mmap->file = mapped_file;
  mmap->start_addr = addr;
  mmap->page_count = page_count;
  
  /* Create SPT entries for each page in the mapping */
  bool success = true;
  for (size_t i = 0; i < page_count && success; i++)
    {
      void *page_addr = (uint8_t *)addr + i * PGSIZE;
      off_t offset = i * PGSIZE;
      size_t read_bytes = (i == page_count - 1) ? 
                         (file_size - i * PGSIZE) : PGSIZE;

      /* Create SPT entry */
      struct spt_entry *entry = spt_create_entry(page_addr, PAGE_MMAP, true);
      if (entry == NULL)
        {
          success = false;
          break;
        }
      
      /* Set file data */
      if (!spt_set_file_data(entry, mapped_file, offset, read_bytes))
        {
          free(entry);
          success = false;
          break;
        }
      
      /* Set mapping ID */
      entry->mapid = mmap->mapid;
      
      /* Insert into SPT */
      if (!spt_insert(&cur->spt, entry))
        {
          free(entry);
          success = false;
          break;
        }
    }
  
  if (success)
    {
      /* Add to process mapping list */
      list_push_back(&cur->mmap_list, &mmap->elem);
      f->eax = mmap->mapid;
    }
  else
    {
      /* Clean up on failure */
      mmap_unmap_pages(mmap);
      fs_lock_acquire();
      file_close(mapped_file);
      fs_lock_release();
      free(mmap);
      f->eax = MAP_FAILED;
    }
}

static void
handle_munmap (struct intr_frame *f)
{
  mapid_t mapid;
  
  if (!get_syscall_arg(f, 1, &mapid, sizeof(mapid_t)))
    {
      thread_exit();
    }
  
  /* Find mapping */
  struct mmap_entry *mmap = mmap_lookup(mapid);
  if (mmap == NULL)
    {
      thread_exit(); /* Invalid mapping ID */
    }
  
  /* Unmap all pages */
  mmap_unmap_pages(mmap);
  
  /* Close file and free mapping */
  fs_lock_acquire();
  file_close(mmap->file);
  fs_lock_release();
  
  list_remove(&mmap->elem);
  free(mmap);
}

/* Look up memory mapping by ID */
static struct mmap_entry *
mmap_lookup(mapid_t mapid)
{
  struct thread *cur = thread_current();
  struct list_elem *e;
  
  for (e = list_begin(&cur->mmap_list); e != list_end(&cur->mmap_list);
       e = list_next(e))
    {
      struct mmap_entry *mmap = list_entry(e, struct mmap_entry, elem);
      if (mmap->mapid == mapid)
        return mmap;
    }
  
  return NULL;
}

/* Check if address range is valid for mapping */
static bool
mmap_is_valid_addr(void *addr, size_t length)
{
  struct thread *cur = thread_current();
  uint8_t *start = (uint8_t *)addr;
  uint8_t *end = start + length;
  
  /* Check each page in the range */
  for (uint8_t *page = start; page < end; page += PGSIZE)
    {
      /* Check if page already exists in SPT */
      if (spt_lookup(&cur->spt, page) != NULL)
        return false;
      
      /* Check if page would overlap with stack */
      if (cur->stack_bottom != NULL && 
          page >= (uint8_t *)cur->stack_bottom - cur->stack_size)
        return false;
    }
  
  return true;
}

/* Unmap pages for a memory mapping */
static void
mmap_unmap_pages(struct mmap_entry *mmap)
{
  struct thread *cur = thread_current();
  
  for (size_t i = 0; i < mmap->page_count; i++)
    {
      void *page_addr = (uint8_t *)mmap->start_addr + i * PGSIZE;
      struct spt_entry *entry = spt_lookup(&cur->spt, page_addr);
      
      if (entry != NULL)
        {
          /* If page is loaded and dirty, write back to file */
          if (entry->status == PAGE_LOADED && entry->kpage != NULL)
            {
              /* Check if page is dirty */
              if (pagedir_is_dirty(cur->pagedir, entry->vaddr))
                {
                  /* Write back to file */
                  fs_lock_acquire();
                  file_write_at(entry->file, entry->kpage, 
                               entry->read_bytes, entry->file_offset);
                  fs_lock_release();
                }
              
              /* Unload page */
              spt_unload_page(entry);
            }
          
          /* Remove from SPT */
          spt_remove(&cur->spt, page_addr);
        }
    }
}