/*
 * Name: Punnawachara Campanag
 * Andrew ID: pcampana
 * 
 * cache.h: header file for cache function used in proxy server
 *     This cache is implemented in the same way as explicit list in malloc 
 *     lab. We maintain the linked list of the occupied blocks
 *     and keep updating.
 * 
 * This cache uses 2 data struct. First is the proxy_cache. This data 
 *      structure will be access by proxy.c. The second is cache_block. 
 *      This is the block of linked list used to store data.
 * 
 * Eviction policy: LRU
 * 
 * Insert policy: Always insert most recently used or new block at the root.
 *     Thus, the LRU block will always be at the end of the block list.
 * 
 * Prioritization: Readers has higher priority
 */
 
#include "csapp.h"

typedef struct proxy_cache {
    /* To manage the cache list */
    unsigned int space; /* the remaining space in cache */
    unsigned int max_object_size;
    struct cache_block *root; /* Pointer to the first block */
    
    /* Semaphores */
    unsigned int readcnt;
    sem_t mutex_read;
    sem_t mutex_write;
} proxy_cache;

typedef struct cache_block {
    int payload_size;
    char *host; /* For searching */
    char *uri;  /* For searching */
    struct cache_block *next_cache_block;
    struct cache_block *prev_cache_block;
    void *payload;
} cache_block;

/* Functions used in proxy.c*/
proxy_cache *init_cache(int max_cache_size, int input_max_object_size);
int read_cache(proxy_cache *my_cache, char *input_host, 
char *input_uri, void *buffer);

int write_cache(proxy_cache *my_cache, char *input_host, 
char *inut_uri, void *buffer, int len);
