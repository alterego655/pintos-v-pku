#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "vm/frame.h"
#include "vm/page.h"

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
#define ALIGN_DOWN(addr, align)   ((addr) & ~((align) - 1))

// Create a structure to pass both file_name and child_status
struct exec_info {
  // char *file_name;
  char *file_name;
  struct child_status *child_status;
};

/** Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  tid_t tid;
  struct exec_info *info = malloc(sizeof(struct exec_info));

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  info->file_name = palloc_get_page (0);
  if (info->file_name == NULL)
    {
      free(info);
      return TID_ERROR;
    }
    
  strlcpy (info->file_name, file_name, PGSIZE);
  
  info->child_status = child_status_create();
  if (info->child_status == NULL)
    {
      free(info);
      return TID_ERROR;
    }

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (file_name, PRI_DEFAULT, start_process, info);

  if (tid == TID_ERROR)
    {
      goto error;
    }

  sema_down(&info->child_status->load_sema);

  ASSERT(info->child_status->load_done);
  if (!info->child_status->load_ok)
    {
      goto error;
    }

  info->child_status->child_tid = tid;
  list_push_back(&thread_current()->children, &info->child_status->elem);
  
  free(info);
  return tid;

error:
  child_status_destroy(info->child_status);
  free(info);
  return TID_ERROR;
}

/** A thread function that loads a user process and starts it
   running. */
static void
start_process (void *info_)
{
  struct exec_info *info = info_;
  struct intr_frame if_;
  bool success;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  
  /* Initialize supplemental page table. */
  spt_init(&thread_current()->spt);
  
  struct thread *cur = thread_current();
  
  /* Initialize stack metadata - will be set properly in setup_stack */
  cur->stack_bottom = NULL;
  cur->stack_size = 0;
  cur->user_esp = NULL;
  
  /* Initialize memory mapping tracking */
  list_init(&cur->mmap_list);
  cur->next_mapid = 1;  /* Start mapping IDs from 1 */
  
  success = load (info->file_name, &if_.eip, &if_.esp);

  /* If load failed, quit. */
  if (!success) {
    info->child_status->load_ok = false;
    info->child_status->load_done = true;
    palloc_free_page (info->file_name);
    sema_up(&info->child_status->load_sema);
    thread_exit ();
  }
  
  info->child_status->load_ok = true;
  info->child_status->load_done = true;
  info->child_status->child_tid = cur->tid;
  sema_up(&info->child_status->load_sema);

  cur->is_user_process = true;
  cur->self_status = info->child_status;

  char *process_name;
  char *save_ptr;

  process_name = strtok_r(info->file_name, " ", &save_ptr);
  if (process_name != NULL)
    strlcpy(cur->self_status->process_name, process_name, 16);
  else
    strlcpy(cur->self_status->process_name, "unknown", 16);

  palloc_free_page (info->file_name);

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/** Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid UNUSED) 
{
  struct child_status *cs = child_status_find(thread_current(), child_tid);

  if (cs == NULL || cs->parent_waited ||
     cs->child_tid == TID_ERROR)
    return -1;

  if (cs->child_exited && cs->exit_code == -1) 
    return -1;
  
  if (!cs->child_exited) {
    sema_down(&cs->exit_sema);
  }

  cs->parent_waited = true;

  list_remove(&cs->elem);
  int exit_code = cs->exit_code;
  child_status_destroy(cs);
  
  return exit_code;
}

/** Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;
  
  if (cur->is_user_process)
    {
      printf ("%s: exit(%d)\n", cur->self_status->process_name, cur->self_status->exit_code);
    }
  
  /* Clean up memory mappings before destroying SPT */
  while (!list_empty(&cur->mmap_list))
    {
      struct list_elem *e = list_pop_front(&cur->mmap_list);
      struct mmap_entry *mmap = list_entry(e, struct mmap_entry, elem);
      
      /* Unmap pages and write back dirty pages */
      for (size_t i = 0; i < mmap->page_count; i++)
        {
          void *page_addr = (uint8_t *)mmap->start_addr + i * PGSIZE;
          struct spt_entry *entry = spt_lookup(&cur->spt, page_addr);
          
          if (entry != NULL && entry->status == PAGE_LOADED && entry->kpage != NULL)
            {
              /* Check if page is dirty and write back */
              if (pagedir_is_dirty(cur->pagedir, entry->vaddr))
                {
                  fs_lock_acquire();
                  file_write_at(entry->file, entry->kpage, 
                               entry->read_bytes, entry->file_offset);
                  fs_lock_release();
                }
            }
        }
      
      /* Close file and free mapping */
      fs_lock_acquire();
      file_close(mmap->file);
      fs_lock_release();
      free(mmap);
    }
  
  /* Clean up all child status records we own (parent process) */
  child_status_cleanup (cur);
  
  if (cur->self_status != NULL)
    {
      cur->self_status->child_exited = true;
      sema_up (&cur->self_status->exit_sema); /* Wake up waiting parent */
    }

  /* Destroy supplemental page table */
  if (cur->pagedir != NULL)
    spt_destroy(&cur->spt);

  bool lock_held = fs_lock_held_by_current_thread();
  if (!lock_held)
    fs_lock_acquire();

  /* Close executable file first */
  if (cur->exec_file != NULL)
    {
      file_allow_write(cur->exec_file);
      file_close(cur->exec_file);
      cur->exec_file = NULL;
    }

  for (int i = 2; i < FD_CNT; i++)
    {
      struct file *file = cur->fd_table[i];
      if (file != NULL)
        {
          file_close(file);
          cur->fd_table[i] = NULL;
        }
    }
  fs_lock_release();

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/** Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/** We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/** ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/** For use with ELF types in printf(). */
#define PE32Wx PRIx32   /**< Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /**< Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /**< Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /**< Print Elf32_Half in hexadecimal. */

/** Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/** Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/** Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /**< Ignore. */
#define PT_LOAD    1            /**< Loadable segment. */
#define PT_DYNAMIC 2            /**< Dynamic linking info. */
#define PT_INTERP  3            /**< Name of dynamic loader. */
#define PT_NOTE    4            /**< Auxiliary info. */
#define PT_SHLIB   5            /**< Reserved. */
#define PT_PHDR    6            /**< Program header table. */
#define PT_STACK   0x6474e551   /**< Stack segment. */

/** Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /**< Executable. */
#define PF_W 2          /**< Writable. */
#define PF_R 4          /**< Readable. */

static bool setup_stack (void **esp, const char *file_name);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/** Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();
  
  /* Make a copy of file_name to parse, since strtok_r modifies the string */
  char *file_name_copy = malloc(strlen(file_name) + 1);
  if (file_name_copy == NULL)
    goto done;
  strlcpy(file_name_copy, file_name, strlen(file_name) + 1);
  
  char *process_name;
  char *save_ptr;
  process_name = strtok_r(file_name_copy, " ", &save_ptr);
  
  bool lock_held = fs_lock_held_by_current_thread();
  if (!lock_held)
    {
      fs_lock_acquire();
    } 
  /* Open executable file. */
  file = filesys_open (process_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", process_name);
      fs_lock_release();
      goto done; 
    }
  t->exec_file = file;
  file_deny_write(file);
  fs_lock_release();

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", process_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp, file_name))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  if (file_name_copy != NULL)
    free (file_name_copy);
  
  // DON'T close the file here if load was successful!
  if (!success && file != NULL)
    {
      t->exec_file = NULL;
      file_close (file);
    }
  return success;
}

/** load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/** Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/** Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  off_t file_offset = ofs;
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;
      
      /* Calculate how much of total zero_bytes this page consumes */
      size_t zero_bytes_used = zero_bytes < page_zero_bytes ? zero_bytes : page_zero_bytes;

      /* Create SPT entry for lazy loading */
      enum page_type type = writable ? PAGE_DATA : PAGE_EXECUTABLE;
      struct spt_entry *entry = spt_create_entry(upage, type, writable);
      if (entry == NULL)
        return false;

      /* Set file data for this page */
      if (!spt_set_file_data(entry, file, file_offset, page_read_bytes, page_zero_bytes))
        {
          free(entry);
          return false;
        }

      /* Insert entry into SPT */
      if (!spt_insert(&thread_current()->spt, entry))
        {
          free(entry);
          return false;
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= zero_bytes_used;
      upage += PGSIZE;
      file_offset += page_read_bytes;
    }
  return true;
}

/** Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp, const char *file_name) 
{
  bool success = false;
  char *file_to_split = malloc(strlen(file_name) + 1);
  
  if (file_to_split == NULL)
    return false;
    
  strlcpy(file_to_split, file_name, strlen(file_name) + 1);

  /* Create SPT entry for initial stack page */
  void *stack_page = ((uint8_t *) PHYS_BASE) - PGSIZE;
  struct spt_entry *entry = spt_create_entry(stack_page, PAGE_STACK, true);
  if (entry != NULL && spt_insert(&thread_current()->spt, entry))
    {
      /* Load the stack page immediately since we need to set up arguments */
      if (spt_load_page(entry))
        {
          success = true;
          *esp = PHYS_BASE;
          
          /* Update stack metadata to be consistent */
          struct thread *cur = thread_current();
          cur->stack_bottom = PHYS_BASE;           /* Bottom is top of user space */
          cur->stack_size = PGSIZE;                /* One page allocated */
          cur->user_esp = PHYS_BASE;               /* Initial ESP at top */
        }
    }

  if (success)
    {
      void *sp = *esp;
      char *token, *save_ptr;
      char *argv[128]; // Increased size slightly for safety, can be tuned
      int argc = 0, total_len = 0;

      // 1. Parse arguments
      for (token = strtok_r (file_to_split, " ", &save_ptr); token != NULL; token = strtok_r (NULL, " ", &save_ptr))
        {
          if (argc < 127) // Prevent overflow
            {
              argv[argc++] = token;
              total_len += strlen(token) + 1;
              if (total_len > PGSIZE)
                {
                  success = false;
                  break;
                }
            }
          else
            {
              // Handle too many arguments, e.g., set success to false and return
              // For now, just break to limit arguments
              break; 
            }
        }
  
      uint8_t *sp_byte = (uint8_t *) sp;

      // 2. Push argument strings onto stack (high addresses to low addresses)
      for (int i = argc - 1; i >= 0; i--)
        {
          int len = strlen (argv[i]) + 1; // +1 for null terminator
          sp_byte -= len;
          memcpy (sp_byte, argv[i], len);
          argv[i] = (char *) sp_byte; // Update argv to point to stack location
        }
  
      // 3. Align stack pointer (to a multiple of 4 for char*)
      // Cast to uintptr_t for bitwise operations
      uintptr_t sp_addr = (uintptr_t) sp_byte;
      sp_addr = ALIGN_DOWN(sp_addr, sizeof(char *)); 
      sp_byte = (uint8_t *) sp_addr;

      // 4. Now set sp_word to the current aligned position
      uintptr_t *sp_word = (uintptr_t *)sp_byte;

      // 5. Push null pointer sentinel (argv[argc])
      *--sp_word = 0;

      // 6. Push argv[i] (pointers to strings) in reverse order
      for (int i = argc - 1; i >= 0; i--)
        *--sp_word = (uintptr_t) argv[i];
  
      // 7. Push argv (pointer to argv array)
      uintptr_t *argv_on_stack = sp_word;
      *--sp_word = (uintptr_t) argv_on_stack;

      // 8. Push argc
      *--sp_word = argc;
      
      // 9. Push fake return address
      *--sp_word = 0; 

      *esp = (void *) sp_word;
    }
    
  free(file_to_split);
  return success;
}

/** Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}

/* Child status management functions */

/** Creates a new child status record for the given child tid.
    Returns the allocated record, or NULL if allocation fails. */
struct child_status *
child_status_create ()
{
  struct child_status *cs = malloc (sizeof (struct child_status));
  if (cs == NULL)
    return NULL;
    
  cs->child_tid = TID_ERROR;
  cs->load_ok = false;
  cs->child_exited = false;
  cs->parent_waited = false;
  cs->parent_exited_first = false;
  cs->exit_code = -1;
  sema_init (&cs->load_sema, 0);
  sema_init (&cs->exit_sema, 0);
  strlcpy(cs->process_name, "unknown", 16);
  
  return cs;
}
 
/** Finds the child status record for the given child tid in the parent's 
    children list. Returns NULL if not found. */
struct child_status *
child_status_find (struct thread *parent, tid_t child_tid)
{
  struct list_elem *e;
  
  for (e = list_begin (&parent->children); e != list_end (&parent->children);
       e = list_next (e))
    {
      struct child_status *cs = list_entry (e, struct child_status, elem);
      if (cs->child_tid == child_tid)
        return cs;
    }
  return NULL;
}

/** Initializes the child status management fields for a thread.
    Should be called when creating a new thread. */
void
child_status_init (struct thread *t)
{
  list_init (&t->children);
  t->self_status = NULL;
}

void
child_status_destroy (struct child_status *cs)
{
  free(cs);
}

/** Cleans up all child status records for a thread.
    Should be called when a process exits. */
void
child_status_cleanup (struct thread *t)
{
  struct list_elem *e, *next;
  
  /* Clean up all child status records this process owns */
  for (e = list_begin (&t->children); e != list_end (&t->children); e = next)
    {
      next = list_next (e);
      struct child_status *cs = list_entry (e, struct child_status, elem);
      list_remove (e);
      if (cs->child_exited) {
        child_status_destroy (cs);
      } else {
        cs->parent_exited_first = true;
      }
    }
}
