# Network Systems Programming Assignment 3

Student Details:

Hemanth Chenna - hech9374@colorado.edu

This is a program to run a forward proxy that caches the files being read and responds with those files when requested again. The program also prefetches links in html files and caches them as well.

Use the following commands to compile and run the file (Replace portno with the port where the socket must be opened and timeout with caching timeout in seconds)

```
gcc -pthread proxyserver.c -o server -lcrypto -lssl
./server portno timeout
```

The program has the following features implemented:
1) Multi-threaded proxy - The proxy can respond to multiple requests at the same time as each request is run on a different pthread
2) Caching - The proxy caches web pages and resources and returns them to the client if requested again before the caching timeout expires for that resource, without querying the host again.
3) Hostname IP Address cache - The proxy caches the DNS query result and uses it instead of another DNS query next time the same webpage is accessed.
4) Blacklist - The proxy maintains a list of websites that are blacklisted and does not allow queries to those websites.
5) Link prefetching - The proxy checks html files for additional links and prefetches them to be cached before the user makes subsequent requests allowing for the pages to be loaded quickly from the cache.
