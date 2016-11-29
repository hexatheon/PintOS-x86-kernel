#ifndef VM_SWAP_H
#define VM_SWAP_H


// some decleration here
//
void swap_init (void);

bool swap_in (struct page *p);

bool swap_out (struct page *p);

bool swap_free(struct page *p);
#endif
