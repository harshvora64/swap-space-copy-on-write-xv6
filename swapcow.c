#include "types.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "buf.h"

static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}























// RMAP

struct{
  struct spinlock lock;
  uint refcount[RMAP_SIZE];
  struct proc* pageprocs[RMAP_SIZE][NPROC_RMAP];
} rmap;


void
rmap_init(void)
{
  // lock rmap
  initlock(&rmap.lock, "rmap");
  // initialize refcount
  // acquire(&rmap.lock);
  for(int i = 0; i < RMAP_SIZE; i++){
    rmap.refcount[i] = 0;
  }
  for(int i = 0; i < RMAP_SIZE; i++){
    for(int j = 0; j < NPROC_RMAP; j++){
      rmap.pageprocs[i][j] = 0;
    }
  }
  // release(&rmap.lock);
}

uint rmap_get_refcount(uint pa)
{
  acquire(&rmap.lock);
  uint refcount = rmap.refcount[pa / PGSIZE];
  release(&rmap.lock);
  return refcount;
}

struct proc* rmap_get_proc(uint pa, int index){
  acquire(&rmap.lock);
  struct proc* p = rmap.pageprocs[pa / PGSIZE][index];
  release(&rmap.lock);
  return p;
}

void rmap_incref(uint pa, struct proc* p)
{
  if(PRINT) cprintf("Allocing proc id : %d to memory locn %x\n", p->pid, pa);
  acquire(&rmap.lock);
  for(int i = 0; i < NPROC_RMAP; i++){
    if(rmap.pageprocs[pa / PGSIZE][i] == p){
      if(PRINT) cprintf("Already present\n");
      release(&rmap.lock);
      return;
    }
  }
  rmap.refcount[pa / PGSIZE]++;
  int flag = 0;
  for(int i = 0; i < NPROC_RMAP; i++){
    if(rmap.pageprocs[pa / PGSIZE][i] == 0){
      rmap.pageprocs[pa / PGSIZE][i] = p;
      flag = 1;
      break;
    }
  }
  release(&rmap.lock);
  if(flag == 0){
    panic("No space in rmap.pageprocs");
  }
}

void rmap_decref(uint pa, struct proc* p)
{
  if(PRINT) cprintf("Deallocing proc id : %d from memory locn %x\n", p->pid, pa);
  acquire(&rmap.lock);
  rmap.refcount[pa / PGSIZE]--;
  int flag = 0;
  if(p!=0){
    for(int i = 0; i < NPROC_RMAP; i++){
      if(rmap.pageprocs[pa / PGSIZE][i] == p){
        rmap.pageprocs[pa / PGSIZE][i] = 0;
        flag = 1;
        break;
      }
    }
  }
  release(&rmap.lock);
  if(flag == 0){
    // panic("No such process in rmap.pageprocs");
  }
}

void rmap_copy(uint pa1, uint pa2){
  acquire(&rmap.lock);
  rmap.refcount[pa2 / PGSIZE] = rmap.refcount[pa1 / PGSIZE];
  for(int i = 0; i < NPROC_RMAP; i++){
    rmap.pageprocs[pa2 / PGSIZE][i] = rmap.pageprocs[pa1 / PGSIZE][i];
    rmap.pageprocs[pa1 / PGSIZE][i] = 0;
  }
  rmap.refcount[pa1 / PGSIZE] = 0;
  release(&rmap.lock);

}










struct sleeplock swaplock;

void acquire_swaplock(){
  acquiresleep(&swaplock);
}

void release_swaplock(){
  releasesleep(&swaplock);
}

int holdingsleep_swaplock(){
  return holdingsleep(&swaplock);
}









// PAGESWAP

struct swap_slot {      // Struct to define a swap slot to store a single page (4KB) (8 blocks)
  int page_perm;
  int is_free;
};

struct spinlock swapslotlock;

struct swap_slot swap_space[NSWAP];

void
swapinit(void)
{
  int i;
  initsleeplock(&swaplock, "swaplock");
  initlock(&(swapslotlock), "swapslotlock");
  for(i = 0; i < NSWAP; i++)
  {
    swap_space[i].is_free = 1;
    swap_space[i].page_perm = 0;
  }
}

struct run* swap_out(struct proc *p, pte_t* pgtable_entry, int va)
{
  int acq = holdingsleep_swaplock();
  if(!acq){
    acquire_swaplock();
  }

  int i;
  // p->rss -= PGSIZE;
  for(i = 0; i < NSWAP; i++)
  {
    // swap out the page from the memory to the swap space
    // return the struct run* value pointing out to the old memory location of the swapped out page
    if(swap_space[i].is_free == 1)
    {
      uint pa;
      pa = PTE_ADDR(*(pgtable_entry));
      if(PRINT) cprintf("Swapping out page at %x to swap space %d, whose ref index is %x\n", pa, i, SWAP_TO_RMAP(i));
      swap_space[i].is_free = 0;
      swap_space[i].page_perm = PTE_FLAGS(*(pgtable_entry));
      struct run *page = (struct run*) P2V(PTE_ADDR(*(pgtable_entry)));
      for(int j = 0; j < 8; j++)
      {
        // read the page from the memory to the disk
        struct buf *b = bread(ROOTDEV, 8*i + SWAPSTART + j);
        memmove(b->data, P2V(PTE_ADDR(*(pgtable_entry))) + j*512, 512);
        bwrite(b);
        brelse(b);
      }
      // *(pgtable_entry) = (((8*i + SWAPSTART) << 12) | (swap_space[i].page_perm)) & ~PTE_P;

      for(int j = 0; j < NPROC_RMAP; j++){
        struct proc* p1 = rmap_get_proc(pa, j);
        if((p1 != 0)){
          p1->rss -= PGSIZE;
          pte_t* pte_1 = walkpgdir(p1->pgdir, (void*)va, 0);      // SHOULD IT BE pa ??
          // pte_t* pte_1;
          // int pte_flag = 0;
          // for(int k = 0; k < p1->sz; k+=PGSIZE){
          //   pte_1 = walkpgdir(p1->pgdir, (void*)k, 0);
          //   if(PTE_ADDR(*pte_1) == pa){
          //     pte_flag = 1;
          //     break;
          //   }
          // }
          if(PTE_ADDR(*pte_1) != pa){
            // panic("swap_out: PTE not found");
          }
          uint flags_1 = PTE_FLAGS(*pte_1);
          flags_1 = flags_1 & ~PTE_P;
          *(pte_1) = (((8*i + SWAPSTART) << 12) | flags_1);       // SHOULD IT BE (swap_space[i].page_perm)) & ~PTE_P ??
          if(PRINT) cprintf("DECREF 0");
          // rmap_decref(pa, p1);
          // rmap_incref(SWAP_TO_RMAP(i), p1);
        }
      }
      rmap_copy(pa, SWAP_TO_RMAP(i));

      lcr3(V2P(p->pgdir));
      if(!acq){
        release_swaplock();
      }
      return page;
    }
  }
  lcr3(V2P(p->pgdir));
  if(!acq){
    release_swaplock();
  }
  return 0;
}

void swap_in(uint fault_va, struct proc* p)
{
    // find the starting disk block id of the swapped page from the page table entry
    // uint fault_va = rcr2();
    // struct proc *p = myproc();
    pte_t *pgtable_entry = walkpgdir(p->pgdir, (void*)fault_va, 0);
    uint swap_block_id = PTE_ADDR(*pgtable_entry) >> 12;
    uint swap_addr = SWAP_TO_RMAP((swap_block_id - SWAPSTART)/8);
    // swap in the page
  // get the page permissions from the struct swap_slot corresponding to the swap_block_id
    // uint page_perm = swap_space[(swap_block_id - SWAPSTART)/8].page_perm;
    // allocate a memory page using kalloc()
    char *mem_page = kalloc();
    // copy data from disk to memory
    if(PRINT) cprintf("Swapping in page at swap space %d, whose ref index is %x to page %x\n", (swap_block_id - SWAPSTART)/8, SWAP_TO_RMAP((swap_block_id - SWAPSTART)/8), V2P(mem_page));
    for (int i = 0; i < 8; i++) {
      struct buf *b = bread(ROOTDEV, swap_block_id + i);
      memmove((mem_page + i*512), b->data, 512);
      brelse(b);
    }
    // restore the page premissions from the corresponding swap_slot.page_perm
    // update the page table entry of the swapped-in page
    for(int i = 0; i < NPROC_RMAP; i++){
      struct proc* p1 = rmap_get_proc(swap_addr, i);
      if(p1 != 0){
        p1->rss += PGSIZE;
        pte_t* pte_1 = walkpgdir(p1->pgdir, (void*)fault_va, 0);
        if(PTE_ADDR(*pte_1) != PTE_ADDR(*pgtable_entry)){
          // panic("swap_in: PTE not found");
        }
        uint flags_1 = PTE_FLAGS(*pte_1);
        flags_1 = flags_1 | PTE_P;
        *(pte_1) = V2P(mem_page) | flags_1;
        // rmap_incref(V2P(mem_page), p1);
        // if(PRINT) cprintf("DECREF 1");
        // rmap_decref(swap_addr, p1);
      }
    }
    rmap_copy(swap_addr, V2P(mem_page));
    // *pgtable_entry = V2P(mem_page) | page_perm | PTE_P;
    swap_space[(swap_block_id - SWAPSTART)/8].is_free = 1;

    // p->rss+=PGSIZE;

    lcr3(V2P(p->pgdir));
}

void free_swap(struct proc* p)
{
  for(int i = 0; i < p->sz; i+=PGSIZE)
  {
    pte_t *pgtable_entry = walkpgdir(p->pgdir, (void*)i, 0);
    if(pgtable_entry && ((*pgtable_entry & PTE_P) == 0))
    {
      uint swap_block_id = PTE_ADDR(*pgtable_entry) >> 12;
      uint swap_addr = SWAP_TO_RMAP((swap_block_id - SWAPSTART)/8);
      if(PRINT) cprintf("DECREF 2");
      rmap_decref(swap_addr, p);
      acquire(&(swapslotlock));
      if(rmap_get_refcount(SWAP_TO_RMAP((swap_block_id - SWAPSTART)/8)) == 0)
      {
        swap_space[(swap_block_id - SWAPSTART)/8].is_free = 1;
      }
      release(&(swapslotlock));
    }
  }
}

void print_slots()
{
  for(int i = 0; i < NSWAP; i++)
  {
    if(PRINT) cprintf("swap_space[%d].is_free = %d\n",i,swap_space[i].is_free);
  }
}














// GET_VICTIM PAGE

struct pte_va get_victim_page(struct proc* p)
{
    int i = 0;
    int max_lim = 0;
    struct pte_va ans;
    ans.pte = 0;
    ans.va = 0;
    for(i = 0; i < p->sz; i+=PGSIZE)
    {
      pte_t *pgtable_entry = walkpgdir(p->pgdir, (void*)i, 0);
      if(((*(pgtable_entry) & PTE_P) != 0) && ((*(pgtable_entry) & PTE_A) == 0))
      {
        ans.pte = pgtable_entry;
        ans.va = i;
        return ans;
      }
      if((*(pgtable_entry) & PTE_A) != 0){
        max_lim++;
      }
    }

    max_lim = (int)((max_lim + 9) * 0.1);
    int counter = 0;
    for (i = 0; i < p->sz; i+=PGSIZE)
    {
      pte_t *pgtable_entry = walkpgdir(p->pgdir, (void*)i, 0);
      if(((*(pgtable_entry) & PTE_A) != 0) && ((*(pgtable_entry) & PTE_P) != 0))
      {
        counter++;
        *(pgtable_entry) = *(pgtable_entry) ^ PTE_A;
      }
      if(counter >= max_lim)
      {
        break;
      }
    }

    max_lim = 0;
    for(i = 0; i < p->sz; i+=PGSIZE)
    {
      pte_t *pgtable_entry = walkpgdir(p->pgdir, (void*)i, 0);
      if(((*(pgtable_entry) & PTE_P) != 0) && ((*(pgtable_entry) & PTE_A) == 0))
      {
        ans.pte = pgtable_entry;
        ans.va = i;
        return ans;
      }
      if((*(pgtable_entry) & PTE_A) != 0){
        max_lim++;
      }
    }

  return ans;
}









char memtemp[PGSIZE];
// PAGE FAULT HANDLER

void pagefault_handler(uint va, pde_t* pgdir)
{
    acquire_swaplock();
    pte_t *pte;
    char* mem;
    // if(PRINT) cprintf("virtual address: %x\n", va);

    if((pte = walkpgdir(pgdir, (void*)va, 0)) == 0){
      panic("trap.c PGFLT: pte should exist");
    }
    if(!(*pte & PTE_P))
    {
      swap_in(va, myproc());
      release_swaplock();
      return;
    //   if(PRINT) cprintf("PTE_P: %d\n", *pte & PTE_P);
    //   if(PRINT) cprintf("star pte: %x\n", *pte);
    //   if(PRINT) cprintf("pte: %x\n", pte);
      // panic("trap.c PGFLT: page not present");
    }
    uint p_add = PTE_ADDR(*pte);          // p_add is a physical address
    if(rmap_get_refcount(p_add) == 1){
      *pte = *pte | PTE_W;
    }

    else{
      memmove(memtemp, (char*)(P2V(p_add)), PGSIZE);

      if((mem = kalloc()) == 0){
          panic("trap.c PGFLT: Page copy not possible. Out of memory");
        }
        memmove(mem, memtemp, PGSIZE);
        
        uint flags = PTE_FLAGS(*pte);
        flags = flags | PTE_W | PTE_P;
        *pte = V2P(mem) | flags;

        rmap_incref(V2P(mem), myproc());
        if(PRINT) cprintf("DECREF 3");
        rmap_decref(p_add, myproc());
    }
    lcr3(V2P(pgdir));

    // for(int i = 0; i < RMAP_SIZE; i++)
    // {
    //   if(rmap_get_refcount(i * PGSIZE) != 0)
    //   {
    //     if(PRINT) cprintf("Page %d: %d\n", i, rmap_get_refcount(i * PGSIZE));
    //   }
    // }
    release_swaplock();
}


// fork test
// lapicid 0: panic: No victim page

// memtest1
// lapicid 0: panic: trap.c PGFLT: pte should exist

// memtest2
// lapicid 0: panic: trap.c PGFLT: pte should exist