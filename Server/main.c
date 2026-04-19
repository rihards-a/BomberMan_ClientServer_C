#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
// #include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#include "../config.h"
#include "../msg_protocol.h"


int main() {  
    int server_fd, client_fd;  
    struct sockaddr_in server_addr, client_addr;  
    char buffer[1024] = {0};  

    server_fd = socket(AF_INET, SOCK_STREAM, 0);  
    if (server_fd == -1) {  
        perror("Socket creation failed");  
        return EXIT_FAILURE;  
    }  

    server_addr.sin_family = AF_INET;  
    server_addr.sin_addr.s_addr = INADDR_ANY;  
    server_addr.sin_port = htons(SERVER_PORT);  

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {  
        perror("Bind failed");  
        return EXIT_FAILURE;  
    }  

    listen(server_fd, 5);  
    printf("Server listening on port 6969\n");  

    socklen_t addr_len = sizeof(client_addr);  
    client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);  
    if (client_fd < 0) {  
        perror("Accept failed");  
        return EXIT_FAILURE;  
    }  

    printf("Client connected\n");  

    /* ------------------ main loop ------------------ */
    while (1) {  
        msg_move_attempt_t msg;

        ssize_t bytes_read = recv(client_fd, &msg, sizeof(msg), MSG_WAITALL);
        if (bytes_read <= 0) {
            printf("Client disconnected\n");
            break;
        }
        
        if (bytes_read != sizeof(msg)) continue;
        
        printf("Direction: %c\n", msg.direction);
    }

    return 0;  
}
