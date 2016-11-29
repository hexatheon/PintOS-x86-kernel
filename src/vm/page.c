#include "vm/page.h"
#include "threads/malloc.h"
#include "vm/frame.h"
#include "threads/thread.h"
#include <stdio.h>
#include "userprog/pagedir.h"
// #include "filesys/file.h"
#include <string.h>
#include "threads/pte.h"
#include "userprog/syscall.h"
#include "vm/swap.h"

#define STACK_MAX (1024 * 1024)
#define PUSH_MAX 32

#define HASH_TO_P(HASH_ELEM) \
	((struct page*) hash_entry(HASH_ELEM, struct page, hash_elem))

static void destroy_page (struct hash_elem *p_, void *aux UNUSED);
static struct page *page_for_addr (const void *address);
static bool page_internal_lock(struct page* p, bool will_write);
static void page_internal_unlock(struct page* p);

struct hash* page_supp_table_create(void){
	struct hash* ret = malloc(sizeof(struct hash));
	if (!ret){
		return NULL;
	}
	return (hash_init(ret, &page_hash, &page_less, NULL)) ? ret : NULL;
}

static void destroy_page (struct hash_elem *p_, void *aux UNUSED)  {
	//cannot assume the lock was already held because this is used as
	//an destructor for hash table
	struct page* p = HASH_TO_P(p_);
	// show_page(p);
	struct frame* f = p->frame;
	frame_pin(f);
	swap_free(p);
	if (f){
		frame_unlock(f);
	}
	free(p);
    frame_unpin(f);
}

void page_exit (void)  {

	struct thread *t = thread_current();
	//printf("%d-%s: Starting page_exit()\n", t->tid, __func__);
	struct hash* table = t->pages;
	//printf("%d-%s: table = %p\n", t->tid, __func__, table);
	hash_destroy(table, &destroy_page);
	//printf("%d-%s: Starting page_exit()\n", t->tid, __func__);
	free(table);
	t->pages = NULL;
}

//only malloc when it is not presend in the supp_pt
static struct page *page_for_addr (const void *address) {
	struct thread* t = thread_current();
	struct hash* table = t->pages;
	struct page* ret = page_lookup(table, address);
	if (ret){
		return NULL;
	}
	ret = malloc(sizeof(struct page));
	//check NULL
	ret->addr = address;	
	ret->thread = t;
	ret->frame = NULL;
	ret->file = NULL;
	ret->file_bytes = 0;
	ret->file_offset = 0;
	ret->sector = 0xffffffff;

	//!! by default it is writing back to file
	// ret->private = false;
	ret->private = true;

	lock_init(&ret->dummy_lock);

	ret->data = false;
	return ret;
}
static bool do_page_in (struct page *p) {
	//do not lock. Do locking outside
	if (!p){
		//////printf("%d-%s: page pointer == NULL\n", thread_current()->tid, __func__);
		return false;
	}
	frame_lock(p);
	if (p->frame == NULL){
		return false;
	}
	//////printf("%d-%s: Locked a frame\n", thread_current()->tid, __func__);
	return true;
}

//does not install pages
//ALWAYS PINNED!
bool page_in (void *fault_addr) {
	struct thread* t = thread_current();
	struct hash* table = t->pages;
	fault_addr = pg_round_down(fault_addr);
	struct page* p = page_lookup(table, fault_addr);

	//printf("%d-%s: Page_in addr == %p, page addr: %p\n", thread_current()->tid,__func__, fault_addr, p->addr);

	page_internal_lock(p, true);

	bool ret = false;

	bool got_a_frame = do_page_in(p);
	if (!got_a_frame){
  		//////printf("%d-%s: Did not get a frame\n", thread_current()->tid, __func__);
		goto done;
	}
	void* kpage = p->frame->base;
	if (p->sector != 0xffffffff){
		// //printf("%d-%s: Calling swap %d\n", thread_current()->tid,__func__, p->sector);
		swap_in(p);
		// show_page(p);
	} else if (p->file){
		//from file
		sys_file_seek(p->file, p->file_offset);
		if (sys_file_read(p->file, kpage, p->file_bytes/*, p->file_offset*/) != (int) p->file_bytes){
			// page_deallocate(fault_addr);
  			//////printf("%d-%s: Failed to read a page\n", thread_current()->tid, __func__);
			goto done;
		}
		memset (kpage + p->file_bytes, 0, PGSIZE - p->file_bytes);
	} else {
		//zero page
		memset (kpage, 0, PGSIZE);
	}
	//success
	ret = true;
done:	
	// ////printf("%d-%s: Finished page_in()?\n", thread_current()->tid, __func__);
	page_internal_unlock(p);
	// ////printf("%d-%s: Finished page_in()\n", thread_current()->tid, __func__);
	return ret;
}

bool valid_stack_access(void* stackptr, void* fault_addr){
	bool ret = (((uintptr_t) stackptr) - PUSH_MAX <= (unsigned int) fault_addr);
	ret = ret && (((uintptr_t) fault_addr) < (uintptr_t) PHYS_BASE);
	ret = ret && ((uintptr_t) (PHYS_BASE - pg_round_down(fault_addr)) <= STACK_MAX);
	//////printf("%d-%s: implied stack size %u v.s. max %u\n",thread_current()->tid, __func__, PHYS_BASE - pg_round_down(fault_addr), STACK_MAX);
	return ret;
}

//NEED TO UNLOCK THREAD FOR SYMMETRY!
bool page_out (struct page *p) {
	ASSERT(p->frame != NULL);
	//printf("%d-%s: In here?\n", thread_current()->tid,__func__);
	pagedir_clear_page(p->thread->pagedir, p->addr);
	page_internal_lock(p, true);

	//printf("%d-%s: In here\n", thread_current()->tid,__func__);
	if (pagedir_is_dirty(p->thread->pagedir, p->addr)){
		//need to write somewhere
		if (p->file && !p->private){
			ASSERT(p->read_only == false);
			//printf("%d-%s: writing to file %x\n", thread_current()->tid,__func__,*(unsigned int*)p->frame->base);
			sys_file_write_at(p->file, p->frame->base, p->file_bytes, p->file_offset);
		} else {
			//to swap
			//printf("%d-%s: Calling swap %x\n", thread_current()->tid,__func__, *(unsigned int*)p->frame->base);
			swap_out(p);
			p->private = true;
			//printf("%d-%s: Swapped done. p->sector == %d\n", thread_current()->tid,__func__, p->sector);
		}
	} else {
		if (p->data){
			//printf("%d-%s: Calling swap %x\n", thread_current()->tid,__func__, *(unsigned int*)p->frame->base);
			show_page(p);
			swap_out(p);
		} else if (p->file){
			//printf("%d-%s: We can read this page from file\n", thread_current()->tid,__func__);
		} else {
			show_page(p);
		}
	}
	//unlock this page
	frame_unlock(p->frame);
	//p->frame is lost.
	page_internal_unlock(p);
	return true;
}
// bool page_accessed_recently (struct page *p) {}

//already inserted the page. Not sure if we should do this
//should probably return NULL if vaddr is not available
//havn't returned anything, so no need to lock page
struct page * page_allocate (void *vaddr, bool read_only) {
	//////printf("%d-%s: Allocating a page\n",thread_current()->tid, __func__);
  	ASSERT (pg_ofs (vaddr) == 0);
	struct page* ret = page_for_addr(vaddr);
	if (!ret){
		return NULL;
	}
	ret->read_only = read_only;
	if (hash_insert(ret->thread->pages, &ret->hash_elem)){
		//if this page (addr) already exists
		destroy_page(&ret->hash_elem, NULL);
		ret = NULL;
	}
	//////printf("%d-%s: Done. Page pointer %p\n", thread_current()->tid,__func__, ret->addr);
	return ret;
}

void page_deallocate (void *vaddr) {
	ASSERT (pg_ofs (vaddr) == 0);
	struct thread* t = thread_current();
	struct hash* table = t->pages;
	struct page* p = page_lookup(table, vaddr);
	ASSERT(p != NULL);

	bool already_held = lock_held_by_current_thread(&p->dummy_lock);
	if (!already_held){
		// page_internal_lock(p, true);
	}

	struct hash_elem *res = hash_delete(table, &p->hash_elem);
	ASSERT(res != NULL);

	pagedir_clear_page(t->pagedir, vaddr);
	destroy_page(res, NULL);

	if (!already_held){
		// page_internal_unlock(p);
	}
} 


struct page* page_lookup(struct hash* table, const void *addr){
	//HERE
	struct hash_elem* ret = NULL;
	struct page local_help;
	//local_help.addr = (void*) (((unsigned int)addr) & PTE_ADDR);
	local_help.addr = pg_round_down(addr);
	// local_help.addr = addr;
	ret = hash_find(table, &local_help.hash_elem);
	return ret != NULL ? HASH_TO_P(ret) : NULL;
	// return ret != NULL ? hash_entry (ret, struct page, hash_elem) : NULL;
}
unsigned page_hash (const struct hash_elem *e, void *aux UNUSED) {
	ASSERT(e != NULL);
	const struct page* p = hash_entry(e, struct page, hash_elem);
	return hash_bytes (&p->addr, sizeof p->addr);
}
bool page_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED) {
	const struct page* p1 = HASH_TO_P(a_);
	const struct page* p2 = HASH_TO_P(b_);
	return (p1->addr < p2->addr);
}

static bool page_internal_lock(struct page* p, bool will_write){
	//dummy stuff
	////printf("%d-%s: Trying to get a lock\n", thread_current()->tid, __func__);
	lock_acquire(&p->dummy_lock);
	////printf("%d-%s: Got the lock\n", thread_current()->tid, __func__);
	return true;
}
static void page_internal_unlock(struct page* p){
	//dummy stuff
	lock_release(&p->dummy_lock);
}
bool page_lock (const void *addr, bool will_write) {
	struct page* p = page_lookup(thread_current()->pages, addr);
	ASSERT(p != NULL);
	return page_internal_lock(p, will_write);
}
void page_unlock (const void *addr) {
	struct page* p = page_lookup(thread_current()->pages, addr);
	ASSERT(p != NULL);
	page_internal_unlock(p);
}


void show_page(struct page* p){
	int tid = thread_current()->tid;
	if (!p){
		//printf("%d-%s: ========no page to show =======\n", tid, __func__);
		return;
	}
    //printf("%d-%s: ========showing page (addr = %p) =======\n", tid, __func__, p->addr);
    //printf("%d-%s: read_only? %d\ntid: %d\nsector: %d\nprivate? %d\nfile? %p\nfile_offset: %d\nfile_bytes: %d\ndirty? %d\n\n", tid, __func__, (p->read_only ? 1 : 0), p->thread->tid, p->sector, (p->private ? 1 : 0), p->file, p->file_offset, p->file_bytes, pagedir_is_dirty(p->thread->pagedir, p->addr));
}
