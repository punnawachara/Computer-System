/*
 * Name: Punnawachara Campanang
 * Andrew ID: pcampana
 * 
 * proxy.c: This is a simple proxy server that handle HTTP Get request from
 *      client by forwarding the request to server (with modified headers).
 *      After that, this proxy will receive the responses from server and 
 *      redirect response back to the client. If cache is enable, the proxy
 *      will cache the object from remote server response in cache for future
 *      use if the client request for the same object.
 * 
 * Debug: The proxy can run with debug message if the DEBUG is set to 1. The
 *      proxy will display the request and response message from client and 
 *      remote server. If the SHOW_CONTENT is set, the proxy will show the 
 *      response body that it will forward to the client.
 * 
 * Cache use: Cache is enable by default. To disable cache, type disable in
 *      <cache_status> when you run program: ./proxy <port> <cache_status>.
 *      
 *      The cache is maintained by using LRU eviction policy and implemented
 *      by using 2-way linked list (as same as explicit list in malloc lab).
 *      please see cache.h and cache.c for more detail.
 * 
 * Erro Handling: Error handling is done by using wrapper functions
 *      modified from csapp.c
 *      - openclientfd_r for thread safety
 *      - Rio_readlineb_r to proper handle the error when errno = EPIPE
 *      - Rio_readnb_r to proper handle the error when errno = EPIPE
 *      - Rio_writen_r to proper handle the error when errno = ECONNRESET
 *      Moreover, the exit() functions is removed from the following functions
 *      in csapp.c to make sure that the server will not terminate but instead
 *      exit the thread when such errors happen.
 *      - unix_error
 *      - posix_error
 *      - dns_error
 *      - app_error
 *      Finally, SIGPIPE is ignored.
 * 
 * Concurency: Concurency is handled by using thread function.
 * 
 * Synchronization: Synchronization is handled by using semaphores for cache
 *      access. This proxy uses the reader and writer model and gives the
 *      higher priority to the readers. Many readers may read the cache at the
 *      time but only one writeer can write to the cache.
 */
#include "csapp.h"
#include "cache.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define DEBUG 0 /* Turn on if you want the server to show the debug messages */
#define SHOW_CONTENT 0 /* Turn on to show response body in debug mode */

/* You won't lose style points for including these long lines in your code */
/* Default value for required headers */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *accept_hdr = "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n";
static const char *accept_encoding_hdr = "Accept-Encoding: gzip, deflate\r\n";

/* Additional header string */
static const char *connection_hdr = "Connection: close\r\n";
static const char *proxy_connection_hdr = "Proxy-Connection: close\r\n";

/* Default protocol and port */
static const char *default_protocol = "http";
static const char *default_port = "80";

/* Force using HTTP/1.0 version (set to 1 if want to use HTTP/1.0)*/
static int use_old_version = 1;

/* Global variables for cache */
static proxy_cache *my_cache = NULL;
static int cache_enable = 1; /* Cache is on my default */

/*****************************************************************************
 * Function prototype
 *****************************************************************************/
static void doit(int connfd);
static int parse_request(char *req, 
char *method, char *protocol, char *host, char *uri, char *port, char *ver);

static void construct_request_header(rio_t *rio, 
char *host, char *port, char *proxy_reqhdr);

static int open_clientfd_r(char *hostname, char *port);
static void *thread(void *vargp);
static void *end_of_content(void* content, int length);
static ssize_t Rio_readnb_r(rio_t *rp, void *usrbuf, size_t n);
static ssize_t Rio_readlineb_r(rio_t *rp, void *usrbuf, size_t maxlen);
static ssize_t Rio_writen_r(int fd, void *usrbuf, size_t n);
/*****************************************************************************
 * End function prototype
 *****************************************************************************/

/*
 * main: Proxy main routine, mainly accept the connection and spwn thread.
 *      If error occur during the initialization, progra, will exit.
 */
int 
main(int argc, char **argv) {
    int listenfd, port, clientlen;
    int *connfd_ptr;
    struct sockaddr_in clientaddr;
    pthread_t tid;
    
    /* Ignore SIGPIPE */
    Signal(SIGPIPE, SIG_IGN);
    
    /* Check command line args */
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <port> <cahche_status>\n", argv[0]);
        exit(1);
    }
    
    /* Check port */
    if ((port = atoi(argv[1])) == 0){
        fprintf(stderr, "Invalid port number\n");
        exit(1);
    }
    
    /* Prepare cahce */
    if (argc == 3){
        /* When receive "disable, don't use cache" */
        if (!strcmp(argv[2], "disable")) {
            cache_enable = 0;
        }
    } else { /* If not specified or else, always use cache */
        cache_enable = 1;
    }
    
    /* Initialize cahce */
    if (cache_enable) {
        my_cache = init_cache(MAX_CACHE_SIZE, MAX_OBJECT_SIZE);
        if (my_cache == NULL) {
            fprintf(stderr, "Can't initialize cache, please try again\n");
            exit(1);
        }
    }
    
    /* Get socket descriptor */
    if ((listenfd = Open_listenfd(port)) < 0) {
        fprintf(stderr, "Listen error\n");
        exit(1);
    }
    
    while (1) {
        clientlen = sizeof(clientaddr);
        connfd_ptr = (int*)Malloc(sizeof(int));
        
        if (connfd_ptr != NULL) { /* Check if malloc fail to avoid segfault */
            *connfd_ptr = 
            Accept(listenfd, (SA *)&clientaddr, (socklen_t *)&clientlen);
            Pthread_create(&tid, NULL, (void *)thread, (void *)connfd_ptr);
        }
    }
}

/*****************************************************************************
 * Helper functions
 *****************************************************************************/

/*
 * thread: Perform concurent request handling.
 */
void 
*thread(void *vargp) {
    Pthread_detach(pthread_self());
    int connfd;
    
    if (vargp == NULL) {
        fprintf(stderr, "Connfd error\n");
        Pthread_exit(NULL);
    }
    
    connfd = *((int *)vargp);
    Free(vargp);
    doit(connfd);
    
    /* Safely close connection */
    if (connfd >= 0 ) {
        Close(connfd);
    }
    return NULL;
}

/*
 * doit: Handle the request from client. will handle only GET request.
 *      The process is as follow:
 *      - Read client request
 *      - search cache if enable
 *      - if cache hit, return the content to client
 *      - if cache miss, forward client request to server then receive the 
 *          response from server and send back to client.
 *      - if the response from server is not too big, write data to cache.
 */
void 
doit(int connfd) {
    /* Request from client */
    char client_request[MAXLINE];
    
    /* Parameter obtain by parsing client request */
    char method[MAXLINE];
    char protocol[MAXLINE]; /* for future use */
    char host[MAXLINE];
    char uri[MAXLINE];
    char port[MAXLINE];
    char version[MAXLINE];
    
    /* IO */
    int proxyfd; /* Connect to remote server */
    rio_t rio_server; /* Connect to remote server */
    rio_t rio_client; /* Connect to our client */
    
    /* For building request the remote server */
    char proxy_reqln[MAXLINE];
    char proxy_reqhdr[MAXLINE];
    
    /* For sending response to our client */
    char server_response[MAXLINE];
    unsigned int read_len = 0;
    
    /* For cache */
    char cache_content[MAX_OBJECT_SIZE];
    int cache_read_len = -1;
    int cache_write_len = 0;
    
    /* Initailize cache content */
    memset((void *)cache_content, 0, MAX_OBJECT_SIZE);
    
    /* Read client request line */
    Rio_readinitb(&rio_client, connfd);
    
    /* Read request from client, return if error */
    if (Rio_readlineb_r(&rio_client, client_request, MAXLINE) < 0) {
        return;
    }
    
    /* Parse client_request and get parameters*/
    if (parse_request(client_request, 
    method, protocol, host, uri, port, version) == -1) {
        /* Return if parsing fail */
        return;
    }
    
    /* Ignore non GET request */
    if (strcasecmp(method, "GET")) {
        /* Not reponsible for other method. Kill thread! */ 
        if (connfd >= 0) {
            Close(connfd);
        }
        Pthread_exit(NULL);
    }                                                    
    
    /* Search cache if cache is enable */
    if (cache_enable) {
        cache_read_len = read_cache(my_cache, host, uri, cache_content);
    }
    
    /* Request process */
    if (cache_read_len < 0) { /* Cache miss or unused, forward request */
        
        if (DEBUG) {
            fprintf(stdout, "Cache MISS: host: %s, uri: %s\n", host, uri);
            fprintf(stdout, "*****Process request regularly*****\n\n");
        }
        /* Forward client request to server */
        /* Construct request lines */
        sprintf(proxy_reqln, "%s %s %s\r\n", method, uri, version);
        
        /* Construct header lines */
        construct_request_header(&rio_client, host, port, proxy_reqhdr);
        
        /* Get channel fd to contact with remote server*/
        proxyfd = open_clientfd_r(host, port);
        
        if (proxyfd < 0) { /* Can't connect to server */
            return;
        }
        
        /* Forward request line to remote server */
        if (Rio_writen_r(proxyfd, proxy_reqln, strlen(proxy_reqln)) < 0) {
            /* Close connection with remote server and return if error */
            if (proxyfd >= 0) {
                Close(proxyfd);
            }
            return;
        }
        
        if (DEBUG) { // Display request line
            fprintf(stdout, "%s", proxy_reqln);
        }
        
        /* Forward header lines to the remote server */
        if (Rio_writen_r(proxyfd, proxy_reqhdr, strlen(proxy_reqhdr)) < 0) {
            /* Close connection with remote server and return if error */
            if (proxyfd >= 0) {
                Close(proxyfd);
            }
            return;
        }
        
        if (DEBUG) { // Display Headers
            fprintf(stdout, "%s", proxy_reqhdr);
        }
        
        /* Get response from remote server */
        /* IO init */
        Rio_readinitb(&rio_server, proxyfd);
        
        /* Read response line from server*/
        if ((read_len = Rio_readlineb_r(&rio_server, 
        server_response, MAXLINE)) < 0) {
            /* Close connection with remote server and return */
            if (proxyfd >= 0) {
                Close(proxyfd);
            }
            return;
        }
        
        if (DEBUG) { // Display server response line
            fprintf(stdout, "**********Server Response**********\n\n");
            fprintf(stdout, "%s", server_response);
        }
        
        /* Put data in cache if available */
        if (cache_enable) {
            if (cache_write_len + read_len <= MAX_OBJECT_SIZE) {
                /* Accumulate length and content */
                memcpy(end_of_content(cache_content, cache_write_len), 
                (void *)server_response, read_len);
            }
            /* Keep track of total response size */
            cache_write_len += read_len;
        }
        
        /* Send response line to client */
        if (Rio_writen_r(connfd, server_response, read_len) < 0) {
            /* Close connection with remote server and return if error */
            if (proxyfd >= 0) {
                Close(proxyfd);
            }
            return;
        }
        
        /* Response headers processing*/
        while(1) {
            
            /* Keep reading header from server */
            if ((read_len = Rio_readlineb_r(&rio_server, 
            server_response, MAXLINE)) < 0) {
                
                /* Close connection with remote server and return if error */
                if (proxyfd >= 0) {
                    Close(proxyfd);
                }
                return;
            }
            
            if (DEBUG) { // Display response headers
                fprintf(stdout, "%s", server_response);
            }
            
            /* Put data in cache if available */
            if (cache_enable) {
                
                if (cache_write_len + read_len <= MAX_OBJECT_SIZE) {
                    /* Accumulate length and content */
                    memcpy(end_of_content(cache_content, cache_write_len), 
                    (void *)server_response, read_len);
                }
                cache_write_len += read_len;
            }
            
            /* Forward to client */
            if (Rio_writen_r(connfd, server_response, read_len) < 0) {
                /* Close connection with remote server and return if error */
                if (proxyfd >= 0) {
                    Close(proxyfd);
                }
                return;
            }
            
            /* Stop after sending all headers to client (include \r\n line) */
            if(strcmp(server_response,"\r\n") == 0) {
                break;
            }
        }
        
        /* Response body processing*/
        while((read_len = Rio_readnb_r(&rio_server, 
        server_response, MAXLINE)) > 0){
            
            if (DEBUG) { // display response body
                if (SHOW_CONTENT) {
                    fprintf(stdout, "%s", server_response);
                }
            }
            
            /* Put data in cache if available */
            if (cache_enable) {
                if (cache_write_len + read_len <= MAX_OBJECT_SIZE) {
                    /* Accumulate length and content */
                    memcpy(end_of_content(cache_content, cache_write_len), 
                    (void *)server_response, read_len);
                }
                cache_write_len += read_len;
            }
            
            if (Rio_writen_r(connfd, server_response, read_len) < 0) {
                /* Close connection with remote server and return if error */
                if (proxyfd >= 0) {
                    Close(proxyfd);
                }
                return;
            }
        }
        
        /* Write to cache if possible*/
        if (cache_enable) {
            if (cache_write_len <= MAX_OBJECT_SIZE) {
                
                if (DEBUG) { // Display cache process
                    fprintf(stdout, "Try to write to cache\n");
                    fprintf(stdout, "Content Length: %d\n", cache_write_len);
                }
                
                if (write_cache(my_cache, host, uri, (void*) cache_content, 
                cache_write_len) < 0) {
                    if (DEBUG) {
                        fprintf(stdout, "Write Fail\n");
                    }
                }
                else {
                    if (DEBUG) {
                        fprintf(stdout, "Write Success\n");
                    }
                }
            }
        }
        
        /* Success, safely close the connection with remote server */
        if (proxyfd >= 0 ) {
            Close(proxyfd);
        }
    }
    else { /* Cache Hit, reply to client */
        if (DEBUG) { //Desplay cache content
            fprintf(stdout, "Cache HIT!\n");
            if (SHOW_CONTENT) {
                fprintf(stdout, "Payload:\n%s\nLength: %d\n", 
                cache_content, cache_read_len);
            }
        }
        
        /* Send cache content back to user */
        if (Rio_writen_r(connfd, cache_content, cache_read_len) < 0) {
            return;
        }
    }
}

/*
 * parse_request: parse the request from client from METHOD URL VERSION 
 *      into small components used to construct the request line.
 * 
 * return 1 = success, -1 = error
 */
int 
parse_request(char *req, char *method, 
char *protocol, char *host, char *uri, char *port, char *ver) {
    char url[MAXLINE];
    char host_port_uri[MAXLINE];
    char host_port[MAXLINE];
    
    /* Invalid request length */
    if (strlen(req) < 1) {
        return -1;
    }
    
    /* Decompose into 3 parts */
    sscanf(req, "%s %s %s", method, url, ver);
    
    /* Version check */
    if (strstr(ver, "/") == NULL) {
        return -1;
    }
    
    /* Decompose url to get protocol, host, uri, port */
    /* Look for protocol */
    if (strstr(url, "://") == NULL) {
        sscanf(url, "%s", host_port_uri);
        
        /* Apply default protocol */
        strcpy(protocol, default_protocol);
    }
    else {
        sscanf(url, "%[^:]://%s", protocol, host_port_uri);
    }
    
    strcpy(uri, "/");
    
    /* Look for the port and uri (if possible) */
    if (strstr(host_port_uri, "/") != NULL){ /* Extract uri */
        sscanf(host_port_uri, "%[^/]%s", host_port, uri);
    }
    else {
        sscanf(host_port_uri, "%s", host_port);
    }
    
    /* Split host and port (if port is included) */
    if ((strstr(host_port, ":")) != NULL){ /* Extract port */
        sscanf(host_port, "%[^:]:%s", host, port);
        
        /* double chekc port to if it is empty port*/
        if (atoi(port) == 0) {
            /* Apply fefault port if port is empty */
            strcpy(port, default_port);
        }
    }
    else {
        sscanf(host_port, "%s", host);
        /* Apply default port */
        strcpy(port, default_port);
    }
    
    /* Force using HTTP/1.0 if desired */
    if (use_old_version) {
        strcpy(ver, "HTTP/1.0");
    }
    
    /* Empty host*/
    if (!strcmp(host, "")) {
        return -1;
    }
    
    return 1;
}

/*
 * construct_request_header: scanning the request from client to filter
 *      the headers. All of required header (Host, User-Agent, Accept, 
 *      Accept-Encoding, Connection and Proxy-Connection) will be modified to
 *      default value. Other headers from client will be forwarded normally.
 */
void 
construct_request_header(rio_t *rio, 
char *host, char *port, char *proxy_reqhdr) {
    /* Header provided by client */
    int host_hdr = 0;
    int user_agent = 0;
    int accept = 0;
    int accept_encoding = 0;
    int connection = 0;
    int proxy_connection = 0;
    char client_header[MAXLINE]; /* Buffer */
    char host_buf[MAXLINE]; /* For bulding Host: header */
    
    /* Keep reading headers from client until we reach the empty line */
    while (Rio_readlineb_r(rio, client_header, MAXLINE) != 0) {
        /* According to RFC 2616, the sequence of header doesn't matter */
        if (strcmp(client_header, "\r\n") == 0) {
            break;
        }
        else if (strstr(client_header, "Host:") != NULL) {
            strcat(proxy_reqhdr, client_header);
            host_hdr = 1;
        }
        else if (strstr(client_header, "User-Agent:") != NULL) {
            strcat(proxy_reqhdr, user_agent_hdr);
            user_agent = 1;
        }
        else if (strstr(client_header, "Accept-Encoding:") != NULL) {
            strcat(proxy_reqhdr, accept_encoding_hdr);
            accept_encoding = 1;
        }
        else if (strstr(client_header, "Accept:") != NULL) {
            strcat(proxy_reqhdr, accept_hdr);
            accept = 1;
        }
        else if (strstr(client_header, "Proxy-Connection:") != NULL) {
            strcat(proxy_reqhdr, proxy_connection_hdr);
            proxy_connection = 1;
        }
        else if (strstr(client_header, "Connection:") != NULL) {
            strcat(proxy_reqhdr, connection_hdr);
            connection = 1;
        }
        else { /* Other types of header tht is not mentioned in the writeup*/
            strcat(proxy_reqhdr, client_header);
        } 
    }
    
    /* Add the missing required header */
    /* Host */
    if(!host_hdr) {
        sprintf(host_buf, "Host: %s:%s\r\n", host, port);
        strcat(proxy_reqhdr, host_buf);
    }
    /* User-Agent */
    if (!user_agent) {
        strcat(proxy_reqhdr, user_agent_hdr);
    }
    /* Accept-Enconding */
    if (!accept_encoding) {
        strcat(proxy_reqhdr, accept_encoding_hdr);
    }
    /* Accept */
    if (!accept) {
        strcat(proxy_reqhdr, accept_hdr);
    }
    /* Proxy-Connection */
    if (!proxy_connection) {
        strcat(proxy_reqhdr, proxy_connection_hdr);
    }
    /* Connection */
    if (!connection) {
        strcat(proxy_reqhdr, connection_hdr);
    }
    /*  End header lines with \r\n */
    strcat(proxy_reqhdr, "\r\n");
}

/*
 * open_clientfd_r - thread-safe version of open_clientfd
 */
int 
open_clientfd_r(char *hostname, char *port) {
    int clientfd;
    struct addrinfo *addlist, *p;
    int rv;

    /* Create the socket descriptor */
    if ((clientfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        return -1;
    }

    /* Get a list of addrinfo structs */
    if ((rv = getaddrinfo(hostname, port, NULL, &addlist)) != 0) {
        return -1;
    }
  
    /* Walk the list, using each addrinfo to try to connect */
    for (p = addlist; p; p = p->ai_next) {
        if ((p->ai_family == AF_INET)) {
            if (connect(clientfd, p->ai_addr, p->ai_addrlen) == 0) {
                break; /* success */
            }
        }
    } 

    /* Clean up */
    freeaddrinfo(addlist);
    if (!p) { /* all connects failed */
        close(clientfd);
        return -1;
    }
    else { /* one of the connects succeeded */
        return clientfd;
    }
}

/*
 * end_of_content: return the pointer to the end of content so that we can use
 *      memset() to concat the cache payload.
 */
void 
*end_of_content(void *content, int length) {
    return (void *)(content + length);
}

/* 
 * Rio_readnb_r: Modified Rio_readnb from CSAPP.
 *      This verion will not call error if erro = EPIPE.
 */
ssize_t 
Rio_readnb_r(rio_t *rp, void *usrbuf, size_t n) {
    ssize_t rc;

    if ((rc = rio_readnb(rp, usrbuf, n)) < 0) {
        if(errno != ECONNRESET){
            unix_error("Rio_readnb error");
        }
    }
    
    return rc;
}


/* 
 * Rio_readlineb_r: Modified Rio_readlineb from CSAPP.
 *      This verion will not call error if erro = EPIPE.
 */
ssize_t 
Rio_readlineb_r(rio_t *rp, void *usrbuf, size_t maxlen) {
    ssize_t rc;

    if ((rc = rio_readlineb(rp, usrbuf, maxlen)) < 0) {
        if(errno != ECONNRESET){
            unix_error("Rio_readlineb error");
        }
    }
    
    return rc;
} 

/* 
 * Rio_writen_r: Modified Rio_writen from CSAPP. This verion will return rc.
 *      of the content read and will not call error if erro = EPIPE
 */
ssize_t 
Rio_writen_r(int fd, void *usrbuf, size_t n) {
    ssize_t rc;

    if ((rc = rio_writen(fd, usrbuf, n)) != n){
        if(errno != EPIPE){
            unix_error("Rio_writen error");
        }
    }
    
    return rc;
}

/*****************************************************************************
 * End helper functions
 *****************************************************************************/