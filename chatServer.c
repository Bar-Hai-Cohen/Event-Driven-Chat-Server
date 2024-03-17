#include "chatServer.h"

//This variable is used as a flag to control the server loop. When it's set to 1, the server loop will end.
static int end_server = 0;

void intHandler(int SIG_INT) {
    end_server = 1; // Set flag to end the server loop
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: Server <port>\n");
        exit(EXIT_FAILURE);
    }

    int port = atoi(argv[1]);
    if (port < 1 || port > 65535) {
        printf("Usage: Server <port>\n");
        exit(EXIT_FAILURE);
    }

    signal(SIGINT, intHandler);

    // Initialize connection pool
    conn_pool_t* pool = malloc(sizeof(conn_pool_t));
    if (initPool(pool) == -1) {
        perror("Error initializing connection pool");
        exit(EXIT_FAILURE);
    }

    // Create socket
    int listen_sd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sd < 0) {
        perror("Error creating socket");
        free(pool);
        exit(EXIT_FAILURE);
    }

    // Set socket to non-blocking
    int on = 1;
    if (ioctl(listen_sd, FIONBIO, (char *)&on) < 0) {
        perror("Error setting socket to non-blocking");
        close(listen_sd);
        free(pool);
        exit(EXIT_FAILURE);
    }
    // Bind socket
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(listen_sd));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_sd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(listen_sd);
        free(pool);
        perror("Error binding socket");
        exit(EXIT_FAILURE);
    }

    // Listen
    //TODO:check how many in the listen ? in the last work was 5
    if (listen(listen_sd, 5) < 0) {
        perror("Error listening on socket");
        free(pool);
        exit(EXIT_FAILURE);
    }
    // Clear sets
    FD_ZERO(&pool->read_set);
    FD_ZERO(&pool->write_set);
    FD_ZERO(&pool->ready_read_set);
    FD_ZERO(&pool->ready_write_set);

    if (addConn(listen_sd, pool) == -1) {
        perror("Failed to add listen_sd\n");
        close(listen_sd);
        free(pool);
        exit(EXIT_FAILURE);
    }
    pool->maxfd = listen_sd;
    // Add active connections to sets
    conn_t *curr_conn = pool->conn_head;
    while (curr_conn != NULL) {
        FD_SET(curr_conn->fd, &pool->read_set);
        if (curr_conn->write_msg_head != NULL) {
            FD_SET(curr_conn->fd, &pool->write_set);
        }
        if (curr_conn->fd > pool->maxfd) {
            pool->maxfd = curr_conn->fd;
        }
        curr_conn = curr_conn->next;
    }
    // Main server loop
    do {
        // Make a copy of the sets for select
        memcpy(&pool->ready_read_set, &pool->read_set, sizeof(fd_set));
        memcpy(&pool->ready_write_set, &pool->write_set, sizeof(fd_set));

        // Print before calling select
        printf("waiting on select()...\nMaxFd %d\n", pool->maxfd);

        // Call select
        pool->nready = select(pool->maxfd + 1, &pool->ready_read_set, &pool->ready_write_set, NULL, NULL);
        if (pool->nready < 0) {
            perror("Error in select");
            continue;
        }

        // Handle listening socket
        if (FD_ISSET(listen_sd, &pool->ready_read_set)) {
            int new_sd = accept(listen_sd, NULL, NULL);
            if (new_sd < 0) {
                perror("Error accepting new connection");
            } else {
                printf("New incoming connection on sd %d\n", new_sd);
                if (addConn(new_sd, pool) == -1) {
                    perror("Error adding new connection");
                }
            }
        }

        // Handle active connections
        curr_conn = pool->conn_head;
        while (curr_conn != NULL) {
            printf("inside while loop %d\n",curr_conn->fd);
            int sd = curr_conn->fd;
            if(sd==listen_sd){
                curr_conn = curr_conn->next;
                continue;
            }
            if (FD_ISSET(sd, &pool->ready_read_set)) {
                char buffer[BUFFER_SIZE];
                //TODO:check if recv of read
                int len = read(sd, buffer, BUFFER_SIZE);
                if (len < 0) {
                    perror("Error reading from client");
                } else if (len == 0) {
                    printf("Connection closed for sd %d\n", sd);
                    removeConn(sd, pool);
                } else {
                    printf("%d bytes received from sd %d\n", len, sd);
                    if(addMsg(sd, buffer, len, pool)==-1){
                        perror("Failed to add mag");
                    }
                }
            }
            if (FD_ISSET(sd, &pool->ready_write_set)) {
                if (writeToClient(sd, pool) == -1) {
                    perror("Error writing to client");
                }
            }
            curr_conn = curr_conn->next;
        }
    } while (end_server == 0);

    // Cleanup connections
    conn_t *curr_conn_cleanup = pool->conn_head;
    while (curr_conn_cleanup != NULL) {
        printf("removing connection with sd %d \n", curr_conn_cleanup->fd);
        conn_t* next_conn = curr_conn_cleanup->next; // Store the next pointer before removing the current connection
        removeConn(curr_conn_cleanup->fd, pool);
        curr_conn_cleanup = next_conn; // Move to the next connection
    }
    free(pool);
    //close(listen_sd);

    return 0;
}

int initPool(conn_pool_t* pool) {
    if (pool == NULL) {
        return -1;
    }
    pool->maxfd = -1;
    pool->nready = 0;
    FD_ZERO(&pool->read_set);
    FD_ZERO(&pool->ready_read_set);
    FD_ZERO(&pool->write_set);
    FD_ZERO(&pool->ready_write_set);
    pool->conn_head=NULL;
    pool->nr_conns = 0;
    return 0;
}

int addConn(int sd, conn_pool_t* pool) {
    if (pool == NULL) {
        return -1;
    }
    conn_t *new_conn = (conn_t *)malloc(sizeof(conn_t));
    if (new_conn == NULL) {
        return -1;
    }
    new_conn->fd = sd;
    new_conn->write_msg_head = NULL;
    new_conn->write_msg_tail = NULL;

    if (pool->conn_head == NULL) {
        new_conn->prev = new_conn->next = NULL;
        pool->conn_head = new_conn;
    } else {
        new_conn->prev = NULL;
        new_conn->next = pool->conn_head;
        pool->conn_head->prev = new_conn;
        pool->conn_head = new_conn;
    }
    pool->nr_conns++;
    if (sd > pool->maxfd) pool->maxfd = sd;
    FD_SET(sd, &(pool->read_set));
    return 0;
}

int removeConn(int sd, conn_pool_t* pool) {
    if (pool == NULL) {
        return -1;
    }
    conn_t *curr_conn = pool->conn_head;
    while (curr_conn != NULL) {
        if (curr_conn->fd == sd) {
            // Remove from connection pool
            if (curr_conn->prev != NULL) {
                curr_conn->prev->next = curr_conn->next;
            }else {
                pool->conn_head = curr_conn->next;
            }
            if (curr_conn->next != NULL) {
                curr_conn->next->prev = curr_conn->prev;
            }
            if (curr_conn->write_msg_head) {
                // Free messages in the queue if any
                msg_t* msg = curr_conn->write_msg_head;
                while (msg) {
                    msg_t* temp = msg;
                    msg = msg->next;
                    free(temp->message);
                    free(temp);
                }
            }
            close(sd);
            FD_CLR(sd, &(pool->read_set));
            FD_CLR(sd, &(pool->write_set));
            free(curr_conn);
            pool->nr_conns--;
            return 0;
        }
        curr_conn = curr_conn->next;
    }
    return -1; // Connection not found
}

int addMsg(int sd, char* buffer, int len, conn_pool_t* pool) {
    if (pool == NULL || buffer==NULL || len<=0) {
        return -1;
    }
    conn_t *curr_conn = pool->conn_head;
    while (curr_conn != NULL && curr_conn->next != NULL) {
        if (curr_conn->fd != sd) {
            // Allocate and populate new message
            msg_t *new_msg = (msg_t*)malloc(sizeof(msg_t));
            if (new_msg == NULL) {
                return -1;
            }
            new_msg->message = (char *)malloc(len + 1);
            if (new_msg->message == NULL) {
                free(new_msg);
                return -1;
            }
            strncpy(new_msg->message, buffer, len);
            new_msg->message[len] = '\0';
            new_msg->size = len;
            new_msg->prev = new_msg->next = NULL;
            // Add message to connection's write queue
            if (curr_conn->write_msg_head == NULL) {
                curr_conn->write_msg_head = new_msg;
                curr_conn->write_msg_tail = new_msg;
            } else {
                curr_conn->write_msg_tail->next = new_msg;
                new_msg->prev = curr_conn->write_msg_tail;
                curr_conn->write_msg_tail = new_msg;
            }
            // Update file descriptor set
            FD_SET(curr_conn->fd, &pool->write_set);
        }
        curr_conn = curr_conn->next;
    }
    return 0;
}

int writeToClient(int sender_sd, conn_pool_t* pool) {
    if (pool == NULL) {
        return -1;
    }
    conn_t *curr_conn = pool->conn_head;
    while (curr_conn != NULL) {
        if (FD_ISSET(curr_conn->fd, &pool->write_set) && curr_conn->write_msg_head != NULL) {
            msg_t *msg = curr_conn->write_msg_head;
            // Convert message to uppercase
            for (int i = 0; i < msg->size; ++i) {
                msg->message[i] = toupper((unsigned char)msg->message[i]);
            }
            int bytes_written = 0;
            while (bytes_written < msg->size) {
                int ret = write(curr_conn->fd, msg->message + bytes_written, msg->size - bytes_written);
                if (ret < 0) {
                    perror("send failed\n");
                    return -1;
                } else if (ret == 0) {

                    return -1;
                } else {
                    bytes_written += ret;
                }
            }
            curr_conn->write_msg_head = msg->next;
            if (curr_conn->write_msg_head == NULL) {
                curr_conn->write_msg_tail = NULL;
            }
            free(msg->message);
            free(msg);
        }
        FD_CLR(curr_conn->fd, &pool->write_set);
        curr_conn = curr_conn->next;
    }
    return 0; // No message sent
}