#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "threads/malloc.h"
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "vm/page.h"

/** Number of page faults processed. */
static long long page_fault_cnt;
#define MAX_STACK_SIZE 8 * 1024 * 1024

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);
static bool handle_stack_growth (void *fault_addr, void *esp, bool user_fault);

/** Registers handlers for interrupts that can be caused by user
   programs.

   In a real Unix-like OS, most of these interrupts would be
   passed along to the user process in the form of signals, as
   described in [SV-386] 3-24 and 3-25, but we don't implement
   signals.  Instead, we'll make them simply kill the user
   process.

   Page faults are an exception.  Here they are treated the same
   way as other exceptions, but this will need to change to
   implement virtual memory.

   Refer to [IA32-v3a] section 5.15 "Exception and Interrupt
   Reference" for a description of each of these exceptions. */
void
exception_init (void) 
{
  /* These exceptions can be raised explicitly by a user program,
     e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
     we set DPL==3, meaning that user programs are allowed to
     invoke them via these instructions. */
  intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
  intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
  intr_register_int (5, 3, INTR_ON, kill,
                     "#BR BOUND Range Exceeded Exception");

  /* These exceptions have DPL==0, preventing user processes from
     invoking them via the INT instruction.  They can still be
     caused indirectly, e.g. #DE can be caused by dividing by
     0.  */
  intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
  intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
  intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
  intr_register_int (7, 0, INTR_ON, kill,
                     "#NM Device Not Available Exception");
  intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
  intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
  intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
  intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
  intr_register_int (19, 0, INTR_ON, kill,
                     "#XF SIMD Floating-Point Exception");

  /* Most exceptions can be handled with interrupts turned on.
     We need to disable interrupts for page faults because the
     fault address is stored in CR2 and needs to be preserved. */
  intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/** Prints exception statistics. */
void
exception_print_stats (void) 
{
  printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/** Handler for an exception (probably) caused by a user process. */
static void
kill (struct intr_frame *f) 
{
  /* This interrupt is one (probably) caused by a user process.
     For example, the process might have tried to access unmapped
     virtual memory (a page fault).  For now, we simply kill the
     user process.  Later, we'll want to handle page faults in
     the kernel.  Real Unix-like operating systems pass most
     exceptions back to the process via signals, but we don't
     implement them. */
     
  /* The interrupt frame's code segment value tells us where the
     exception originated. */
  switch (f->cs)
    {
    case SEL_UCSEG:
      /* User's code segment, so it's a user exception, as we
         expected.  Kill the user process.  */
      printf ("%s: dying due to interrupt %#04x (%s).\n",
              thread_name (), f->vec_no, intr_name (f->vec_no));
      intr_dump_frame (f);
      thread_exit (); 

    case SEL_KCSEG:
      /* Kernel's code segment, which indicates a kernel bug.
         Kernel code shouldn't throw exceptions.  (Page faults
         may cause kernel exceptions--but they shouldn't arrive
         here.)  Panic the kernel to make the point.  */
      intr_dump_frame (f);
      PANIC ("Kernel bug - unexpected interrupt in kernel"); 

    default:
      /* Some other code segment?  Shouldn't happen.  Panic the
         kernel. */
      printf ("Interrupt %#04x (%s) in unknown segment %04x\n",
             f->vec_no, intr_name (f->vec_no), f->cs);
      thread_exit ();
    }
}

/** Page fault handler.  Implements virtual memory by loading pages
   on demand from the supplemental page table.

   At entry, the address that faulted is in CR2 (Control Register
   2) and information about the fault, formatted as described in
   the PF_* macros in exception.h, is in F's error_code member.  The
   example code here shows how to parse that information.  You
   can find more information about both of these in the
   description of "Interrupt 14--Page Fault Exception (#PF)" in
   [IA32-v3a] section 5.15 "Exception and Interrupt Reference". */
static void
page_fault (struct intr_frame *f) 
{
  bool not_present;  /**< True: not-present page, false: writing r/o page. */
  bool write;        /**< True: access was write, false: access was read. */
  bool user;         /**< True: access by user, false: access by kernel. */
  void *fault_addr;  /**< Fault address. */

  /* Obtain faulting address, the virtual address that was
     accessed to cause the fault.  It may point to code or to
     data.  It is not necessarily the address of the instruction
     that caused the fault (that's f->eip).
     See [IA32-v2a] "MOV--Move to/from Control Registers" and
     [IA32-v3a] 5.15 "Interrupt 14--Page Fault Exception
     (#PF)". */
  asm ("movl %%cr2, %0" : "=r" (fault_addr));

  /* Turn interrupts back on (they were only off so that we could
     be assured of reading CR2 before it changed). */
  intr_enable ();

  /* Count page faults. */
  page_fault_cnt++;

  /* Determine cause. */
  not_present = (f->error_code & PF_P) == 0;
  write = (f->error_code & PF_W) != 0;
  user = (f->error_code & PF_U) != 0;

  /* Debug info for page fault */
  // printf("PAGE_FAULT: tid=%d addr=%p user=%d write=%d present=%d esp=%p\n",
  //       thread_current()->tid, fault_addr, user, write, !not_present, 
  //       user ? (void*)f->esp : thread_current()->user_esp);

  /* Helper macro for consistent error handling */
  #define HANDLE_FAULT_ERROR() do { \
    if (user) { \
      kill(f); \
    } else { \
      thread_exit(); \
    } \
  } while (0)

  /* Validate fault address is in valid user space */
  if (!is_user_vaddr(fault_addr) || fault_addr < (void *) 0x08048000) {
    HANDLE_FAULT_ERROR();
    return;
  }

  /* Ensure we have a valid user process with page directory */
  struct thread *cur = thread_current();
  if (cur->pagedir == NULL) {
    if (!user) {
      printf("%s: dying due to kernel access without page directory.\n",
             thread_name());
    }
    HANDLE_FAULT_ERROR();
    return;
  }

  /* Attempt to resolve the page fault */
  struct spt_entry *page_entry = spt_lookup(&cur->spt, fault_addr);
  
  /* If page not found, try stack growth */
  if (page_entry == NULL) {
    void *effective_esp = user ? (void *) f->esp : cur->user_esp;
    
    if (!handle_stack_growth(fault_addr, effective_esp, user)) {
      HANDLE_FAULT_ERROR();
      return;
    }
    
    /* Stack growth succeeded - retrieve the new page entry */
    page_entry = spt_lookup(&cur->spt, fault_addr);
    ASSERT(page_entry != NULL);
  }

  /* Verify write permissions */
  if (write && !page_entry->writable) {
    HANDLE_FAULT_ERROR();
    return;
  }

  /* Load the page based on its current status */
  bool load_success = false;
  
  switch (page_entry->status) {
    case PAGE_NOT_LOADED:
    case PAGE_SWAPPED:
      load_success = spt_load_page(page_entry);
      break;
      
    case PAGE_LOADED:
      if (not_present) {
        /* Inconsistent state - page marked loaded but not present */
        HANDLE_FAULT_ERROR();
        return;
      }
      load_success = true; /* Already loaded, nothing to do */
      break;
      
    default:
      /* Unknown page status */
      HANDLE_FAULT_ERROR();
      return;
  }

  /* Handle load failure */
  if (!load_success) {
    HANDLE_FAULT_ERROR();
    return;
  }
  
  #undef HANDLE_FAULT_ERROR
}

/** Handles stack growth by validating the access and creating new stack pages.
   
   This function implements several critical checks:
   1. Uses proper ESP (saved user ESP for kernel faults)
   2. Enforces 32-byte proximity heuristic for PUSHA instruction support
   3. Ensures contiguous stack growth (no gaps)
   4. Enforces 8MB stack size limit
   5. Validates fault address is in valid stack region
   
   Returns true if stack growth was successful, false otherwise. */
static bool handle_stack_growth (void *fault_addr, void *esp, bool user_fault)
{
  struct thread *cur = thread_current();
  
  /* Get effective ESP - use saved user ESP for kernel faults */
  void *effective_esp = user_fault ? esp : cur->user_esp;
  
  /* Validate effective ESP is reasonable */
  if (effective_esp == NULL || !is_user_vaddr(effective_esp)) {
    return false;
  }
  
  /* Check if fault address is below current stack bottom */
  if (fault_addr >= cur->stack_bottom) {
    return false;  /* Not below current stack */
  }
  
  /* Enforce 32-byte proximity heuristic for PUSHA instruction support */
  if ((uint8_t *) fault_addr < (uint8_t *) effective_esp - 32) {
    return false;  /* Too far below ESP - not a valid stack access */
  }
  
  /* Calculate how much the stack would grow */
  void *new_stack_page = pg_round_down(fault_addr);
  void *current_stack_bottom = (uint8_t *) cur->stack_bottom - cur->stack_size;
  
  /* Check that new page is below current stack */
  if (new_stack_page >= current_stack_bottom) {
    return false;  /* Not a stack growth - overlaps existing stack */
  }
  
  /* Calculate how many pages we need to grow */
  size_t pages_to_grow = ((uint8_t *)current_stack_bottom - (uint8_t *)new_stack_page) / PGSIZE;
  size_t new_stack_size = cur->stack_size + pages_to_grow * PGSIZE;
  
  /* Check 8MB stack limit */
  if (new_stack_size > MAX_STACK_SIZE) {
    return false;  /* Stack would exceed 8MB limit */
  }
  
  /* Check absolute lower bound (should be well above code/data) */
  if (new_stack_page < (void *) (PHYS_BASE - MAX_STACK_SIZE)) {
    return false;  /* Below valid stack region */
  }
  
  /* Create SPT entries for all pages we need to grow */
  void *page_addr = current_stack_bottom - PGSIZE;  /* Start with first new page */
  while (page_addr >= new_stack_page) {
    struct spt_entry *entry = spt_create_entry(page_addr, PAGE_STACK, true);
    if (entry == NULL) {
      return false;  /* Memory allocation failed */
    }
    
    if (!spt_insert(&cur->spt, entry)) {
      free(entry);
      return false;  /* SPT insertion failed */
    }
    
    page_addr -= PGSIZE;  /* Move to next page down */
  }
  
  /* Update stack metadata */
  cur->stack_size = new_stack_size;
  
  return true;  /* Stack growth successful */
}

