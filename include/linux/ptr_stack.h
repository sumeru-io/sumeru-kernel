/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Definitions for the 'struct ptr_stack' datastructure.
 *
 * Author: Minhu Wang <minhuw@acm.org>
 *
 * This is a limited-size LIFO (Last-In, First-Out) stack maintaining
 * pointers, potentially used by multiple CPUs concurrently,
 * protected by a single spinlock. Includes batch push operations.
 */

 #ifndef _LINUX_PTR_STACK_H
 #define _LINUX_PTR_STACK_H 1
 
 #ifdef __KERNEL__
 #include <linux/spinlock.h>
 #include <linux/cache.h>
 #include <linux/types.h>
 #include <linux/compiler.h>
 #include <linux/slab.h>
 #include <linux/mm.h>
 #include <asm/errno.h>
 #include <linux/minmax.h> /* For min() */
 #include <linux/string.h> /* For memcpy() */
 #endif
 
 struct ptr_stack {
	 int top;
	 spinlock_t lock;
	 /* Read-only by concurrent accesses after init/resize */
	 int size ____cacheline_aligned_in_smp; /* Max entries in stack */
	 void **queue;
 };
 
 /*---------------------------------------------------------------------------*/
 /* Full Checks                                                               */
 /*---------------------------------------------------------------------------*/
 
 /* Note: callers invoking this in a loop must use a compiler barrier,
  * for example cpu_relax().
  * Callers must hold the stack lock.
  */
 static inline bool __ptr_stack_full(struct ptr_stack *s)
 {
	 /* Stack is full if the next free slot index is equal to or greater than size */
	 return s->top >= s->size;
 }
 
 static inline bool ptr_stack_full(struct ptr_stack *s)
 {
	 bool ret;
 
	 spin_lock(&s->lock);
	 ret = __ptr_stack_full(s);
	 spin_unlock(&s->lock);
 
	 return ret;
 }
 
 static inline bool ptr_stack_full_irq(struct ptr_stack *s)
 {
	 bool ret;
 
	 spin_lock_irq(&s->lock);
	 ret = __ptr_stack_full(s);
	 spin_unlock_irq(&s->lock);
 
	 return ret;
 }
 
 static inline bool ptr_stack_full_any(struct ptr_stack *s)
 {
	 unsigned long flags;
	 bool ret;
 
	 spin_lock_irqsave(&s->lock, flags);
	 ret = __ptr_stack_full(s);
	 spin_unlock_irqrestore(&s->lock, flags);
 
	 return ret;
 }
 
 static inline bool ptr_stack_full_bh(struct ptr_stack *s)
 {
	 bool ret;
 
	 spin_lock_bh(&s->lock);
	 ret = __ptr_stack_full(s);
	 spin_unlock_bh(&s->lock);
 
	 return ret;
 }
 
 /*---------------------------------------------------------------------------*/
 /* Single Push Operations                                                    */
 /*---------------------------------------------------------------------------*/
 
 /* Note: callers invoking this in a loop must use a compiler barrier,
  * for example cpu_relax(). Callers must hold the stack lock.
  * Callers are responsible for making sure pointer that is being pushed
  * points to a valid data.
  */
 static inline int __ptr_stack_push(struct ptr_stack *s, void *ptr)
 {
	 /* Check if size is valid and stack is not full */
	 if (unlikely(!s->size) || s->top >= s->size)
		 return -ENOSPC; /* Or -EOVERFLOW maybe? Sticking to ENOSPC */
 
	 /* Make sure the pointer we are storing points to valid data. */
	 /* Pairs with potential dependency ordering in __ptr_stack_pop/__ptr_stack_peek. */
	 smp_wmb(); /* Write memory barrier before writing pointer */
 
	 WRITE_ONCE(s->queue[s->top], ptr);
	 s->top++; /* Increment top to point to the next free slot */
 
	 return 0;
 }
 
 static inline int ptr_stack_push(struct ptr_stack *s, void *ptr)
 {
	 int ret;
 
	 spin_lock(&s->lock);
	 ret = __ptr_stack_push(s, ptr);
	 spin_unlock(&s->lock);
 
	 return ret;
 }
 
 static inline int ptr_stack_push_irq(struct ptr_stack *s, void *ptr)
 {
	 int ret;
 
	 spin_lock_irq(&s->lock);
	 ret = __ptr_stack_push(s, ptr);
	 spin_unlock_irq(&s->lock);
 
	 return ret;
 }
 
 static inline int ptr_stack_push_any(struct ptr_stack *s, void *ptr)
 {
	 unsigned long flags;
	 int ret;
 
	 spin_lock_irqsave(&s->lock, flags);
	 ret = __ptr_stack_push(s, ptr);
	 spin_unlock_irqrestore(&s->lock, flags);
 
	 return ret;
 }
 
 static inline int ptr_stack_push_bh(struct ptr_stack *s, void *ptr)
 {
	 int ret;
 
	 spin_lock_bh(&s->lock);
	 ret = __ptr_stack_push(s, ptr);
	 spin_unlock_bh(&s->lock);
 
	 return ret;
 }
 
 /*---------------------------------------------------------------------------*/
 /* Batch Push Operations                                                     */
 /*---------------------------------------------------------------------------*/
 
 /**
  * __ptr_stack_push_batch - Push multiple pointers onto the stack.
  * @s: The ptr_stack structure.
  * @array: Array of pointers to push onto the stack.
  * @n: Number of pointers in the array to attempt to push.
  *
  * Pushes up to 'n' pointers from 'array' onto the stack 's'.
  * Callers must hold the stack lock.
  * Callers are responsible for ensuring the pointers in 'array' are valid.
  *
  * Returns: The number of pointers successfully pushed (0 <= count <= n).
  * Returns 0 if n <= 0, the array is NULL, or the stack is initially full.
  */
 static inline int __ptr_stack_push_batch(struct ptr_stack *s, void **array, int n)
 {
	 int available, count;
 
	 /* Basic validation */
	 if (unlikely(n <= 0 || !array))
		 return 0;
 
	 /* Check if size is valid and stack is not already full */
	 if (unlikely(!s->size) || s->top >= s->size)
		 return 0; /* Cannot push any more */
 
	 /* Calculate how many pointers we can actually push */
	 available = s->size - s->top;
	 count = min(n, available);
 
	 /* If no space available or nothing to push */
	 if (count == 0)
	     return 0;
 
	 /* Ensure data pointed to by array elements is visible before pointers are */
	 smp_wmb(); /* Write memory barrier before writing pointers */
 
	 /* Copy the pointers to the stack queue */
	 memcpy(&s->queue[s->top], array, count * sizeof(void *));
 
	 /* Update the top index */
	 s->top += count;
 
	 return count; /* Return the number of pointers actually pushed */
 }
 
 /**
  * ptr_stack_push_batch - Push multiple pointers onto the stack (locked).
  * @s: The ptr_stack structure.
  * @array: Array of pointers to push onto the stack.
  * @n: Number of pointers in the array to attempt to push.
  *
  * Acquires the stack lock, pushes up to 'n' pointers, releases the lock.
  *
  * Returns: The number of pointers successfully pushed (0 <= count <= n).
  */
 static inline int ptr_stack_push_batch(struct ptr_stack *s, void **array, int n)
 {
	 int ret;
 
	 spin_lock(&s->lock);
	 ret = __ptr_stack_push_batch(s, array, n);
	 spin_unlock(&s->lock);
 
	 return ret;
 }
 
 /**
  * ptr_stack_push_batch_irq - Push multiple pointers onto the stack (IRQ lock).
  * @s: The ptr_stack structure.
  * @array: Array of pointers to push onto the stack.
  * @n: Number of pointers in the array to attempt to push.
  *
  * Disables local interrupts, acquires lock, pushes up to 'n' pointers,
  * releases lock, enables local interrupts.
  *
  * Returns: The number of pointers successfully pushed (0 <= count <= n).
  */
 static inline int ptr_stack_push_batch_irq(struct ptr_stack *s, void **array, int n)
 {
	 int ret;
 
	 spin_lock_irq(&s->lock);
	 ret = __ptr_stack_push_batch(s, array, n);
	 spin_unlock_irq(&s->lock);
 
	 return ret;
 }
 
 /**
  * ptr_stack_push_batch_any - Push multiple pointers onto the stack (IRQ save lock).
  * @s: The ptr_stack structure.
  * @array: Array of pointers to push onto the stack.
  * @n: Number of pointers in the array to attempt to push.
  *
  * Saves IRQ flags, disables local interrupts, acquires lock, pushes up to 'n' pointers,
  * releases lock, restores IRQ flags.
  *
  * Returns: The number of pointers successfully pushed (0 <= count <= n).
  */
 static inline int ptr_stack_push_batch_any(struct ptr_stack *s, void **array, int n)
 {
	 unsigned long flags;
	 int ret;
 
	 spin_lock_irqsave(&s->lock, flags);
	 ret = __ptr_stack_push_batch(s, array, n);
	 spin_unlock_irqrestore(&s->lock, flags);
 
	 return ret;
 }
 
 /**
  * ptr_stack_push_batch_bh - Push multiple pointers onto the stack (BH lock).
  * @s: The ptr_stack structure.
  * @array: Array of pointers to push onto the stack.
  * @n: Number of pointers in the array to attempt to push.
  *
  * Disables local bottom halves, acquires lock, pushes up to 'n' pointers,
  * releases lock, enables local bottom halves.
  *
  * Returns: The number of pointers successfully pushed (0 <= count <= n).
  */
 static inline int ptr_stack_push_batch_bh(struct ptr_stack *s, void **array, int n)
 {
	 int ret;
 
	 spin_lock_bh(&s->lock);
	 ret = __ptr_stack_push_batch(s, array, n);
	 spin_unlock_bh(&s->lock);
 
	 return ret;
 }
 
 
 /*---------------------------------------------------------------------------*/
 /* Peek Operation                                                            */
 /*---------------------------------------------------------------------------*/
 
 /* Internal function to peek at the top element without removing it.
  * Callers must hold the stack lock.
  */
 static inline void *__ptr_stack_peek(struct ptr_stack *s)
 {
	 /* Check if size is valid and stack is not empty */
	 if (likely(s->size) && s->top > 0)
		 /* Read the element at the current top - 1 */
		 return READ_ONCE(s->queue[s->top - 1]);
	 return NULL;
 }
 
 /*---------------------------------------------------------------------------*/
 /* Empty Checks                                                              */
 /*---------------------------------------------------------------------------*/
 
 /*
  * Test stack empty status without taking any locks.
  *
  * NB: This is only safe to call if stack is never resized AND
  * you can tolerate stale results (e.g., another CPU might push/pop
  * immediately after this check).
  *
  * Note: callers invoking this in a loop must use a compiler barrier,
  * for example cpu_relax().
  */
 static inline bool __ptr_stack_empty_unsafe(struct ptr_stack *s)
 {
	 /* Check if size is valid and top is at 0 */
	 return !s->size || READ_ONCE(s->top) == 0;
 }
 
 /* Internal function to check if the stack is empty.
  * Callers must hold the stack lock.
  */
 static inline bool __ptr_stack_empty(struct ptr_stack *s)
 {
	 /* Stack is empty if top index is 0 */
	 return s->top == 0;
 }
 
 static inline bool ptr_stack_empty(struct ptr_stack *s)
 {
	 bool ret;
 
	 spin_lock(&s->lock);
	 ret = __ptr_stack_empty(s);
	 spin_unlock(&s->lock);
 
	 return ret;
 }
 
 static inline bool ptr_stack_empty_irq(struct ptr_stack *s)
 {
	 bool ret;
 
	 spin_lock_irq(&s->lock);
	 ret = __ptr_stack_empty(s);
	 spin_unlock_irq(&s->lock);
 
	 return ret;
 }
 
 static inline bool ptr_stack_empty_any(struct ptr_stack *s)
 {
	 unsigned long flags;
	 bool ret;
 
	 spin_lock_irqsave(&s->lock, flags);
	 ret = __ptr_stack_empty(s);
	 spin_unlock_irqrestore(&s->lock, flags);
 
	 return ret;
 }
 
 static inline bool ptr_stack_empty_bh(struct ptr_stack *s)
 {
	 bool ret;
 
	 spin_lock_bh(&s->lock);
	 ret = __ptr_stack_empty(s);
	 spin_unlock_bh(&s->lock);
 
	 return ret;
 }
 
 /*---------------------------------------------------------------------------*/
 /* Single Pop Operation                                                      */
 /*---------------------------------------------------------------------------*/
 
 /* Internal function to remove and return the top element.
  * Callers must hold the stack lock.
  */
 static inline void *__ptr_stack_pop(struct ptr_stack *s)
 {
	 void *ptr;
 
	 /* Check if size is valid and stack is not empty */
	 if (unlikely(!s->size) || s->top == 0)
		 return NULL;
 
	 /* Decrement top first to point to the element to pop */
	 s->top--;
	 /* Read the pointer */
	 ptr = READ_ONCE(s->queue[s->top]);
 
	 /* Optional: Clear the slot to avoid dangling references */
	 /* WRITE_ONCE(s->queue[s->top], NULL); */
 
	 return ptr;
 }
 
 static inline void *ptr_stack_pop(struct ptr_stack *s)
 {
	 void *ptr;
 
	 spin_lock(&s->lock);
	 ptr = __ptr_stack_pop(s);
	 spin_unlock(&s->lock);
 
	 return ptr;
 }
 
 static inline void *ptr_stack_pop_irq(struct ptr_stack *s)
 {
	 void *ptr;
 
	 spin_lock_irq(&s->lock);
	 ptr = __ptr_stack_pop(s);
	 spin_unlock_irq(&s->lock);
 
	 return ptr;
 }
 
 static inline void *ptr_stack_pop_any(struct ptr_stack *s)
 {
	 unsigned long flags;
	 void *ptr;
 
	 spin_lock_irqsave(&s->lock, flags);
	 ptr = __ptr_stack_pop(s);
	 spin_unlock_irqrestore(&s->lock, flags);
 
	 return ptr;
 }
 
 static inline void *ptr_stack_pop_bh(struct ptr_stack *s)
 {
	 void *ptr;
 
	 spin_lock_bh(&s->lock);
	 ptr = __ptr_stack_pop(s);
	 spin_unlock_bh(&s->lock);
 
	 return ptr;
 }
 
 /*---------------------------------------------------------------------------*/
 /* Initialization, Resize, Cleanup                                         */
 /*---------------------------------------------------------------------------*/
 
 /* Not all gfp_t flags (besides GFP_KERNEL) are allowed. See
  * documentation for kvmalloc_array for which of them are legal.
  */
 static inline void **__ptr_stack_init_queue_alloc_noprof(unsigned int size, gfp_t gfp)
 {
	 /* Use kvmalloc_array for potentially large allocations */
	 if (size > KMALLOC_MAX_SIZE / sizeof(void *))
		 return kvmalloc_array(size, sizeof(void *), gfp | __GFP_ZERO);
	 else
		 /* Use kmalloc_array for smaller allocations */
		 return kmalloc_array(size, sizeof(void *), gfp | __GFP_ZERO);
	 /* Note: ptr_ring used kvmalloc_array_noprof. Adjust if profiling is a concern. */
 }
 
 static inline int ptr_stack_init_noprof(struct ptr_stack *s, int size, gfp_t gfp)
 {
	 /* Initialize fields to known state */
	 s->queue = NULL;
	 s->size = 0;
	 s->top = 0;
	 spin_lock_init(&s->lock);
 
	 if (size <= 0) /* Allow size 0, but queue will be NULL */
	      return 0;
 
	 s->queue = __ptr_stack_init_queue_alloc_noprof(size, gfp);
	 if (!s->queue)
		 return -ENOMEM;
 
	 s->size = size;
	 /* s->top is already 0 */
 
	 return 0;
 }
 /* Define the alloc_hooks macro if needed, otherwise just alias */
 #ifndef alloc_hooks
 #define alloc_hooks(call) call
 #endif
 #define ptr_stack_init(...)      alloc_hooks(ptr_stack_init_noprof(__VA_ARGS__))
 
 
 /* Resize the stack.
  * Existing elements up to the smaller of the old/new size are preserved.
  * If shrinking, elements beyond the new size are discarded (and destroyed if 'destroy' is provided).
  * Note: This is potentially a slow operation.
  */
 static inline int ptr_stack_resize_noprof(struct ptr_stack *s, int new_size, gfp_t gfp,
					   void (*destroy)(void *))
 {
	 unsigned long flags;
	 void **new_queue = NULL;
	 void **old_queue;
	 int old_top;
	 int i, copy_count;
 
	 if (new_size < 0)
		 return -EINVAL;
 
	 if (new_size > 0) {
		 new_queue = __ptr_stack_init_queue_alloc_noprof(new_size, gfp);
		 if (!new_queue)
			 return -ENOMEM;
	 } else {
		 new_queue = NULL; /* Resizing to 0 */
	 }
 
 
	 spin_lock_irqsave(&s->lock, flags);
 
	 old_queue = s->queue;
	 old_top = s->top;
 
	 /* Determine how many elements to copy */
	 copy_count = min(old_top, new_size);
 
	 /* Copy elements from old queue to new queue */
	 if (copy_count > 0 && new_queue && old_queue) {
		 memcpy(new_queue, old_queue, copy_count * sizeof(void *));
	 }
 
	 /* If shrinking and a destroy function is provided, destroy discarded elements */
	 if (destroy && new_size < old_top) {
		 for (i = new_size; i < old_top; i++) {
			 /* Ensure we don't access freed memory if old_queue was NULL */
			 if (old_queue && old_queue[i])
			    destroy(old_queue[i]);
		 }
	 }
 
	 /* Update stack structure */
	 s->queue = new_queue;
	 s->size = new_size;
	 s->top = copy_count; /* New top is the number of copied elements */
 
	 spin_unlock_irqrestore(&s->lock, flags);
 
	 /* Free the old queue */
	 kvfree(old_queue); /* Use kvfree as allocation could be kvmalloc or kmalloc */
 
	 return 0;
 }
 #define ptr_stack_resize(...)    alloc_hooks(ptr_stack_resize_noprof(__VA_ARGS__))
 
 /* Clean up the stack, freeing the queue and optionally destroying remaining elements. */
 static inline void ptr_stack_cleanup(struct ptr_stack *s, void (*destroy)(void *))
 {
	 void *ptr;
 
	 if (destroy) {
		 /* Keep popping and destroying until empty */
		 /* Note: This acquires/releases lock repeatedly. Could optimize if needed */
		 /* by taking lock once, looping through s->top, then releasing. */
		 while ((ptr = ptr_stack_pop(s))) {
			 destroy(ptr);
		 }
	 }
 
	 /* Free the queue array itself */
	 kvfree(s->queue); /* Use kvfree to match allocation strategy */
 
	 /* Reset fields (optional but good practice) */
	 s->queue = NULL;
	 s->size = 0;
	 s->top = 0;
	 /* Lock is already initialized, no need to destroy */
}
 
#endif /* _LINUX_PTR_STACK_H  */
 