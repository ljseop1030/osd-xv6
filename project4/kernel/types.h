typedef unsigned int   uint;
typedef unsigned short ushort;
typedef unsigned char  uchar;

typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int  uint32;
typedef unsigned long uint64;

typedef uint64 pde_t;

/* AI assisted - proj4
    According to the README, struct page should be placed in types.h.
    However, since pagetable_t is defined in riscv.h and types.h is included beforehand,
    this could lead to an unknown type name 'pagetable_t' error.
    We proceeded as instructed by the README for now;
    if it triggers an error, we made it to resolve it by changing the field type to
    uint64 *pagetable; , which is identical to pagetable_t.
*/
// proj4 README - requires 
struct page {
    struct page *next, *prev;
    pagetable_t  pagetable;   // if error; uint64 *pagetable;
    uint64       vaddr;
};