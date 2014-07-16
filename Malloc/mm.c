/*
 * mm.c
 *
 * Name: Punnawachara Campanang
 * Andrew ID: pcampana
 * 
 * This is the memory allocator for 64 bit machine.
 * 
 * Segregated List:
 *     This memory allocator uses regular segregated list. The list is
 *     implemented by doubly linked list. Which means that the minimum block
 *     size = 4 (header) + 8 (prev_block ptr) + 8 (next_block ptr) + 4 (footer)
 *     = 24 bytes. 
 *     The pointers to the root of each list are store at thebeginning of the 
 *     heap. When mm_init is called, the initialization looks like this
 * 
 * origin                                  heap_listp
 * |                                           |
 * V                                           V
 * [List1 ptr][...][padding + prologue header][preologue footer][epilogue]
 *      |                                          
 *      V                                          
 * [  free    ] -> [  free    ] -> [  free    ] 
 * [  block   ] <- [  block   ] <- [  block   ]
 * 
 * Size Bracket: Multiple of 24 is used for the block size. For small blocks,
 *     next list will increase the size by 24. Larger block size is categorized
 *     in term of power of 2 of multiple of 24.
 * 
 * Block Structure:
 *     [     Footer of previous block    ] ssssssss ssssssss ssssssss sssssuua
 *     pppppppp pppppppp pppppppp pppppppp pppppppp pppppppp pppppppp pppppppp
 *     nnnnnnnn nnnnnnnn nnnnnnnn nnnnnnnn nnnnnnnn nnnnnnnn nnnnnnnn nnnnnnnn
 *     ssssssss ssssssss ssssssss sssssuua [       Header of next block      ]
 * 
 *     s: Size bit. Since the address must be double word aligned, we don't 
 *        need 3 least significant bits for size
 *     u: unused
 *     a: Allocate bit indicates that the block is not free.
 *     p: Previous block pointer for free block.
 *     n: Next block pointer for free block.
 * 
 * Search Policy: Since the block is already segmented by size range,
 *     this memory allocator uses "First Fit" policy for best throughput.  
 * 
 * Insert: Always insert the block at the begining of the list.
 * 
 * Coalescing: Block is coalesced instantly (no deferred).
 * 
 * Check Heap: The function mm_checkheap() is called after the iportant 
 *     operations like mm_init, alloc, realloc, free. 
 *     To activate the heap checker, please change the 
 *     #define NO_CHECK_HEAP (line 152) to CHECK_HEAP
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mm.h"
#include "memlib.h"

/* If you want debugging output, use the following macro.  When you hand
 * in, remove the #define DEBUG line. */
#define DEBUGx /* Padded with x for hand in */
#ifdef DEBUG
# define dbg_printf(...) printf(__VA_ARGS__)
#else
# define dbg_printf(...)
#endif


/* do not change the following! */
#ifdef DRIVER
/* create aliases for driver tests */
#define malloc mm_malloc
#define free mm_free
#define realloc mm_realloc
#define calloc mm_calloc
#endif /* def DRIVER */

/************************************** 
 Macros
***************************************/

/* Single word (4) or double word (8) alignment */
#define ALIGNMENT 8

/* Rounds up to the nearest multiple of ALIGNMENT */
#define ALIGN(p) (((size_t)(p) + (ALIGNMENT-1)) & ~0x7)

/* Basic macros and constant (adapt from text book*/
#define WSIZE 4 /* Word and header/footer size (bytes) */
#define DSIZE 8 /* Double word size (bytes) */
#define CHUNKSIZE (168) /* Extend heap by this amount (bytes) */
#define MINIMUM_BLK_SIZE 24 /* Minimum block size */
#define MINIMUM_PAYLOAD_SIZE 16 /* 2 ptr = 16 bytes in 64 bit machine */
#define HEADER_SIZE 8 /* Header + Footer size */

#define MAX(x,y) ((x) > (y)? (x) : (y))

/* Pack a size and allocate bit into a word */
#define PACK(size, alloc) ((size) | (alloc))

/* Read and write a word at address p */
#define GETW(p)       (*(unsigned int *)(p))
#define PUTW(p, val)  (*(unsigned int *)(p) = (val))
#define GETD(p)       (*(size_t *)(p))
#define PUTD(p, val)  (*(size_t *)(p) = (val))

/* Read the size and allocate fields from address p */
#define GET_SIZE(p)  (GETW(p) & ~0x7)
#define GET_ALLOC(p) (GETW(p) & 0x1)

/* Given block ptr bp, compute address of its header and footer */
#define HDRP(bp)     ((char *)(bp) - WSIZE)
#define FTRP(bp)     ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)

/* Given block ptr bp, compute address of next and previous blocks*/
#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(((char *)(bp) - WSIZE)))
#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE(((char *)(bp) - DSIZE)))

/*  Linked list macros */ 
#define NEXT_FREE_BLKP(bp)           ((char *)(GETD((char *)bp + DSIZE)))
#define PREV_FREE_BLKP(bp)           ((char *)(GETD((char *)bp)))
#define SET_NEXT_FREE_BLKP(bp, n_bp) PUTD((char *)bp + DSIZE, ((size_t)n_bp))
#define SET_PREV_FREE_BLKP(bp, p_bp) PUTD((char *)bp, ((size_t)p_bp))

/* Seglist macros */
#define LIST(list)          ((char *)origin + ((list - 1) * DSIZE)) 
#define GET_LISTP(list)     ((char *)(GETD(LIST(list))))
#define SET_LISTP(list, bp) PUTD((LIST(list)), (size_t)(bp))

/* Max size for each list (no max size for last list) */
#define MAX_SIZE_01 32
#define MAX_SIZE_02 64
#define MAX_SIZE_03 128
#define MAX_SIZE_04 256
#define MAX_SIZE_05 512
#define MAX_SIZE_06 1024
#define MAX_SIZE_07 2048
#define MAX_SIZE_08 4096
#define MAX_SIZE_09 8192
#define MAX_SIZE_10 16384
#define MAX_SIZE_11 32768
#define MAX_SIZE_12 65536
#define LISTS       13

/* Check heap activation. */ 
/* Please change NO_CHECK_HEAP to CHECK_HEAP to activate checkheap function */
#define NO_CHECK_HEAP

/************************************** 
 End of macros
***************************************/
 
/************************************** 
 Function prototype
***************************************/

static void print_block(void *bp);
static int  check_block(void *bp);
static int  check_coalesce(void* bp);
static int  check_list_cycle(int verbose);
static int  check_free_list(int verbose);
static int  check_block_size_range(void *bp, int list_index);
static void *extend_heap(size_t words);
static void *find_fit(size_t size);
static void *coalesce(void *bp);
static void place(void *bp, size_t a_size);
static void insert_free_block(void *bp);
static void remove_free_block(void *bp);
static int  get_list_index(size_t a_size);
static unsigned int get_max_size(int list_index);

/************************************** 
 End of function prototype
***************************************/

/************************************** 
 Global variables
***************************************/

char *heap_listp = 0; /* A pointer to epilogue block */
char *origin = 0; /* A pointer to the beginning of the lists area */

/************************************** 
 End of global variables
***************************************/ 

/*
 * Initialize: Initialize the heap space for memory allocation.
 *     Initial heap space can be divided into 2 part. The first part is the
 *     list area where the pointers to each segregated list are stored.
 *     Each pointer is 8 bytes in 64 bit machine so this area will take
 *     DSIZE x LISTS byte. The second part of initialization is to create
 *     the epilogue and prologue blocks. 4 x WSIZE bytes are allocated.
 *     To follow the alignment rule, the first 4 byte is padded with 0.
 *     The next bytes are used for prologue header (4 bytes), prologue footer
 *     (4 bytes), and epilogue block (4 bytes).
 *     The header and footer of prologue block will have size of 8 bytes with
 *     allocate but = 1. The epilogue block will have size = 0 with allocate
 *     bit = 1. The origin ptr is set at the first list ptr, the heap_listp
 *     ptr is set at the prologue block
 * 
 * Heap structure after initialization:
 * 
 * [pppppppp][pppppppp][pppppppp][pppppppp][pppppppp][0000 hhhh][ffff eplg]
 *  ^                                                           ^           
 *  |                                                           |           
 * origin                                                   heap_listp      
 * 
 *   After mandatory blocks creatation, the mm_init will extend the heap size
 * by CHUNKSIZE. This space is the first free block in the heap the the memory
 * allocator can use for malloc, realloc, calloc and free
 */
int mm_init(void) {
    int i;
    /* Create space for all list roots */
    if ((origin = mem_sbrk(LISTS * DSIZE)) == NULL) {
        return -1;
    }
    
    /* Create initial empty heap (need 2 minimum block size for starting)*/
    if ((heap_listp = mem_sbrk(4 * WSIZE)) == NULL) {
        return -1;
    }
    
    /* Initialize the prologue block */
    PUTW(heap_listp, 0);                            /* Alignment padding */
    PUTW(heap_listp + (1 * WSIZE), PACK(DSIZE, 1)); /* Prologue header */
    PUTW(heap_listp + (2 * WSIZE), PACK(DSIZE, 1)); /* Prologue footer */
    
    /* Initialize the epillogue block */
    PUTW(heap_listp + (3 * WSIZE), PACK(0, 1));
    
    /* initialize roots */
    for (i = 1; i <= LISTS; i++) {
        SET_LISTP(i, NULL);
    }
    
    /* Set heap_listp after initialization */
    heap_listp += 2 * WSIZE;
    
    /* Extend empty heap with a free block of CHUNKSIZE bytes */
    if (extend_heap(CHUNKSIZE/WSIZE) == NULL) {
        return -1;
    }
    
    /* Check heap for correctness */
    #ifdef CHECK_HEAP
        mm_checkheap(0);
    #endif
    
    return 0;
}

/*
 * malloc: This function will allocate the space for the requires size of data
 *     (bytes). The function will return the pointer that points to the
 *     beginning of the allocated block.
 * 
 * [header][allocated space for payload (bytes)][footer]
 *          ^
 *          |
 *     return pointer
 * 
 *     Since we need some space to put the linked list pointer when the block 
 * is free, malloc will adjust the size requested by user to the minimum of 24 
 * bytes. Moreover, the allocated memory space must be double word aligned.
 * 
 *     After adjust the requested size to align with alignment, malloc will
 * travel through the free lists tofind the available free block that big
 * enough. If there is no available free block, malloc will extend the heap
 * to acquire more free space.
 * 
 *    After malloc get space, malloc will mark that block as allocated by
 * place() function (see more detail in place() function).
 */
void *malloc (size_t size) {
    size_t a_size;
    size_t extend_size;
    char *bp;
    
    /* Initiallize the heap if the heap is not initialize before */
    if (heap_listp == 0){
        mm_init();
    }
    
    /* Ignore spurious request */
    if (size == 0) {
        return NULL;
    }
    
    /* Adjust block size to include overhead and alignment reqs. */
    if (size <= MINIMUM_PAYLOAD_SIZE) { 
        a_size = MINIMUM_BLK_SIZE; /* Allocate at least minimu block size */
    }
    else {
        a_size = DSIZE * ((size + (DSIZE) + (DSIZE-1)) / DSIZE);
    }

    /* Search a free list for a fit */
    if ((bp = find_fit(a_size)) != NULL) {
        place(bp, a_size);
        
        /* Check heap for correctness */
        #ifdef CHECK_HEAP
            mm_checkheap(0);
        #endif
        
        return bp;
    }
    
    /* No fit found. Get more memory and place the block */
    extend_size = MAX(a_size, CHUNKSIZE);
    if ((bp = extend_heap(extend_size/WSIZE)) == NULL) {
        return NULL;
    }
    
    /* Check heap for correctness */
    #ifdef CHECK_HEAP
        mm_checkheap(0);
    #endif
    
    place(bp, a_size);
    return bp;
}

/*
 * free: Free the allocated space pointed by the pointer ptr. This block will
 *     be marked as free and insert into free lists (with coalescing).
 */
void free (void *ptr) {
    size_t size;
    
    /* Initialize the heap if the heap is not initilized */
    if (heap_listp == 0){
        mm_init();
    }
    
    /* return if ptr = NULL */
    if (!ptr) {
        return;
    }
    
    /* Mark the block as free */
    size = GET_SIZE(HDRP(ptr));
    PUTW(HDRP(ptr), PACK(size, 0));
    PUTW(FTRP(ptr), PACK(size, 0));
    
    /* Coalesce with adjacent block and insert it to free list */
    coalesce(ptr);
    
    /* Check heap for correctness */
    #ifdef CHECK_HEAP
        mm_checkheap(0);
    #endif
}

/*
 * realloc: Reallocate the space that is allocated before with new size.
 * 
 * If new size <= old size: The function will try to shrink current block if
 *     the rest of space is bigger than minimum block size. The function will 
 *     return old ptr.
 * 
 * If new size > old size: The function will check the next block for the 
 *     possibility to extend. If possible, the function will extend and return 
 *     old ptr. Otherwise, This function just call malloc again to find 
 *     a new place for the memory allocation and then copy the data from 
 *     current memory to the newly allocated space. Then, free the old ptr
 *     and return new ptr.
 */
void *realloc(void *old_ptr, size_t size) {
    size_t old_size;
    size_t new_size;
    size_t next_alloc;
    size_t next_size;
    size_t extend_size;
    size_t payload_size;
    void *new_ptr;
    void *bp;
    
    /* If size == 0 then this is just free, and we return NULL. */
    if(size == 0) {
        free(old_ptr);
        return 0;
    }

    /* If old_ptr is NULL, then this is just malloc. */
    if(old_ptr == NULL) {
        return malloc(size);
    }
    
    /* Get old size*/
    old_size = GET_SIZE(HDRP(old_ptr));
    
    /* Get new size */
    if (size <= MINIMUM_PAYLOAD_SIZE) {
        new_size = MINIMUM_BLK_SIZE;
    }
    else {
        new_size = ALIGN(size) + HEADER_SIZE;
    }
    
    /* Start realloc */
    if (new_size == old_size) { /* new = old, do nothing */
        /* Check heap for correctness */
        #ifdef CHECK_HEAP
            mm_checkheap(0);
        #endif
        return old_ptr;
    }
    else if (new_size < old_size) { /* Can use the current block */
         /* split if the rest is biggerthan minimum block size */
        if ((old_size - new_size) >= MINIMUM_BLK_SIZE) {
            PUTW(HDRP(old_ptr), PACK(new_size, 1));
            PUTW(FTRP(old_ptr), PACK(new_size, 1));
            
            /* Free the rest of the block */
            bp = NEXT_BLKP(old_ptr);
            PUTW(HDRP(bp), PACK((old_size - new_size), 0));
            PUTW(FTRP(bp), PACK((old_size - new_size), 0));
            coalesce(bp);
        }
        
        /* Check heap for correctness */
        #ifdef CHECK_HEAP
            mm_checkheap(0);
        #endif
        return old_ptr;
    }
    else { /* new_size > old_size, find the way to extend if possible */
        next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(old_ptr)));
        next_size = GET_SIZE(HDRP(NEXT_BLKP(old_ptr)));
        extend_size = new_size - old_size;
        payload_size = old_size - HEADER_SIZE;
        
        if (!next_alloc && next_size > extend_size) { /* Can extend */
            remove_free_block(NEXT_BLKP(old_ptr));
            
            /* Split the block if the rest is bigger than min block size */
            if ((next_size - extend_size) >= MINIMUM_BLK_SIZE) {
                PUTW(HDRP(old_ptr), PACK(new_size, 1));
                PUTW(FTRP(old_ptr), PACK(new_size, 1));
                
                /* Free the rest of the block */
                bp = NEXT_BLKP(old_ptr);
                PUTW(HDRP(bp), PACK((next_size - extend_size), 0));
                PUTW(FTRP(bp), PACK((next_size - extend_size), 0));
                coalesce(bp);
            }
            else { /* Can't split, just merge the blocks */
                PUTW(HDRP(old_ptr), PACK((old_size + next_size), 1));
                PUTW(FTRP(old_ptr), PACK((old_size + next_size), 1));
            }
            
            /* Check heap for correctness */
            #ifdef CHECK_HEAP
                mm_checkheap(0);
            #endif
            return old_ptr;
        }
        else { /* Can't extend, Find new place */
            new_ptr = malloc(size);
            
            /* If realloc() fails the original block is left untouched  */
            if(!new_ptr) {
                return NULL;
            }
            
            /* Copy the old data. */
            memcpy(new_ptr, old_ptr, payload_size);
            
            /* Free the old block. */
            free(old_ptr);
            
            /* Check heap for correctness */
            #ifdef CHECK_HEAP
                mm_checkheap(0);
            #endif
            return new_ptr;
        }
    }
    
    return NULL;
}

/*
 * calloc: This function works the same way as malloc but will set the payload
 *     area to 0. So, just call mallc and use memset :D.
 */
void *calloc (size_t nmemb, size_t size) {
    void *bp;
    size_t total_size = nmemb * size;
    bp = malloc(total_size);
    memset(bp, 0, total_size);
    return bp;
}

/*
 * in_heap: Return whether the pointer is in the heap.
 *     May be useful for debugging.
 */
static int in_heap(const void *p) {
    return p <= mem_heap_hi() && p >= mem_heap_lo();
}

/*
 * aligned: Return whether the pointer is aligned.
 *     May be useful for debugging.
 */
static int aligned(const void *p) {
    return (size_t)ALIGN(p) == (size_t)p;
}

/*
 * mm_checkheap: Perform the heap and free block check for correctness
 *     print message if verbose
 *     print block's detail if verbose > 1
 * 
 * Check list:
 * 1. The correctness of list ptr area and prologue 
 *     
 * 2. Travel through each block in heap and and Call
 *    check_block function
 *    - Check block pointer vs heap boundary
 *    - Check pointer alignment 
 *    - Check header & footer match
 *    - Check alignment of payload area
 *    - Check block size vs. minimum block size
 * 
 *    check_coalesce function
 *    - Check for block coalescing
 *     
 * 3. Check the correctness of epilogue block
 * 
 * 4. Check the cycle reference in the free list using hare and turtle
 *      In this check, if the cycle is detected, the function will call exit
 *    because the cycle in the list will create infinite loop when we travel
 *    through the free list. So, check no. 5 and no. 6 will not work properly.
 * 
 * 5. Go through each bloack in the free lis and check for
 *    - The pointer to the block must be within heap range
 *    - The pointer must be aligned with the alignment
 *    - The consistency of the prev and next link of linked list
 *    - The size of free block must falls into its list bracket
 *     
 * 6. Check for the dead free block in the heap (the block that is free but
 *    not in the free list)
 */
void mm_checkheap(int verbose) {
    int list_index;
    int error = 0;
    int heap_free_blocks = 0;
    int list_free_blocks = 0;
    char *bp = heap_listp;
    
    if (verbose) {
        printf("Check heap: Start\n");
        printf("Check heap correctness: Start\n");
    }
    
    /* Check the origin pointer */
    if (verbose > 1) {
        printf("Origin (%p):\n", origin);
    }
    
    /* Check origin position */
    if (!in_heap(origin)) {
        printf("Error: origin is out of heap\n");
        error = 1;
    }
    
    /* Check the list ptr are for alignment */
    if ((origin - heap_listp) % ALIGNMENT) {
        printf("Error: Origin area is not aligned\n");
        error = 1;
    }
    
    /* Check that list area is aligned correctly */
    if (LISTS * DSIZE != heap_listp - origin - DSIZE) {
        printf("Error: Lists area is not aligned correctly\n");
        error = 1;
    }
    
    /* Check Heap_initialization & prologue */
    if (verbose > 1) {
        printf("Heap (%p):\n", heap_listp);
    }
    
    /* Check origin ptr address alignment */
    if (!aligned(origin)) {
        printf("Error: %p origin ptr is not aligned\n", origin);
        error = 1;
    }
    
    /* Check heap_listp ptr address alignment */
    if (!aligned(heap_listp)) {
        printf("Error: %p heap_listp ptr is not aligned\n", heap_listp);
        error = 1;
    }
    
    /* Check prologue block */
    if ((GET_SIZE(HDRP(heap_listp)) != DSIZE) || !GET_ALLOC(HDRP(heap_listp))) {
	    printf("Error: Bad prologue header\n");
        error = 1;
    }
    check_block(heap_listp);
    
    /* Check every block in heap for correctness */
    for (bp = heap_listp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp)) {
	    if (verbose > 1) { 
	        print_block(bp);
        }
        
        /* Check block structure */
	    if (check_block(bp)) {
            error = 1;
        }
        
        /* Check coalescing */
        if (check_coalesce(bp)) {
            error = 1;
        }
        
        /* Count free block if the block is free*/
        if (!GET_ALLOC(HDRP(bp))) {
            heap_free_blocks++;
        }
    }
    
    /* Check epilogue block*/
    if (verbose > 1) {
	    print_block(bp);
    }
    if ((GET_SIZE(HDRP(bp)) != 0) || !(GET_ALLOC(HDRP(bp)))) {
	    printf("Error: Bad epilogue header\n");
        error = 1;
    }
    
    if (verbose) {
        if (!error) {
            printf("Check heap correctness: No error detected\n");
        }
    }
    
    /* Check for cycle reference in the free lists */
    if (check_list_cycle(verbose)) {
        /* When the cycle is detected, it means that when we travel through */
        /* the free lists, we will get into infinite loop.*/
        /* Thus, the check_free_list and check_dea_block will not work */
        /* Since it requires traveling through free lists */
        /* So, I call exit here */
        printf("Check heap fail: Terminate the program\n");
        exit(1);
    }
    
    /* Check free lists */
    if (check_free_list(verbose)) {
        error = 1;
    }
    
    /* Check for number of free blocks in heap vs. in free lists */
    for (list_index = 1; list_index <= LISTS; list_index++) {
        for (bp = GET_LISTP(list_index); 
        bp != NULL && GET_SIZE(HDRP(bp)) > 0; bp = NEXT_FREE_BLKP(bp) ) {
            list_free_blocks++;
        }
    }
    if (heap_free_blocks != list_free_blocks) {
        printf("Error: Number of free blocks in heap and lists mismatch\n");
        printf("Free blocks in heap: %d\n", heap_free_blocks);
        printf("Free blocks in list: %d\n", list_free_blocks);
        error = 1;
    }
    
    /* Display final message */
    if (error) {
        printf("Check heap fail: Terminate the program\n");
        exit(1);
    }
    else {
        if (verbose) {
            printf("Check heap complete: No error detected\n");
        }
    }
}

/************************************** 
 My utility functions
***************************************/

/*
 * print_block: Print the block detail.
 *     - Header & footer (size and allocate status)
 *     - If the block is free, show next and prev block ptr.
 */
static void print_block(void *bp) {
    size_t hsize;
    size_t halloc; 
    size_t fsize; 
    size_t falloc;
    
    if (bp == NULL) {
        printf("Pointer  = NULL\n");
        return;
    }
    
    hsize = GET_SIZE(HDRP(bp));
    halloc = GET_ALLOC(HDRP(bp));  
    fsize = GET_SIZE(FTRP(bp));
    falloc = GET_ALLOC(FTRP(bp));  

    if (hsize == 0) {
        printf("%p: Epilogue\n", bp);
        return;
    }
    
    if (halloc){
        printf("%p: header:[%lu:%c] footer:[%lu:%c]\n", 
        bp, 
        hsize, (halloc ? 'a' : 'f'),
        fsize, (falloc ? 'a' : 'f'));
    }
    else {
        printf("%p: header:[%lu:%c] p_free:%p n_free:%p footer:[%lu:%c]\n",
        bp, 
        hsize, (halloc ? 'a' : 'f'), 
        PREV_FREE_BLKP(bp),
        NEXT_FREE_BLKP(bp), 
        fsize, (falloc ? 'a' : 'f'));
    }
}

/*
 * check_block: This function will check the basic properties of the blocks
 *     - The pointer of the block must be in heap range
 *     - The address of the block must be doubleword aligned
 *     - The header must match the footer
 *     - The payload area must be aligned
 *     - Block size must be bigger than minimum block size
 */
static int check_block(void *bp) {
    int error = 0;
    
    if (bp == NULL) {
        return 0;
    }
    
    /* Pointer position check */
    if (!in_heap(bp)) {
        printf("Error: %p is not in heap\n", bp);
        error = 1;
    }
    
    /* Address alignment check */
    if ((size_t)bp % ALIGNMENT) {
        printf("Error: %p is not doubleword aligned\n", bp);
        error = 1;
    }
    
    /* Header and footer check */
    if (GETW(HDRP(bp)) != GETW(FTRP(bp))) {
	    printf("Error: %p header does not match footer\n", bp);
        error = 1;
    }
    
    /* Check payload area alignment */
    if ((GET_SIZE(HDRP(bp)) - HEADER_SIZE) % ALIGNMENT) {
        printf("Error: %p payload area is not aligned\n", bp);
        error = 1;
    }
    
    /* Check minimum block size (will not check prologue and epilogue) */
    if (bp != heap_listp && GET_SIZE(HDRP(bp)) > 0){
        if (GET_SIZE(HDRP(bp)) < MINIMUM_BLK_SIZE) {
            printf("Error: %p block is smaller than minimum block size\n", bp);
            error = 1;
        }
    }
    
    return error;
}

/*
 * check_coalesce: If the block is free, we need to make sure that the adjacent
 *     blocks of bp is not a free block if the memory allocator works correctly.
 */
static int check_coalesce(void* bp) {
    size_t prev_alloc;
    size_t next_alloc;
    
    /* Check coalescing with the adjacent block when bp is the free block */
    if (!GET_ALLOC(HDRP(bp))) {
        prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
        next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
        
        if (!prev_alloc || !next_alloc) {
            printf("The free block bp = %p is not coalesce\n", bp);
            return 1;
        }
    }
    
    return 0;
}

/* 
 * check_list_cycle(): Linked List cycle check using Hare and Turtle Algorithm
 *     Both Hare and Turtle will start at the beginning of the free list
 *     Hare will move 2 step at a time while turtle move one step at a time
 *     If they meet each other, it means that there is cycle in the linked list
 */
static int check_list_cycle(int verbose) {
    void *hare;
    void *turtle;
    int list_index;
    int error = 0;
    
    /* Display message if verbose */
    if (verbose) {
        printf("Check cycle in free list: Start\n");
    }
    
    for (list_index = 1; list_index <= LISTS; list_index++) {
        
        /* Hare and turtle start at starting point */
        hare = GET_LISTP(list_index);
        turtle = GET_LISTP(list_index);
        
        /* Detect cycle in each list and print the list that has cycle */
        while (hare != NULL && turtle != NULL && NEXT_FREE_BLKP(hare) != NULL) {
            
            /* Turtle moves only 1 step and Hare moves 2 steps */
            turtle = NEXT_FREE_BLKP(turtle); 
            hare = NEXT_FREE_BLKP(NEXT_FREE_BLKP(hare));
            
            /* Normally, Turtle will not be able to catch up with Hare */
            /* If both of them meet, it means the Hare runs in cycle */
            if (hare == turtle) {
                printf("Cycle detected in seg list %d\n", list_index);
                error = 1;
                break;
            }
        }
    }
    
    /* If no cycle, show success message */
    if (verbose) {
        if (!error) {
            printf("Check cycle in free list: No cycle detected\n");
        }
    }
    
    return error;
}

/*
 * check_free_list: This function will traverse through the free list and check
 *     - The pointer to the block must be within heap range
 *     - The pointer must be aligned with the alignment
 *     - The blocks in free list should not be allocated
 *     - The consistency of the prev and next link of linked list
 *     - Root block's prev_blkp must be NULL
 *     - The size of free block must falls into its list bracket
 *     - Double check coalescing for each free block in list
 *     - Print Message if verbose
 *     - Print free blocks if verbose > 1
 */
static int check_free_list(int verbose) {
    int list_index;
    int error = 0;
    void *bp;
    
    if(verbose) {
        printf("Checking free lists: Start\n");
    }
    
    for (list_index = 1; list_index <= LISTS; list_index++) {
        /* Display message if verbose */
        if (verbose > 1) {
            printf("List %d ", list_index);
            if (GET_LISTP(list_index) == NULL) {
                printf("is empty\n");
            }
        }
        
        for (bp = GET_LISTP(list_index); 
        bp != NULL && GET_SIZE(HDRP(bp)) > 0; bp = NEXT_FREE_BLKP(bp)) {
            
            /* Check if the pointer is in heap */
            if (!in_heap(bp)) {
                printf("Error: Free block %p is not in heap\n", bp);
                error = 1;
            }
            
            /* Check bp alignment */
            if (!aligned(bp)) {
                printf("Error: %p is not aligned with the alignment\n", bp);
                error = 1;
            }
            
            /* Check for misallocated block */
            if (GET_ALLOC(HDRP(bp))) {
                printf("Error: %p block is allocated in free list\n", bp);
                error = 1;
            }
            
            /* Check root correctness */
            if (bp == GET_LISTP(list_index)) {
                if (PREV_FREE_BLKP(bp) != NULL) {
                    printf("Error: %p root block doesn't point back to null\n", 
                    bp);
                    error = 1;
                }
            }
            
            /* Check link reference consistency */
            if (NEXT_FREE_BLKP(bp) != NULL) {
                if (PREV_FREE_BLKP(NEXT_FREE_BLKP(bp)) != bp) {
                    printf("Error: Link between blocks does not match ");
                    printf("in seg list %d\n", list_index);
                    error = 1;
                }
            }
            if (PREV_FREE_BLKP(bp) != NULL) {
                if (NEXT_FREE_BLKP(PREV_FREE_BLKP(bp)) != bp) {
                    printf("Error: Link between blocks does not match ");
                    printf("in seg list %d\n", list_index);
                    error = 1;
                }
            }
            
            /* Verify that block size falls into the correct bracket */
            if (check_block_size_range(bp, list_index)) {
                error = 1;
            }
            
            /* Double check for coalescing in list level */
            if (check_coalesce(bp)) {
                error = 1;
            }
            
            /* Print each block if verbose */
            if (verbose > 1) {
                printf("\n");
                print_block(bp);
            }
        }
    }
    
    /* Display success message if no error is detected */
    if(verbose) {
        if (!error) {
            printf("Checking free lists: No error detected\n");    
        }
    }

    return error;
}

/*
 * check_block_size_range: Check the size of the block then compare to the 
 *     maximum and minim block size of the the particular list. 
 *     Toggle error if the size is out of range.
 * Note: the min_size in this function is not actual minimum size of block
 *     in the bracket, it is the max size of the previous bracket.
 *     So, I use min_size + DSIZE to display the actual minimum size 
 *     (doubleword alignment)
 */
static int check_block_size_range(void *bp, int list_index) {
    unsigned int max_size;
    unsigned int min_size;
    
    if (list_index == 1) { /* Minimum size is 0 for the first list */
        max_size = get_max_size(list_index);
        min_size = 0;
        if (GET_SIZE(HDRP(bp)) <= min_size || GET_SIZE(HDRP(bp)) > max_size) {
            printf("Error: Block size is out of range in list ");
            printf("%d, min size: %d, max size: %d\n", 
            list_index, min_size + DSIZE, max_size);
            print_block(bp);
            return 1;
        }
    }
    else if (list_index == LISTS) { /* No maximum size for last list */
        min_size = get_max_size(list_index - 1);
        if (GET_SIZE(HDRP(bp)) <= min_size) {
            printf("Error: Block size is out of range in list ");
            printf("%d, min size: %d, no max size\n", 
            list_index, min_size + DSIZE);
            print_block(bp);
            return 1;
        }
    }
    else { 
        /* Block size must be larger than maximum size of previous list */
        /* and smaller than its list maximum size */
        max_size = get_max_size(list_index);
        min_size = get_max_size(list_index - 1);
        if (GET_SIZE(HDRP(bp)) <= min_size || GET_SIZE(HDRP(bp)) > max_size) {
            printf("Error: Block size is out of range in list ");
            printf("%d, min size: %d, max size: %d\n", 
            list_index, min_size + DSIZE, max_size);
            print_block(bp);
            return 1;
        }
    }
    
    return 0;
}

/*
 * extend_heap: Extend the heap to acquire more free space. Return the pointer
 *    to the beginning of the newly allocated region.
 *    
 *      This function will mark the newly allocate region as free block and 
 *    add it into the free list. The new epilogue block is created at the end
 *    of the extended space.
 */
static void *extend_heap(size_t words) {
    char *bp;
    size_t size;
    
    /* Allocate an even number of words to maintain alignment */
    size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE;
    
    if (size < MINIMUM_BLK_SIZE) {
        size = MINIMUM_BLK_SIZE;
    }
    
    if ((long)(bp = mem_sbrk(size)) == -1) {
        return NULL;
    }
    
    /* Initialize free block header/footer and the epilogue header */
    PUTW(HDRP(bp), PACK(size, 0));         /* Free block header*/  
    PUTW(FTRP(bp), PACK(size, 0));         /* Free block fotter */
    PUTW(HDRP(NEXT_BLKP(bp)), PACK(0, 1)); /* New epilogue header */
    
    /* Coalesce if the previous block was free */
    return coalesce(bp);
}

/*
 * find_fit: travel through the free lists to find the free block that mactch
 *     with the required size. The function start from getting what list it
 *     should search for the free block of the particular size. If there is no
 *     free block in the list that match the requirement, the function will
 *     search in the list that has bigger bracket of block size. Return null
 *     if there is no block in any list that match the required size.
 * 
 *       The "First Fit" policy is applied to improve the throughput.
 *     Even though the block might be bigger than user need, the place function
 *     will try to split the unused space and insert it into free lists again.
 *     So, we don't waste that much memory
 */
static void *find_fit(size_t a_size) {
    void *bp;
    int list_index;
    list_index = get_list_index(a_size);

    while (list_index <= LISTS) {
        for (bp = GET_LISTP(list_index); 
        bp != NULL && GET_SIZE(HDRP(bp)) > 0; 
        bp = NEXT_FREE_BLKP(bp)) {
            /* Check for size match */
            if (!GET_ALLOC(HDRP(bp)) && (a_size <= GET_SIZE(HDRP(bp)))) {
                return bp;
            }
        } 
        list_index++;
    }
  
    return NULL; /* No fit */
}

/*
 * coalesce: everytime the block is marked as free and will be insert into
 *     free list, the coalescing must be check to see if the adjacent block 
 *     can be merge. This function help us make sure that there is no 
 *     conseccutive free blocks in the heap.
 */
static void *coalesce(void *bp) {
    /* get previous and next blocks status */
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    size_t size = GET_SIZE(HDRP(bp));
    
    if (prev_alloc && next_alloc) { /* previous and next blocks are not free */
        insert_free_block(bp);
        return bp;
    }
    else if (prev_alloc && !next_alloc) { /* next block is free*/
        size += GET_SIZE(HDRP(NEXT_BLKP(bp)));
       
        remove_free_block(NEXT_BLKP(bp));
        
        PUTW(HDRP(bp), PACK(size, 0));
        PUTW(FTRP(bp), PACK(size, 0));
        insert_free_block(bp);
        return bp;
    }
    else if (!prev_alloc && next_alloc) { /* previous block is free*/
        size += GET_SIZE(HDRP(PREV_BLKP(bp)));
        
        remove_free_block(PREV_BLKP(bp));
        
        PUTW(FTRP(bp), PACK(size, 0));
        PUTW(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp);
        insert_free_block(bp);
        return bp;
    }
    else { /* Both previous and next blocks are free */
        size += GET_SIZE(HDRP(PREV_BLKP(bp))) + GET_SIZE(FTRP(NEXT_BLKP(bp)));
        
        remove_free_block(PREV_BLKP(bp));
        remove_free_block(NEXT_BLKP(bp));
        
        PUTW(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        PUTW(FTRP(NEXT_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp);
        insert_free_block(bp);
        return bp;
    }
    
    return bp;
}

/*
 * place: mark the block as allocated when malloc finds the block for user to
 *     use. The block will be mark as allocate and the block size will be 
 *     compared with the required size. If the block size is bigger and the
 *     remaining space is bigger than the minimum block size, we will split
 *     the block so the remaining space can be used in the future.
 */
static void place(void *bp, size_t a_size) {
    size_t csize = GET_SIZE(HDRP(bp));
    
    remove_free_block(bp);
    /* Split the block if possible */
    if ((csize - a_size) >= MINIMUM_BLK_SIZE) {
        PUTW(HDRP(bp), PACK(a_size, 1));
        PUTW(FTRP(bp), PACK(a_size, 1));
        
        /* Free the rest of the block */
        bp = NEXT_BLKP(bp);
        PUTW(HDRP(bp), PACK(csize - a_size, 0));
        PUTW(FTRP(bp), PACK(csize - a_size, 0));
        coalesce(bp);
    }
    else {
        PUTW(HDRP(bp), PACK(csize, 1));
        PUTW(FTRP(bp), PACK(csize, 1));
    }
}

/*
 * insert_free_block: Insert a free block into the free list. The function will
 *     get list index to decide whitch list should it put the block to. The 
 *     block will be put at the beginning of the list for better throughput.
 */
static void insert_free_block(void *bp) {

    int list_index;
    size_t csize = GET_SIZE(HDRP(bp));
    list_index = get_list_index(csize);

    if (GET_LISTP(list_index) == NULL) { /* There is no block in the list */
        /* Set bp as list's root and initialize the list */
        SET_LISTP(list_index, bp);
        SET_NEXT_FREE_BLKP(bp, NULL); 
        SET_PREV_FREE_BLKP(bp, NULL);
    }
    else {
        /* Connect with the current root */ 
        SET_PREV_FREE_BLKP(bp, NULL);
        SET_NEXT_FREE_BLKP(bp, GET_LISTP(list_index));
        SET_PREV_FREE_BLKP(GET_LISTP(list_index), bp);
        
        /* Set bp as list's root */
        SET_LISTP(list_index, bp);
    }
}

/*
 * remove_free_block: remove the block from the free lists. Set the new root
 *   if the removed block is root.
 */
static void remove_free_block(void *bp) {
    
    int list_index;
    size_t csize = GET_SIZE(HDRP(bp));
    list_index = get_list_index(csize);
    
    /* Check if there is previous free block (this node is root)*/
    if (PREV_FREE_BLKP(bp) == NULL) {
        SET_LISTP(list_index, NEXT_FREE_BLKP(bp));
    }
    else {
        SET_NEXT_FREE_BLKP(PREV_FREE_BLKP(bp), NEXT_FREE_BLKP(bp));
    }
    
    /* Set the previous free block pointer of the next block */
    if (NEXT_FREE_BLKP(bp) != NULL) {
        SET_PREV_FREE_BLKP(NEXT_FREE_BLKP(bp), PREV_FREE_BLKP(bp));
    }
}

/*
 * get_list_index: return the index of the free list that contain the block
 *     of the particular size.
 */
static int get_list_index(size_t a_size) {
    if (a_size == 0) {
        return 0;
    }

    if (a_size <= MAX_SIZE_01) {
        return 1;
    }
    else if (a_size <= MAX_SIZE_02) {
        return 2;
    }
    else if (a_size <= MAX_SIZE_03) {
        return 3;
    }
    else if (a_size <= MAX_SIZE_04) {
        return 4;
    }
    else if (a_size <= MAX_SIZE_05) {
        return 5;
    }
    else if (a_size <= MAX_SIZE_06) {
        return 6;
    }
    else if (a_size <= MAX_SIZE_07) {
        return 7;
    }
    else if (a_size <= MAX_SIZE_08) {
        return 8;
    }
    else if (a_size <= MAX_SIZE_09) {
        return 9;
    }
    else if (a_size <= MAX_SIZE_10) {
        return 10;
    }
    else if (a_size <= MAX_SIZE_11) {
        return 11;
    }
    else if (a_size <= MAX_SIZE_12) {
        return 12;
    }
    else {
        return 13;
    }

    return 0;
}

/*
 * get_max_size: return the maximum size of the block that is possible in
 *     particular list. Terminate the program if the spurious request is fed.
 *       
 *       Since the size of each block can't be bigger than 4 byte, 
 *     the unsigned int data type is enough for the returned size.
 */
static unsigned int  get_max_size(int list_index) {
    
    if (list_index < 1 || list_index > LISTS) {
        printf("Error: Invalid list index\n");
        exit(1);
    }
    
    if (list_index == 1) {
        return (unsigned int)MAX_SIZE_01;
    }
    else if (list_index == 2) {
        return (unsigned int)MAX_SIZE_02;
    }
    else if (list_index == 3) {
        return (unsigned int)MAX_SIZE_03;
    }
    else if (list_index == 4) {
        return (unsigned int)MAX_SIZE_04;
    }
    else if (list_index == 5) {
        return (unsigned int)MAX_SIZE_05;
    }
    else if (list_index == 6) {
        return (unsigned int)MAX_SIZE_06;
    }
    else if (list_index == 7) {
        return (unsigned int)MAX_SIZE_07;
    }
    else if (list_index == 8) {
        return (unsigned int)MAX_SIZE_08;
    }
    else if (list_index == 9) {
        return (unsigned int)MAX_SIZE_09;
    }
    else if (list_index == 10) {
        return (unsigned int)MAX_SIZE_10;
    }
    else if (list_index == 11) {
        return (unsigned int)MAX_SIZE_11;
    }
    else if (list_index == 12) {
        return (unsigned int)MAX_SIZE_12;
    }
    else {
        return 0; /* No max size for last list */
    }

    return 0;
}

/************************************** 
 End of my utility functions
***************************************/