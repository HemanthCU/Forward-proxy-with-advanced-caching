/* 
 * proxyserver.c - A program to implement a forward proxy with caching and link prefetching
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>      /* for fgets */
#include <strings.h>     /* for bzero, bcopy */
#include <unistd.h>      /* for read, write */
#include <sys/socket.h>  /* for socket use */
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <openssl/md5.h>

#define LISTENQ  1024
#define MAXREAD  80000
#define HOSTLEN  100

int open_listenfd(int port);
int open_sendfd(int port, char *host);
void echo(int connfd);
void *thread(void *vargp);
char* getFType(char *tgtpath);
char* hostname_to_ip(char *hostname);
char* checkCache(char *url);
char* str2md5(const char *str, int length);
void *threadlpf(void *vargp);

struct lpfwrapper {
    char *fname;
    char *hostname;
};

char ***cacheList;
char ***ipcache;
int cacheLen;
int ipcacheLen;
int timelimit = 600;
pthread_mutex_t mtx=PTHREAD_MUTEX_INITIALIZER;

int main(int argc, char **argv) {
    int i, j, listenfd, *connfdp, port, clientlen=sizeof(struct sockaddr_in);
    cacheList = (char***) malloc (400 * sizeof(char**));
    ipcache = (char***) malloc (400 * sizeof(char**));
    for (i = 0; i < 400; i++) {
        cacheList[i] = (char**) malloc (3 * sizeof(char*));
        ipcache[i] = (char**) malloc (2 * sizeof(char*));
        for (j = 0; j < 3; j++) {
            cacheList[i][j] = (char*) malloc (HOSTLEN * sizeof(char));
            bzero(cacheList[i][j], HOSTLEN);
        }
        for (j = 0; j < 2; j++) {
            ipcache[i][j] = (char*) malloc (HOSTLEN * sizeof(char));
            bzero(ipcache[i][j], HOSTLEN);
        }
    }
    cacheLen = 0;
    ipcacheLen = 0;
    struct sockaddr_in clientaddr;
    pthread_t tid; 

    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> <timeout>\n", argv[0]);
        exit(0);
    }
    port = atoi(argv[1]);
    timelimit = atoi(argv[2]);

    listenfd = open_listenfd(port);
    while (1) {
        connfdp = malloc(sizeof(int));
        *connfdp = accept(listenfd, (struct sockaddr*)&clientaddr, &clientlen);
        // Create a new thread only if a new connection arrives
        if (*connfdp >= 0)
            pthread_create(&tid, NULL, thread, connfdp);
    }
}

char* getFType(char *tgtpath) {
    int i;
    char *temp1 = (char*) malloc (100*sizeof(char));
    char *temp2 = (char*) malloc (100*sizeof(char));
    char *temp3 = NULL;
    strcpy(temp1, tgtpath);
    temp3 = strrchr(temp1, '.');
    if (temp3 == NULL) {
        printf("ERROR in file type\n");
        return NULL;
    }
    temp2 = temp3 + 1;
    for (i = 0; i < strlen(temp2); i++) {
        if (!(temp2[i] >= 'a' && temp2[i] <= 'z') && !(temp2[i] >= 'A' && temp2[i] <= 'Z'))
            return "html";
    }
    return temp2;
}

char* checkCache(char *url) {
    int i;
    for (i = 0; i < cacheLen; i++) {
        time_t rawtime;
        time(&rawtime);
        if(strcmp(url, cacheList[i][0]) == 0 && (rawtime - atol(cacheList[i][2]) < timelimit)) {
            //printf("%ld %ld %ld\n", rawtime - atol(cacheList[i][2]), rawtime, atol(cacheList[i][2]));
            return cacheList[i][1];
        }
    }
    return "";
}

char* str2md5(const char *str, int length) {
    int n;
    MD5_CTX c;
    unsigned char digest[16];
    char *out = (char*) malloc (33);

    MD5_Init(&c);

    while (length > 0) {
        if (length > 512) {
            MD5_Update(&c, str, 512);
        } else {
            MD5_Update(&c, str, length);
        }
        length -= 512;
        str += 512;
    }

    MD5_Final(digest, &c);

    for (n = 0; n < 16; n++) {
        snprintf(&(out[n*2]), 16*2, "%02x", (unsigned int)digest[n]);
    }

    return out;
}

void * thread(void * vargp) {
    int connfd = *((int *)vargp);
    int sendfd;
    pthread_detach(pthread_self());
    free(vargp);

    size_t n, m;
    int i, j, f, found;
    int keepalive = 0;
    int first = 1;
    int msgsz;
    char buf[MAXREAD];
    char buf1[MAXREAD];
    unsigned char *resp = (char*) malloc (MAXREAD*sizeof(char));
    unsigned char *msg = (char*) malloc (MAXREAD*sizeof(char));
    char *context = NULL;
    char *comd;
    char *host;
    char *temp = NULL;
    char *tgtpath;
    char *fname1 = (char*) malloc (100*sizeof(char));
    char *httpver;
    char *contType;
    char *postdata;
    char *fpath;
    char *fname = (char*) malloc (100*sizeof(char));
    char *myfname = (char*) malloc (100*sizeof(char));
    char *contexthost = NULL;
    char c;
    FILE *fp;

    int lpf = 0;

    while (keepalive || first) {
        if (!first)
            printf("Waiting for incoming request\n");
        else
            first = 0;
        n = read(connfd, buf, MAXREAD);
        if ((int)n >= 0 && buf != NULL && strcmp(buf, "") != 0 && strcmp(buf, "GET") != 0 && strcmp(buf, "CONNECT") != 0) {
            printf("Request received\n");
            memcpy(buf1, buf, MAXREAD);
            comd = strtok_r(buf, " \t\r\n\v\f", &context);
            tgtpath = strtok_r(NULL, " \t\r\n\v\f", &context);
            httpver = strtok_r(NULL, " \t\r\n\v\f", &context);
            host = strtok_r(NULL, " \t\r\n\v\f", &context);
            host = strtok_r(NULL, " \t\r\n\v\f", &context);
            if (host == NULL) {
                strcpy(myfname, tgtpath);
                host = strtok_r(myfname, "/", &contexthost);
                host = strtok_r(NULL, "/", &contexthost);
            }
            //printf("comd=%s tgtpath=%s httpver=%s host=%s keepalive=%d \n", comd, tgtpath, httpver, host, keepalive);
            if (strcmp(comd, "GET") == 0) {
                sendfd = open_sendfd(80, host);
                if (sendfd < 0) {
                    //printf("sendfd < 0 sendfd = %d\n", sendfd);
                    if (sendfd == -2) {
                        sprintf(msg, "<html><head><title>404 Not Found</title></head><body><h2>404 Not Found</h2></body></html>");
                        sprintf(resp, "%s 404 File Not Found\r\nContent-Type:text/html\r\nContent-Length:%d\r\n\r\n%s", httpver, (int)strlen(msg), msg);
                    } else if (sendfd == -3) {
                        sprintf(msg, "<html><head><title>ERROR 403 Forbidden</title></head><body><h2>ERROR 403 Forbidden</h2></body></html>");
                        sprintf(resp, "%s 403 Forbidden\r\nContent-Type:text/html\r\nContent-Length:%d\r\n\r\n%s", httpver, (int)strlen(msg), msg);
                    } else {
                        sprintf(msg, "<html><head><title>400 Bad Request</title></head><body><h2>400 Bad Request</h2></body></html>");
                        sprintf(resp, "%s 400 Bad Request\r\nContent-Type:text/html\r\nContent-Length:%d\r\n\r\n%s", httpver, (int)strlen(msg), msg);
                    }
                    write(connfd, resp, strlen(resp));
                    close(sendfd);
                } else {
                    pthread_mutex_lock(&mtx);
                    fpath = checkCache(tgtpath);
                    lpf = 0;
                    if (strcmp(fpath, "") == 0) {
                        write(sendfd, buf1, n);
                        bzero(resp, MAXREAD);
                        m = 1;
                        f = 1;
                        while ((int)m > 0) {
                            m = read(sendfd, resp, MAXREAD);
                            if ((int)m < 0) {
                                printf("No response from server\n");
                                sprintf(msg, "<html><head><title>400 Bad Request</title></head><body><h2>400 Bad Request</h2></body></html>");
                                sprintf(resp, "%s 400 Bad Request\r\nContent-Type:text/html\r\nContent-Length:%d\r\n\r\n%s", httpver, (int)strlen(msg), msg);
                                m = strlen(resp);
                                write(connfd, resp, m);
                                m = -1;
                            } else if ((int)m == 0) {
                                printf("EOF\n");
                                write(connfd, resp, m);
                            } else {
                                write(connfd, resp, m);
                                printf("Store to cache ");
                                found = 0;
                                for (i = 0; i < cacheLen && found == 0; i++) {
                                    if(strcmp(tgtpath, cacheList[i][0]) == 0) {
                                        printf("Update Existing %s\n", tgtpath);
                                        time_t rawtime;
                                        time(&rawtime);
                                        sprintf(cacheList[i][2], "%ld", rawtime);
                                        sprintf(fname1, "./cached/%s", cacheList[i][1]);
                                        if (f) {
                                            fp = fopen(fname1, "wb+");
                                            f = 0;
                                        } else {
                                            fp = fopen(fname1, "ab+");
                                        }
                                        fwrite(resp, 1, m, fp);
                                        fclose(fp);
                                        found = 1;
                                    }
                                }
                                if (!found) {
                                    printf("New %s\n", tgtpath);
                                    strcpy(cacheList[cacheLen][0], tgtpath);
                                    sprintf(fname, "%s.%s", str2md5(tgtpath, strlen(tgtpath)), getFType(tgtpath));
                                    strcpy(cacheList[cacheLen][1], fname);
                                    sprintf(fname1, "./cached/%s", fname);
                                    time_t rawtime;
                                    time(&rawtime);
                                    sprintf(cacheList[i][2], "%ld", rawtime);
                                    if (f) {
                                        fp = fopen(fname1, "wb+");
                                        f = 0;
                                    } else {
                                        fp = fopen(fname1, "ab+");
                                    }
                                    fwrite(resp, 1, m, fp);
                                    fclose(fp);
                                    cacheLen++;
                                }
                                //Link Prefetching
                                if (strcmp("html", getFType(tgtpath)) == 0) {
                                    lpf = 1;
                                }
                            }
                        }
                    } else {
                        printf("Read from cached file %s\n", tgtpath);
                        sprintf(fname1, "./cached/%s", fpath);
                        fp = fopen(fname1, "rb+");
                        fseek(fp, 0, SEEK_SET);
                        m = fread(resp, 1, MAXREAD, fp);
                        fclose(fp);
                        write(connfd, resp, m);
                    }
                    close(sendfd);
                    pthread_mutex_unlock(&mtx);

                    //Link Prefetching
                    char *lpftok;
                    char *contextlpf = NULL;
                    char *hreflpf = (char*) malloc (100*sizeof(char));
                    char *linklpf;
                    char *contextlinklpf = NULL;
                    if (lpf) {
                        lpftok = strtok_r(resp, "<", &contextlpf);
                        while(lpftok != NULL) {
                            if (lpftok[0] == 'a'){
                                j = 0;
                                while (j < strlen(lpftok) - 8) {
                                    if (lpftok[j] == 'h' && lpftok[j + 1] == 'r' && lpftok[j + 2] == 'e' && lpftok[j + 3] == 'f' && lpftok[j + 4] == '=' && lpftok[j + 5] == '\"') {
                                        sprintf(hreflpf, "%s", lpftok + j);
                                        if (strlen(hreflpf) > 6) {
                                            linklpf = strtok_r(hreflpf, "\"", &contextlinklpf);
                                            linklpf = strtok_r(NULL, "\"", &contextlinklpf);
                                            if (linklpf != NULL) {
                                                pthread_t tidlpf;
                                                struct lpfwrapper *mywrapper = (struct lpfwrapper*) malloc (sizeof(struct lpfwrapper));
                                                mywrapper->fname = (char*) malloc (100*sizeof(char));
                                                mywrapper->hostname = (char*) malloc (100*sizeof(char));
                                                strcpy(mywrapper->fname, linklpf);
                                                strcpy(mywrapper->hostname, host);
                                                pthread_create(&tidlpf, NULL, threadlpf, mywrapper);
                                            }
                                        }
                                    }
                                    j++;
                                }
                            }
                            lpftok = strtok_r(NULL, "<", &contextlpf);
                        }
                    }
                }
            } else {
                sprintf(msg, "<html><head><title>400 Bad Request</title></head><body><h2>400 Bad Request</h2></body></html>");
                sprintf(resp, "%s 400 Bad Request\r\nContent-Type:text/html\r\nContent-Length:%d\r\n\r\n%s", httpver, (int)strlen(msg), msg);
                write(connfd, resp, strlen(resp));
            }
        } else {
            printf("No data received\n");
            keepalive = 0;
        }
    }
    printf("Closing thread\n");
    close(connfd);
    return NULL;
}

void * threadlpf(void * vargp) {
    struct lpfwrapper *mywrapper = ((struct lpfwrapper *)vargp);
    char *myfname = (char*) malloc (100*sizeof(char));
    char *host = (char*) malloc (100*sizeof(char));
    strcpy(myfname, mywrapper->fname);
    strcpy(host, mywrapper->hostname);
    pthread_detach(pthread_self());
    free(vargp);

    size_t n, m;
    int i, j, f, found, sendfd;
    int keepalive = 0;
    int first = 1;
    int msgsz;
    char buf[MAXREAD];
    char *tgtpath = (char*) malloc (100*sizeof(char));
    char *hosttok;
    char *contexthost = NULL;
    unsigned char *resp = (char*) malloc (MAXREAD*sizeof(char));
    char *fpath;
    char *fname = (char*) malloc (100*sizeof(char));
    char *fname1 = (char*) malloc (100*sizeof(char));
    char *myfname1 = (char*) malloc (100*sizeof(char));
    char c;
    FILE *fp;


    if (strlen(myfname) < 4) {
        //DO NOTHING
        return NULL;
    } else if (myfname[0] == 'h' && myfname[1] == 't' && myfname[2] == 't' && myfname[3] == 'p' && myfname[3] == 's') {
        printf("Cannot get https link\n");
        return NULL;
    } else if (myfname[0] == 'h' && myfname[1] == 't' && myfname[2] == 't' && myfname[3] == 'p') {
        strcpy(tgtpath, myfname);
        free(host);
        host = strtok_r(myfname, "/", &contexthost);
        host = strtok_r(NULL, "/", &contexthost);
    } else {
        sprintf(tgtpath, "http://%s/%s", host, myfname);
        sprintf(myfname1, "/%s", myfname);
    }

    sprintf(buf, "GET %s HTTP/1.1\r\nHost: %s\r\n\r\n", myfname1, host);
    sendfd = open_sendfd(80, host);
    if (sendfd >= 0) {
        write(sendfd, buf, strlen(buf));
        bzero(resp, MAXREAD);
        m = 1;
        f = 1;
        while((int)m > 0) {
            m = read(sendfd, resp, MAXREAD);
            if ((int)m > 0) {
                printf("LPF Store to cache %s\n", tgtpath);
                pthread_mutex_lock(&mtx);
                found = 0;
                for (i = 0; i < cacheLen && found == 0; i++) {
                    if(strcmp(tgtpath, cacheList[i][0]) == 0) {
                        time_t rawtime;
                        time(&rawtime);
                        sprintf(cacheList[i][2], "%ld", rawtime);
                        sprintf(fname1, "./cached/%s", cacheList[i][1]);
                        if (f) {
                            fp = fopen(fname1, "wb+");
                            f = 0;
                        } else {
                            fp = fopen(fname1, "ab+");
                        }
                        fseek(fp, 0, SEEK_SET);
                        fwrite(resp, 1, m, fp);
                        fclose(fp);
                        found = 1;
                    }
                }
                if (!found) {
                    strcpy(cacheList[cacheLen][0], tgtpath);
                    sprintf(fname, "%s.%s", str2md5(tgtpath, strlen(tgtpath)), getFType(tgtpath));
                    strcpy(cacheList[cacheLen][1], fname);
                    sprintf(fname1, "./cached/%s", fname);
                    time_t rawtime;
                    time(&rawtime);
                    sprintf(cacheList[i][2], "%ld", rawtime);
                    if (f) {
                        fp = fopen(fname1, "wb+");
                        f = 0;
                    } else {
                        fp = fopen(fname1, "ab+");
                    }
                    fseek(fp, 0, SEEK_SET);
                    fwrite(resp, 1, m, fp);
                    fclose(fp);
                    cacheLen++;
                }
                pthread_mutex_unlock(&mtx);
            } else if ((int)m == 0) {
                printf("LPF EOF\n");
            } else {
                printf("LPF m < 0 %s\n", tgtpath);
            }
        }
    }
    close(sendfd);
    return NULL;
}

/* 
 * open_listenfd - open and return a listening socket on port
 * Returns -1 in case of failure 
 */
int open_listenfd(int port) {
    int listenfd, optval=1;
    struct sockaddr_in serveraddr;
  
    /* Create a socket descriptor */
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;

    /* Eliminates "Address already in use" error from bind. */
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, 
                    (const void *)&optval , sizeof(int)) < 0)
        return -1;

    /* Sets a timeout of 10 secs. */
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    if (setsockopt(listenfd, SOL_SOCKET, SO_RCVTIMEO,
                    (struct timeval *)&tv,sizeof(struct timeval)) < 0)
        return -1;

    /* listenfd will be an endpoint for all requests to port
       on any IP address for this host */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET; 
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY); 
    serveraddr.sin_port = htons((unsigned short)port); 
    if (bind(listenfd, (struct sockaddr*)&serveraddr, sizeof(serveraddr)) < 0)
        return -1;

    /* Make it a listening socket ready to accept connection requests */
    if (listen(listenfd, LISTENQ) < 0)
        return -1;
    return listenfd;
} /* end open_listenfd */

/* 
 * open_sendfd - open and return a sending socket on port
 * Returns -1 in case of failure 
 */
int open_sendfd(int port, char *host) {
    int sendfd;
    struct sockaddr_in serveraddr;
    char *hostip;
    FILE *fp;
    char * line = NULL;
    size_t len = 0;
    ssize_t read;

    fp = fopen("blacklist.txt", "rb+");
    while ((read = getline(&line, &len, fp)) != -1) {
        if (strcmp(host, line) == 0)
            return -3;
    }
    fclose(fp);

    /* Create a socket descriptor */
    if ((sendfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;
    
    /* Sets a timeout of 10 secs. */
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    if (setsockopt(sendfd, SOL_SOCKET, SO_RCVTIMEO,
                    (struct timeval *)&tv,sizeof(struct timeval)) < 0)
        return -1;

    hostip = hostname_to_ip(host);
    if (strcmp(hostip, "error") == 0)
        return -2;
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET; 
    serveraddr.sin_addr.s_addr = inet_addr(hostip);
    serveraddr.sin_port = htons((unsigned short)port);
    if (connect(sendfd, (struct sockaddr*)&serveraddr, sizeof(serveraddr)) < 0)
        return -1;
    return sendfd;
} /* end open_sendfd */

char* hostname_to_ip(char *hostname) {
    struct hostent *he;
    struct in_addr **addr_list;
    char *ip = (char*) malloc (20 * sizeof(char));
    int i;

    for (i = 0; i < ipcacheLen; i++) {
        if (strcmp(ipcache[i][0], hostname) == 0) {
            return ipcache[i][1];
        }
    }

    if ((he = gethostbyname(hostname)) == NULL) {
        // get the host info
        herror("gethostbyname");
        return "error";
    }

    addr_list = (struct in_addr **) he->h_addr_list;

    for (i = 0; addr_list[i] != NULL; i++) {
        //Return the first one;
        strcpy(ip , inet_ntoa(*addr_list[i]));
        strcpy(ipcache[ipcacheLen][0], hostname);
        strcpy(ipcache[ipcacheLen][1], ip);
        ipcacheLen++;
        return ip;
    }

    strcpy(ipcache[ipcacheLen][0], hostname);
    strcpy(ipcache[ipcacheLen][1], ip);
    ipcacheLen++;
    return ip;
}