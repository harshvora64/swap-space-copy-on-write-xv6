#define NPROC        64  // maximum number of processes
#define KSTACKSIZE 4096  // size of per-process kernel stack
#define NCPU          1  // maximum number of CPUs
#define NOFILE       16  // open files per process
#define NFILE       100  // open files per system
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       1  // device number of file system root disk
#define MAXARG       32  // max exec arguments
#define MAXOPBLOCKS  10  // max # of blocks any FS op writes
#define LOGSIZE      (MAXOPBLOCKS*3)  // max data blocks in on-disk log
#define NBUF         (MAXOPBLOCKS*3)  // size of disk block cache
#define SWAPBLOCKS   (400 * 8)  // number of swap blocks
#define FSSIZE       5000  // size of file system in blocks
#define NSWAP        SWAPBLOCKS / 8  // number of swap slots
#define NUM_PAGES    1024  // number of pages in the physical memory
#define RMAP_SIZE    NUM_PAGES + NSWAP + 100 // size of the rmap
#define SWAP_TO_RMAP(i) ((i + NUM_PAGES) * PGSIZE)
#define SWAPSTART    2
#define NPROC_RMAP   (NPROC)
#define PRINT        0