#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>

#define BUFFER_SIZE 4096
#define CACHE_DIR "cache"
#define BLOCKLIST_FILE "blocked.txt"
#define MAX_THREADS 100

int CACHE_TIMEOUT;
pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

int is_blocked(const char *hostname);
void *handle_client(void *client_socket_ptr);
// void cache_response(const char *url, const char *response);
int serve_from_cache(int client_socket, const char *url);
int encode_url_to_filename(const char *url, char *buffer, size_t buffer_size);


int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <port> <cache_timeout>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int port = atoi(argv[1]);
    CACHE_TIMEOUT = atoi(argv[2]);
    printf("Cache timeout set to %d seconds\n", CACHE_TIMEOUT);

    int server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
 

    // Create server socket
    if ((server_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int optval = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR,
               (const void *)&optval, sizeof(int));

    // Setup server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // Bind socket to the port
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        perror("bind");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    // Listen for connections
    if (listen(server_socket, 10) == -1)
    {
        perror("listen");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d...\n", port);

    pthread_t threads[MAX_THREADS];
    int thread_count = 0;

    while (1)
    {
        // Accept new client connection
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket == -1)
        {
            perror("accept");
            continue;
        }

        int *new_sock = malloc(sizeof(int));
        if (new_sock == NULL)
        {
            perror("malloc");
            close(client_socket);
            continue;
        }
        *new_sock = client_socket;

        // Create a new thread to handle the client
        if (pthread_create(&threads[thread_count++], NULL, handle_client, (void *)new_sock) != 0)
        {
            perror("pthread_create");
            close(client_socket);
            free(new_sock);
        }

        // Clean up threads if limit is reached
        if (thread_count >= MAX_THREADS)
        {
            for (int i = 0; i < MAX_THREADS; i++)
            {
                pthread_join(threads[i], NULL);
            }
            thread_count = 0;
        }
    }

    close(server_socket);
    return 0;
}

void *handle_client(void *arg)
{
    int client_socket = *(int *)arg;
    free(arg);
    char buffer[BUFFER_SIZE];
    char method[16], url[2048], version[16];
    char host[512], path[1024];
    int server_socket;
    struct sockaddr_in server_addr;

    int bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received <= 0)
    {
        close(client_socket);
        return NULL;
    }
    buffer[bytes_received] = '\0';
    printf("Received request: %s\n", buffer);

    sscanf(buffer, "%s %s %s", method, url, version);
    printf("Parsed method: %s, URL: %s, Version: %s\n", method, url, version);

    if (strcmp(method, "GET") != 0)
    {
        send(client_socket, "HTTP/1.1 405 Method Not Allowed\r\n\r\n", 35, 0);
        close(client_socket);
        return NULL;
    }

    sscanf(url, "http://%511[^/]/%1023[^\n]", host, path);
    if (!*path)
        strcpy(path, "/");

    char *port_position = strchr(host, ':');
    int port = 80;
    if (port_position)
    {
        *port_position = '\0';
        port = atoi(port_position + 1);
    }
    printf("Parsed host: %s, path: %s, port: %d\n", host, path, port);

    if (is_blocked(host))
    {
        printf("Blocked host: %s\n", host);
        const char *forbidden_response =
            "HTTP/1.1 403 Forbidden\r\n"
            "Content-Length: 48\r\n\r\n"
            "<html><body><h1>403 Forbidden</h1></body></html>";
        send(client_socket, forbidden_response, strlen(forbidden_response), 0);
        close(client_socket);
        return NULL;
    }

    pthread_mutex_lock(&cache_mutex);
    if (serve_from_cache(client_socket, url))
    {
        pthread_mutex_unlock(&cache_mutex);
        printf("Served from cache: %s\n", url);
        close(client_socket);
        return NULL;
    }
    pthread_mutex_unlock(&cache_mutex);

    printf("Cache miss: fetching content for URL: %s\n", url);

    if ((server_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        perror("socket");
        close(client_socket);
        return NULL;
    }
    printf("Connecting to server: %s\n", host);

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    struct hostent *server = gethostbyname(host);
    if (!server)
    {
        perror("gethostbyname");
        close(client_socket);
        close(server_socket);
        return NULL;
    }
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);

    if (connect(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("connect");
        close(client_socket);
        close(server_socket);
        return NULL;
    }

    snprintf(buffer, sizeof(buffer), "GET /%s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    send(server_socket, buffer, strlen(buffer), 0);
    printf("Sent request to server: %s\n", buffer);

    char cache_filename[1024];
    encode_url_to_filename(url, cache_filename, sizeof(cache_filename));
    char full_cache_path[1024];
    snprintf(full_cache_path, sizeof(full_cache_path), "%s/%s", CACHE_DIR, cache_filename);
    printf("Caching URL: %s as %s\n", url, full_cache_path);

    pthread_mutex_lock(&cache_mutex);
    FILE *cache_file = fopen(full_cache_path, "w");
    if (!cache_file)
    {
        perror("Failed to open cache file");
        pthread_mutex_unlock(&cache_mutex);
        close(client_socket);
        close(server_socket);
        return NULL;
    }
    pthread_mutex_unlock(&cache_mutex);

    while ((bytes_received = recv(server_socket, buffer, sizeof(buffer), 0)) > 0)
    {
        send(client_socket, buffer, bytes_received, 0);
        fwrite(buffer, 1, bytes_received, cache_file);
    }

    fclose(cache_file);
    printf("Caching complete for URL: %s\n", url);

    close(server_socket);
    close(client_socket);
    return NULL;
}

int is_blocked(const char *hostname)
{
    FILE *blocklist = fopen(BLOCKLIST_FILE, "r");
    if (!blocklist)
    {
        perror("Failed to open blocklist file");
        printf("Hostname %s is NOT blocked (blocklist file missing)\n", hostname);
        return 0; // Not blocked by default
    }

    char line[256];
    printf("Checking blocklist for hostname: %s\n", hostname);
    while (fgets(line, sizeof(line), blocklist))
    {
        // Trim newline characters
        line[strcspn(line, "\r\n")] = '\0';

        // Skip leading spaces
        char *trimmed_line = line;
        while (isspace(*trimmed_line))
        {
            trimmed_line++;
        }

        // Compare hostnames case-insensitively
        if (strcasecmp(trimmed_line, hostname) == 0)
        {
            fclose(blocklist);
            printf("Hostname %s is blocked (matched %s in blocklist)\n", hostname, trimmed_line);
            return 1; // Blocked
        }
    }

    fclose(blocklist);
    printf("Hostname %s is NOT blocked\n", hostname);
    return 0; // Not blocked
}

int encode_url_to_filename(const char *url, char *buffer, size_t buffer_size)
{
    if (url == NULL || buffer == NULL || buffer_size < 20)
        return -1;

    // Simple DJB2 hash function for generating a unique filename
    unsigned long hash = 5381;
    int c;
    while ((c = *url++))
    {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }

    // Convert the hash to a hex string and store it in the buffer
    snprintf(buffer, buffer_size, "%lx", hash);
    return 0;
}
int serve_from_cache(int client_socket, const char *url)
{
    char encoded_url[1024];
    if (encode_url_to_filename(url, encoded_url, sizeof(encoded_url)) != 0)
    {
        fprintf(stderr, "Failed to encode URL for cache lookup\n");
        return 0; // Cache miss
    }

    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/%s", CACHE_DIR, encoded_url);

    struct stat file_info;
    if (stat(filepath, &file_info) != 0)
    {
        printf("Cache miss: %s does not exist\n", filepath);
        return 0; // Cache miss
    }

    if (difftime(time(NULL), file_info.st_mtime) > CACHE_TIMEOUT)
    {
        printf("Cache expired for URL %s\n", url);
        return 0; // Cache expired
    }

    FILE *file = fopen(filepath, "r");
    if (!file)
    {
        printf("Cache file for URL %s found but could not be opened\n", url);
        return 0; // Cache miss due to file access error
    }

    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        send(client_socket, buffer, bytes_read, 0);
    }
    fclose(file);

    printf("Served cached content for URL: %s\n", url);
    return 1; // Cache hit
}

// void cache_response(const char *url, const char *response)
// {
//     // Encode the URL to generate a unique filename for caching
//     char encoded_url[1024];
//     if (encode_url_to_filename(url, encoded_url, sizeof(encoded_url)) != 0)
//     {
//         fprintf(stderr, "Failed to encode URL for caching\n");
//         return;
//     }

//     // Create the full path to the cache file
//     char filepath[1024];
//     snprintf(filepath, sizeof(filepath), "%s/%s", CACHE_DIR, encoded_url);
//     printf("Caching response for URL %s at %s\n", url, filepath);

//     // Lock the cache to avoid race conditions
//     pthread_mutex_lock(&cache_mutex);

//     // Open the file for writing
//     FILE *file = fopen(filepath, "w");
//     if (file)
//     {
//         // Write the response to the cache file
//         fputs(response, file);
//         fclose(file);
//         printf("Response cached: %s\n", filepath);
//     }
//     else
//     {
//         perror("Error caching response");
//     }

//     // Unlock the cache mutex
//     pthread_mutex_unlock(&cache_mutex);
// }