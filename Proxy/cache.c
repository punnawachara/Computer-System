/* 
 * Name: Punnawachara Campanang
 * Andrew ID: pcampana
 * 
 * cache.c: implementation of cache.h and the helper functions used in cache
 * 
 * Eviction policy: LRU
 * 
 * Insert policy: Always insert most recently used or new block at the root.
 *     Thus, the LRU block will always be at the end of the block list.
 * 
 * Prioritization: Readers has higher priority
 */

#include "cache.h"

/* Functions prototype used only in cache.c */
static cache_block *create_block(char *input_host, 
char *input_uri, void *buffer, int size);

static void free_block(cache_block *block_ptr);
static void insert_block(proxy_cache *my_cache, cache_block *block_ptr);
static void remove_block(proxy_cache *my_cache, cache_block *block_ptr);
static cache_block *search_block(proxy_cache *my_cache, 
char *input_host, char* input_uri);

static int read_cache_block(cache_block *block_ptr, void *buffer);
static void lru_update(proxy_cache *my_cache, cache_block *block_ptr);
static cache_block *get_lru(proxy_cache *my_cache);
static void eviction(proxy_cache *my_cache);

/* Functions */

/*
 * init_cache: Initialize the cache for proxy use. User can specify the
 *      cache size and maximum object size.
 */
proxy_cache 
*init_cache(int max_cache_size, int input_max_object_size) {
    /* Allocate space */
    proxy_cache *my_cache = (proxy_cache *)Malloc(sizeof (proxy_cache));
    
    if (my_cache == NULL) {
        return NULL;
    }
    
    /* Init the variables */
    my_cache->space = max_cache_size;
    my_cache->max_object_size = input_max_object_size;
    my_cache->root = NULL;
    
    /* Init semaphores */
    my_cache->readcnt = 0;
    Sem_init(&my_cache->mutex_read, 0, 1);
    Sem_init(&my_cache->mutex_write, 0, 1);
    
    return my_cache;
}

/*
 * create_block: Accquire memory space for content that will be stored
 *      in the cache.
 * 
 * return block pointer if success, NULL if not enough sapce
 */
cache_block 
*create_block(char *input_host, char *input_uri, void *buffer, int size) {
    /* Allocate space */
    cache_block *block_ptr = (cache_block *)Malloc(sizeof(cache_block));
    
    /* If memory is full, return NULL*/
    if (block_ptr == NULL) {
        return NULL;
    }
    
    /* Primary key initialization */
    block_ptr->host = (char *)Malloc((strlen(input_host)+1) * sizeof(char));
    block_ptr->uri = (char *)Malloc((strlen(input_uri)+1) * sizeof(char));
    
    /* If memory is full, return NULL */
    if (block_ptr->host == NULL || block_ptr->uri == NULL) {
        return NULL;
    }
    
    strcpy(block_ptr->host, input_host);
    strcpy(block_ptr->uri, input_uri);
    
    /* Initialization for linked list */
    block_ptr->next_cache_block = NULL;
    block_ptr->prev_cache_block = NULL;
    
    /* Payload initialization */
    block_ptr->payload_size = size;
    block_ptr->payload = (void *)Malloc((size) * sizeof(char));
    
    /* If memory is full, return NULL */
    if (block_ptr->payload == NULL) {
        return NULL;
    }
    
    memcpy((void *)(block_ptr->payload), (void *)buffer, size);
    
    return block_ptr;
}

/*
 * free_block: Free the memory space accquired by the block for future use.
 * 
 */
void 
free_block(struct cache_block *block_ptr) {
    Free(block_ptr->host);
    Free(block_ptr->uri);
    Free(block_ptr->payload);
    Free(block_ptr);
}

/*
 * insert_block: Put block into linked list and update the space
 */
void 
insert_block(proxy_cache *my_cache, cache_block *block_ptr) {
    
    if (my_cache->root == NULL) { /* There is no block in the list */
        /* Set block_ptr as cache's root and initialize the list */
        my_cache->root = block_ptr;
        block_ptr->next_cache_block = NULL;
        block_ptr->prev_cache_block = NULL;
    }
    else {
        /* Connect with the current root */
        block_ptr->prev_cache_block = NULL;
        block_ptr->next_cache_block = my_cache->root;
        my_cache->root->prev_cache_block = block_ptr;
        
        /* Set block_ptr as cache's root */
        my_cache->root = block_ptr;
    }
    
    /* Update remaining space */
    my_cache->space -= block_ptr->payload_size;
}

/*
 * remove_block: Take the block out of linked list and update the space
 * 
 */
void 
remove_block(proxy_cache *my_cache, cache_block *block_ptr) {
    
    /* Check if there is previous block (this node is root)*/
    if (block_ptr->prev_cache_block == NULL) {
        my_cache->root = block_ptr->next_cache_block;
    }
    else {
        block_ptr->prev_cache_block->next_cache_block = 
        block_ptr->next_cache_block;
    }
    
    /* Set the previous block pointer of the next block */
    if (block_ptr->next_cache_block != NULL){
        block_ptr->next_cache_block->prev_cache_block = 
        block_ptr->prev_cache_block;
    }
    
    /* Update remaining space */
    my_cache->space += block_ptr->payload_size;
}

/*
 * search_block: Return the pointer of the block that has the content of the
 *      request host and port from client.
 */
cache_block 
*search_block(proxy_cache *my_cache, char *input_host, char* input_uri) {
    cache_block *block_ptr;
    
    for (block_ptr = my_cache->root; block_ptr != NULL; block_ptr = 
    block_ptr->next_cache_block) {
        
        if (!strcmp(block_ptr->host, input_host) && 
        !strcmp(block_ptr->uri, input_uri)) {
            return block_ptr;
        }
        
    }
    
    return NULL;
}

/*
 * read_cache_block: Extract the payload to buffer, check for false request
 * 
 * return length if success, -1 if error
 */
int 
read_cache_block(cache_block *block_ptr, void *buffer) {
    
    /* Ignore spurious request */
    if (block_ptr == NULL) {
        return -1;
    }
    
    /* Read to buffer */
    memcpy((void *)buffer, 
    (void *)block_ptr->payload, block_ptr->payload_size);
    
    return block_ptr->payload_size;
}

/*
 * lru_update: Put the most recently
 *      used block at the beginning of the linked list (with synchroniztion).
 * 
 */
void
lru_update(proxy_cache *my_cache, cache_block *block_ptr) {
    
    /* Rearrange the linked list (take out and re-insert as root) */
    P(&my_cache->mutex_write); /* Need wrtie permission */
    remove_block(my_cache, block_ptr);
    insert_block(my_cache, block_ptr);
    V(&my_cache->mutex_write);

}

/*
 * get_lru: Return the pointer to the last block in the linked list. According
 *      to our insert policy, the last block is always LRU block.
 */
cache_block 
*get_lru(proxy_cache *my_cache) {
    cache_block *block_ptr;
    
    /* If the cache is empy, just return */
    if ((block_ptr = my_cache->root) == NULL) {
        return NULL;
    }
    
    /* LRU block is last block in the linked list (LRU->next = NULL)*/
    while (block_ptr->next_cache_block != NULL) {
        block_ptr = block_ptr->next_cache_block;
    }
    
    return block_ptr;
}

/*
 * eviction: Remove the last block in the linked list 
 *      and free the memory space
 * 
 */
void 
eviction(proxy_cache *my_cache) {
    cache_block *lru_block;
    lru_block = get_lru(my_cache);
    remove_block(my_cache, lru_block);
    free_block(lru_block);
}

/*
 * read_cache: Search cache by using host and uri as primary key. If the block
 *      is found, copy the block content to the buffer then rearrange the
 *      linked list to maintain LRU order. Many readers may read the block at
 *      the same time but only 1 can move or delete block. Thus, rearrange
 *      cache list is considered as write operation and need to wait for write
 *      permission.
 * 
 * return payload length = hit, -1 = miss
 */
int 
read_cache(proxy_cache *my_cache, 
char *input_host, char *input_uri, void *buffer) {
    int read_len;
    cache_block *block_ptr;
    
    /* Can't read if no cahce */
    if (my_cache == NULL) {
        return -1;
    }
    
    /* Semaphores */
    P(&my_cache->mutex_read);
    my_cache->readcnt += 1;
    if (my_cache->readcnt == 1) { /* First reader locks the write flag */
        P(&my_cache->mutex_write);
    }
    V(&my_cache->mutex_read);
    
    if ((block_ptr = search_block(my_cache, input_host, input_uri)) == NULL) {
    /* Cache Miss */
        
        /* Semaphores */
        P(&my_cache->mutex_read);
        my_cache->readcnt -= 1;
        if (my_cache->readcnt == 0) { /* First reader locks write flag */
            V(&my_cache->mutex_write);
        }
        V(&my_cache->mutex_read);
        
        return -1;
    }
    
    /* read to buffer */
    read_len = read_cache_block(block_ptr, buffer);
    
    /* Semaphores */
    P(&my_cache->mutex_read);
    my_cache->readcnt -= 1;
    if (my_cache->readcnt == 0) { /* Last reader unlocks write flag */
        V(&my_cache->mutex_write);
    }
    V(&my_cache->mutex_read);
    
    /* Update LRU order */
    lru_update(my_cache, block_ptr);
    
    return read_len;
}

/*
 * write_cache: Write the content to the cache with synchronization.
 *      Only 1 writer is allowed to write cache at a time.
 * 
 * return 1 = success, -1 = error
 */
int 
write_cache(proxy_cache *my_cache, 
char *input_host, char *input_uri, void *buffer, int len) {
    cache_block *block_ptr;
    
    /* Ignore spurious request */
    if (my_cache == NULL) {
        return -1;
    }
    
    /* Check length validity */
    if (len > my_cache->max_object_size) {
        return -1;
    }
    
    /* Semaphores: Lock write permission*/
    P(&my_cache->mutex_write);
    
    /* If there is not enough space, keep deleting LRU block */
    while (my_cache->space < len) {
        eviction(my_cache);
    }
    
    /* Create the block and write the content */
    block_ptr = create_block(input_host, input_uri, buffer, len);
    
    /* Check for block validation */
    if (block_ptr == NULL) {
        /* Semaphores: Unlock write permission */
        V(&my_cache->mutex_write);
        return -1;
    }
    
    /* Insert to linked list */
    insert_block(my_cache, block_ptr);
    
    /* Semaphores: Unlock write permission */
    V(&my_cache->mutex_write);
    
    return 1;
}

