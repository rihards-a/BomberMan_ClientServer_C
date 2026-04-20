#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#include "../net.h"
#include "../config.h"
#include "../msg_protocol.h"

msg_map_t *GAME_MAP;
int CLIENT_FD;

ssize_t send_all(int fd, const void *buf, size_t len);

int main() {  
    int server_fd;  
    struct sockaddr_in server_addr, client_addr;  

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
    CLIENT_FD = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);  
    if (CLIENT_FD < 0) {  
        perror("Accept failed");  
        return EXIT_FAILURE;  
    }  

    printf("Client connected\n");  

    /* -----------------test-input-map-------------------- */
    uint8_t height, width, c;
    /* make sure you're running it from the right directory (as ./server)*/
    FILE *fp = fopen("maps/test_map_1.txt", "r");
    if (!fp) return 1;

    fscanf(fp, "%hhd %hhd", &height, &width);
    GAME_MAP = malloc(sizeof(GAME_MAP) + height * width);
    GAME_MAP->width = width;
    GAME_MAP->height = height;

    /* skip 4 characters while testing */
    for (int i = 0; i < 4; i++) {
        fscanf(fp, " %c", &c);
    }

    for (int i = 0; i < height * width; i++)
        fscanf(fp, " %c", &GAME_MAP->cells[i]);
    
    fclose(fp);

    usleep(1000000); /* wait ; check if the map will change from client loop */

    /* send MSG_SYNC_BOARD from TARGET_SERVER (255) to everyone (254) - but actually just the CLIENT_FD */
    printf("Sending map message to client...\n");
    if (send_map_message(CLIENT_FD, MSG_SYNC_BOARD, TARGET_SERVER, TARGET_BROADCAST, GAME_MAP) < 0) {
        perror("Failed to send map message");
        return EXIT_FAILURE;
    }

    /* ------------------ main loop ------------------ */
    while (1) {  
        msg_move_attempt_t msg;

        ssize_t bytes_read = recv(CLIENT_FD, &msg, sizeof(msg), MSG_WAITALL);
        if (bytes_read <= 0) {
            printf("Client disconnected\n");
            break;
        }
        
        if (bytes_read != sizeof(msg)) continue;
        
        printf("Direction: %c\n", msg.direction);
        usleep(1000000 / TICKS_PER_SECOND); /* 1e6 for microseconds */
    }

    return 0;  
}


/* -------------------------- function declarations --------------------------- */

