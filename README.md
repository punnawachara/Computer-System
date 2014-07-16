Computer-System
===============

My works from Introduction to Computer System Course at CMU (15213)

Malloc folder contains the implementation of my version of memory allocation function "malloc" in C
The source code can be seen in the file mm.c. This malloc function uses segregated list technique to manage the 
free blocks space in heap as well as coalescing technique to combine free blocks that are close to each other.

Proxy folder contains the implementation of multi-thread proxy server with cache in C. 
The source code for proxy server is in proxy.c file.
The source code for cache is in cache.c and cache.h files. 2-way linked list is used to implement LRU cache.
