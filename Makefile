# Makefile syntax info:
# $@ - target output name
# $^ - all dependencies
# $< - first dependency
# for previously declared values

# how it works:
# make server -> compiles server only
# make -> compiles both

# explanatory pattern of the Makefile:
#	all: server client
# -> server
#	server: $(COMMON_OBJ)
# -> COMMON_OBJ = $(COMMON_SRC:.c:.o)
# -> COMMON_SRC = net.c
# -> $(net.c:.c:.o)
# -> net.o
# 	net.o : net.c
#	$(CC) $(CFLAGS) -c $< -o $@
#   gcc -f ... -c net.c -o net.o
# -> server : net.o $(SERVER_OBJ)
# ... the same steps to get server.o
# -> server : net.o server.o 
# 	$(CC) $(CFLAGS) -o $@ $^
#   gcc -Wall -Wextra -Werror -o server net.o server.o
# the same steps for client

CC = gcc
CFLAGS = -Wall -Wextra -Werror

# resulting executables
SERVER = Server/server
CLIENT = Client/client

# sources
COMMON_SRC = net.c
SERVER_SRC = Server/main.c
CLIENT_SRC = Client/main.c

# objects
COMMON_OBJ = $(COMMON_SRC:.c=.o)
SERVER_OBJ = $(SERVER_SRC:.c=.o)
CLIENT_OBJ = $(CLIENT_SRC:.c=.o)

# default builds both
all: $(SERVER) $(CLIENT)

# gcc -flags... -o $(SERVER) $(COMMONG_OBJ) $(SERVER_OBJ)
$(SERVER): $(COMMON_OBJ) $(SERVER_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(CLIENT): $(COMMON_OBJ) $(CLIENT_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lncurses

# -c flags means compile only, no linking
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(COMMON_OBJ) $(SERVER_OBJ) $(CLIENT_OBJ)

fclean: clean
	rm -f $(SERVER) $(CLIENT)

# runs the specified commands in order
re: fclean all

# prevents conflict with equal file names
.PHONY: all clean fclean re
