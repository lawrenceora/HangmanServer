#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>

#include "network.h"
#include "game.h"
#include <signal.h>

#ifndef PORT
    #define PORT 54261
#endif
#define MAX_QUEUE 5


/* The set of socket descriptors for select to monitor.
 * This is a global variable because we need to remove socket descriptors
 * from allset when a write to a socket fails.
 */
fd_set allset;


/* Fill buf with count null terminators */
void null_terminate_all(char *buf, int count){
    for (int i = 0; i < count; i++){
        buf[i] = '\0';
    }
}


/* Add a client to the head of the linked list */
void add_player(struct client **top, int fd, struct in_addr addr) {
    struct client *p = malloc(sizeof(struct client));

    if (!p) {
        perror("malloc");
            exit(1);
    }

    printf("Adding client %s\n", inet_ntoa(addr));

    p->fd = fd;
    p->ipaddr = addr;
    p->name[0] = '\0';
    p->in_ptr = p->inbuf;
    p->inbuf[0] = '\0';
    p->next = *top;
    *top = p;
}


/* Removes client from the linked list and closes its socket.
 * Also removes socket descriptor from allset 
 */
void remove_player(struct client **top, int fd) {
    struct client **p;

    for (p = top; *p && (*p)->fd != fd; p = &(*p)->next);
    // Now, p points to (1) top, or (2) a pointer to another client
    // This avoids a special case for removing the head of the list
    if (*p) {
        struct client *t = (*p)->next;
        printf("Removing client %d %s\n", fd, inet_ntoa((*p)->ipaddr));
        FD_CLR((*p)->fd, &allset);
        close((*p)->fd);
        free(*p);
        *p = t;
    } else {
        fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n", fd);
    }
}


/* Remove player from new_list and add to game->head
*/
void activate_player(struct client **new_list, struct game_state *game, struct client *new_p){
    struct client **p;
    //remove player from new_list
    for (p = new_list; *p && (*p)->fd != new_p->fd; p = &(*p)->next);
    if (*p){
        struct client *t = (*p)->next;
        *p = t;
    }

    //add player to head of game
    new_p->next = game->head;
    game->head = new_p;
}


/* Read in the name written by the client pointed to by new_p.
- If an error occurs, return -1.
- If name is already taken, return -2.
- If a newline has yet to be found, return -3
- Otherwise, return length of name inputted.
*/
int ask_for_name(struct game_state *game, struct client *new_p){
    int buf_offset = strlen(new_p->name) * sizeof(char);
    int num_read = read(new_p->fd, new_p->name + buf_offset, sizeof(char) * MAX_NAME);
    printf("[%d] Read %d bytes\n", new_p->fd, num_read);

    // If there are errors
    if (num_read <= 0){
        return -1;
    }
    // If network newline was not found
    int net_nl = find_network_newline(new_p->name, MAX_NAME);
    if (net_nl == -1){
        new_p->name[buf_offset + num_read] = '\0';
        return -3;	
    }

    //If no errors and reading has finished
    new_p->name[net_nl] = '\0';
    new_p->name[net_nl+1] = '\0';
    printf("[%d] Found newline %s\n", new_p->fd, new_p->in_ptr);
    
    // Check if name already exists among active players
    struct client *curr = game->head;
    while(curr != NULL){
        if (strcmp(curr->name, new_p->name) == 0){
            return -2;
        }
        curr = curr->next;
    }
    return strlen(new_p->name);
}


/* Removes an active player from game.
Prerequisites: player is a pointer to an active player in the game
*/
void leave_handler(struct game_state *game, struct client *player){  

    // Special case when the player is at the head of the list
    if (game->head == player){
        if (player->next == NULL){ // if player is the only person remaining.
            game->head = NULL;
            game->has_next_turn = NULL;
        } else { // otherwise
            game->head = player->next;
        }
    }


    // Reconnect the remaining players in the linked list and announce departure
    // broadcast() cannot be used in this function as it will cause an infinite loop.
    struct client *curr = game->head;
    char leave_msg[MAX_MSG];
    sprintf(leave_msg, "\r\n%s has left the game\r\n", player->name);
    while (curr != NULL){
        // send message that player has disconnected:
        if (curr != player){
            game_write(game, curr, leave_msg, strlen(leave_msg));    
        }
        // remove player from linked list
        if (curr->next == player){
            curr->next = player->next;
        } 
        curr = curr->next;
    }

    // If it was currently player's turn
    if (game->has_next_turn == player){
        advance_turn(game);
    }

    // Reannounce turn. Won't cause an infinite loop since player has already been removed.
    announce_turn(game);

    // Remove from allset, close and free
    printf("Removing client %d %s\n", player->fd, inet_ntoa(player->ipaddr));
    FD_CLR(player->fd, &allset);
    close(player->fd);
    free(player);
}



int main(int argc, char **argv) {
    int clientfd, maxfd, nready;
    struct client *p;
    struct sockaddr_in q;
    fd_set rset;
    
    if(argc != 2){
        fprintf(stderr,"Usage: %s <dictionary filename>\n", argv[0]);
        exit(1);
    }
    // Ignore SIGPIPE
    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if(sigaction(SIGPIPE, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }
    
    // Create and initialize the game state
    struct game_state game;

    srandom((unsigned int)time(NULL));
    // Set up the file pointer outside of init_game because we want to 
    // just rewind the file when we need to pick a new word
    game.dict.fp = NULL;
    game.dict.size = get_file_length(argv[1]);

    init_game(&game, argv[1]);
    
    // head and has_next_turn also don't change when a subsequent game is
    // started so we initialize them here.
    game.head = NULL;
    game.has_next_turn = NULL;
    
    /* A list of client who have not yet entered their name.  This list is
     * kept separate from the list of active players in the game, because
     * until the new playrs have entered a name, they should not have a turn
     * or receive broadcast messages.  In other words, they can't play until
     * they have a name.
     */
    struct client *new_players = NULL;
    
    struct sockaddr_in *server = init_server(PORT);
    int listenfd = set_up_socket(server, MAX_QUEUE);
    
    // initialize allset and add listenfd to the
    // set of file descriptors passed into select
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    // maxfd identifies how far into the set to search
    maxfd = listenfd;

    while (1) {
        // make a copy of the set before we pass it into select
        rset = allset;
        nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
        if (nready == -1) {
            perror("select");
            continue;
        }

        if (FD_ISSET(listenfd, &rset)){
            printf("A new client is connecting\n");
            clientfd = accept_connection(listenfd);

            FD_SET(clientfd, &allset);
            if (clientfd > maxfd) {
                maxfd = clientfd;
            }
            printf("Connection from %s\n", inet_ntoa(q.sin_addr));
            add_player(&new_players, clientfd, q.sin_addr);
            char *greeting = WELCOME_MSG;
            if(write(clientfd, greeting, strlen(greeting)) == -1) {
                fprintf(stderr, "Write to client %s failed\n", inet_ntoa(q.sin_addr));
                remove_player(&new_players, p->fd);
            };
        }
        
        /* Check which other socket descriptors have something ready to read.
         * The reason we iterate over the rset descriptors at the top level and
         * search through the two lists of clients each time is that it is
         * possible that a client will be removed in the middle of one of the
         * operations. This is also why we call break after handling the input.
         * If a client has been removed the loop variables may not longer be 
         * valid.
         */
        int cur_fd;
        for(cur_fd = 0; cur_fd <= maxfd; cur_fd++) {
            if(FD_ISSET(cur_fd, &rset)) {
                
                // Check if this socket descriptor is an active player 
                for(p = game.head; p != NULL; p = p->next) {
                    if(cur_fd == p->fd) {

                        // Handle input from client with current turn                  
                        if (p == game.has_next_turn){
                            int guess_status = process_turn_input(&game, p);
                            
                            // If guess is valid (single, lowercase, unguessed letter)
                            if (guess_status == 0){
                                if (check_game_over(&game, p) == 0){
                                    start_new_game(&game, argv[1]);
                                }
                                announce_status(&game, NULL);
                                announce_turn(&game);

                            // If reading has found a network newline
                            } else if (guess_status > -1){ 
                                null_terminate_all(p->in_ptr, MAX_BUF);
                            }
                         // Handle input from client who doesnt have their turn
                         } else {
                            if (game_read(&game, p, p->in_ptr, MAX_BUF) == 0){
                                char *ignore_msg = "It's not yet your turn!\r\n";
                                printf("Player %s tried to guess out of turn\n", p->name);
                                null_terminate_all(p->in_ptr, MAX_BUF);
                                game_write(&game, p, ignore_msg, strlen(ignore_msg));
                            }
                        }
                        FD_CLR(cur_fd, &rset);
                        break;
                    }
                }
        
                // Check if this socket descriptor is adding their name
                for(p = new_players; p != NULL; p = p->next) {
                    if(cur_fd == p->fd) {
                        int name_len = ask_for_name(&game, p);
                        char name_msg[MAX_MSG]; 
                        // if name is valid
                        if (name_len > 0){
                            activate_player(&new_players, &game, p);

                            
                            // if this is the first person to be added
                            if (game.has_next_turn == NULL){
                                advance_turn(&game);
                            }

                            sprintf(name_msg, "%s has entered the game!\r\n", p->name);
                            broadcast(&game, name_msg);
                            announce_status(&game, p);
                            announce_turn(&game);

                        //if name not finished writing, just pass
                        } else if (name_len == -3){

                        //if name is NOT valid
                        } else {
                            if (name_len == 0){
                                sprintf(name_msg, "Please enter a non-empty name...\r\n");
                            } else if (name_len == -2){
                                sprintf(name_msg, "That name is already taken. Try another name\r\n");
                            } 
                            if (name_len == -1 || write(cur_fd, name_msg, strlen(name_msg)) == -1){
                                remove_player(&new_players, cur_fd);
                            }
                            null_terminate_all(p->name, MAX_NAME);
                        }
                        
                        FD_CLR(cur_fd, &rset);
                        break;
                    }
                }
            }
        }
    }
    return 0;
}
