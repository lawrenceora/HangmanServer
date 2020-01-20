#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "game.h"


/* Search the first count characters of buf for a network newline and return the index of '\r'
* or -1 if no network newline is found.
*/
int find_network_newline(const char *buf, int count){
    for (int i = 0; i < count-1; i++){
        if (buf[i] == '\r' && buf[i+1] == '\n'){
            return i;
        }
    }
    return -1;
}


/* Read from player->fd to *buf count bytes.
Preconditions: Calling this function would not block read().
- If the read ends in a network newline '\r\n':
        - replace it with '\0\0'
        - return 0.
- If the read does not end in a network newline:
        - set player->in_ptr to point to '\r\n'
        - retu***************
rn number of bytes read.
- On error:
        - call leave_handler
        - return -1.
*/
int game_read(struct game_state *game, struct client *player, char *buf, size_t count){
    int num_read = read(player->fd, player->in_ptr, count);
    printf("[%d] Read %d bytes\n", player->fd, num_read);
    int net_nl = find_network_newline(player->in_ptr, MAX_BUF);
    if (net_nl != -1){
        printf("[%d] Found newline %s", player->fd, player->in_ptr);
    }

    // If reading was unsuccessful
    if (num_read <= 0){
        leave_handler(game, player);
        return -1;
    // If network newline was not found
    } else if (net_nl == -1){
        player->in_ptr += num_read;
    } else {
        player->in_ptr[net_nl] = '\0';
        player->in_ptr[net_nl+1] = '\0';
        player->in_ptr = player->inbuf;
        return 0;
    }
    return num_read;
}


/* Write to player->fd count bytes starting from the location at buf.
    - Returns similar values as per write(), but calls leave_handler when an error occured.
*/
int game_write(struct game_state *game, struct client *player, char *buf, size_t count){
    int num_write = write(player->fd, buf, count);
    if (num_write == -1){
        leave_handler(game, player);
    }
    return num_write;
}


/* Send the message in outbuf to all clients */
void broadcast(struct game_state *game, char *outbuf){
    struct client *curr = game->head;
    while (curr != NULL){
        game_write(game, curr, outbuf, strlen(outbuf));
        curr = curr->next;
    }
}


/* Move the has_next_turn pointer to the next active client */
void advance_turn(struct game_state *game){
    // If first person was added/reached end of the list
    if (game->has_next_turn == NULL || game->has_next_turn->next == NULL){
        game->has_next_turn = game->head;
    } else {
        game->has_next_turn = game->has_next_turn->next;
    }

    // Display to server who's turn it is
    if (game->has_next_turn != NULL){
        printf("It's %s's turn\n", game->has_next_turn->name);
    }
}


/* Announce to all active players who's turn it is, and prompt the person to guess
*/
void announce_turn(struct game_state *game){
    char turn_msg[MAX_MSG];
    // Message for player with current turn
    char *your_turn = "Your guess?\r\n";

    // Message for other players
    if (game->has_next_turn != NULL){
        sprintf(turn_msg, "It's %s's turn\r\n", game->has_next_turn->name);
    }

    // Broadcast custom message
    struct client *curr = game->head;
    while (curr != NULL){
        if (curr != game->has_next_turn){
            game_write(game, curr, turn_msg, strlen(turn_msg));
        } else {
            game_write(game, curr, your_turn, strlen(your_turn));
        }
        curr = curr->next;
    }
}


/* Announce to the status of the game to player.
*   - If player is NULL, the message is broadcast to everyone.
*/
void announce_status(struct game_state *game, struct client *player){
    // Announce to everyone the status message of the game.
    char status_msg[MAX_MSG];
    status_message(status_msg, game);

    if (player == NULL){
        // Announce to everyone the status message of the game.
        broadcast(game, status_msg);
    } else {
        game_write(game, player, status_msg, strlen(status_msg));
    }

}


/* Announce to all players who the winner is. */
void announce_winner(struct game_state *game, struct client *winner){
    char *you_win = "You won!\r\n";
    char winner_msg[MAX_MSG];
    sprintf(winner_msg, "You lost. %s is the winner!\r\n", winner->name);

    struct client *curr = game->head;
    while (curr != NULL){
        if (curr != winner){
            game_write(game, curr, winner_msg, strlen(winner_msg));
        } else {
            game_write(game, curr, you_win, strlen(you_win));
        }
        curr = curr->next;
    }   
}


/* Process the guess of player.
*  Prerequisite: player->inbuf[0] is a valid lower case letter to guess
*/
void guess_char(struct game_state *game, struct client *player){
    char chr_msg[MAX_MSG];
    int word_len = strlen(game->word);
    int fail = 1;

    // Broadcast that someone guessed a letter
    sprintf(chr_msg, "%s guesses: %c\r\n", player->name, player->inbuf[0]);
    broadcast(game, chr_msg);
    game->letters_guessed[player->inbuf[0] - 'a'] = 1;

    // Update word->guess
    for (int i = 0; i < word_len; i++){
        if (game->word[i] == player->inbuf[0]){
            game->guess[i] = game->word[i];
            fail = 0;
        }
    }
    
    // Handle failed guesses
    if (fail == 1){
        printf("Letter %c is not in the word\n", player->inbuf[0]);
        sprintf(chr_msg, "Letter %c is not in the word\r\n", player->inbuf[0]);
        broadcast(game, chr_msg);
        game->guesses_left--;
        advance_turn(game);
    // Handle successful guesses
    } else {
        printf("Letter %c is in the word\n", player->inbuf[0]);
        sprintf(chr_msg, "Letter %c is a correct guess!\r\n", player->inbuf[0]);
        broadcast(game, chr_msg);        
    }

}


/* Processes input of the player who has the current turn.
    - If game_read returns an error, return -1.
    - If guess is invalid, inform the player and return 1.
    - If the guess is valid, return 0.
*/
int process_turn_input(struct game_state *game, struct client *player){
    int read_status = game_read(game, player, player->in_ptr, MAX_BUF);
    char guess_msg[MAX_MSG];
    guess_msg[0] = '\0';

    if (read_status != 0){
        return -1;
    } else if (strlen(player->in_ptr) == 0){
        sprintf(guess_msg, "Enter something non-empty...\r\n");
    } else if (strlen(player->in_ptr) > 1){
        sprintf(guess_msg, "That guess is too long. Input just one character\r\n");
    } else if ('a' > player->inbuf[0] || player->inbuf[0] > 'z'){
        sprintf(guess_msg, "Enter a lowercase letter...\r\n");
    } else if (game->letters_guessed[player->inbuf[0] - 'a'] == 1){
        sprintf(guess_msg, "That letter has already been guessed...\r\n");
    }
    // If it failed any of the above conditions,
    if (strlen(guess_msg) > 0){
        game_write(game, player, guess_msg, strlen(guess_msg));
        return 1;
    } else {
        guess_char(game, player);
        return 0;
    }
}


/* Checks if game is finished, and broadcast appropriate messages.
- Returns 1 if game is not over
- Returns 0 if game is over 
*/
int check_game_over(struct game_state *game, struct client *curr_player){
    /* Game over conditions (guesses_left == 0 or word successfully guessed) are mutually exclusive
       since guesses_left does not decrease when a letter is guessed successfully
    */ 

    // Check if there are no more guesses
    char game_over_msg[MAX_MSG];
    if (game->guesses_left == 0){
        printf("Evaluated game over due to zero remaining guesses\n");
        sprintf(game_over_msg, "No more guesses. The word was %s.\r\n", game->word);
        broadcast(game, game_over_msg);
        return 0;
    }

    // Check if word has been guessed
    int word_len = strlen(game->word);
    for (int i = 0; i < word_len; i++){
        if (game->guess[i] == '-'){
            return 1;
        }
    }
    
    announce_winner(game, curr_player);
    printf("Evaluated game over due to %s's victory\n", curr_player->name);
    sprintf(game_over_msg, "The word was %s\r\n", game->word);
    broadcast(game, game_over_msg);
    return 0;
}


/* Broadcasts and reinitiates the start of a new game */
void start_new_game(struct game_state *game, char *dict_name){
    char *new_game_msg = "STARTING NEW GAME\r\n";
    printf("New game\n");
    broadcast(game, new_game_msg);
    init_game(game, dict_name);
}


/* Return a status message that shows the current state of the game.
 * Assumes that the caller has allocated MAX_MSG bytes for msg.
 */
char *status_message(char *msg, struct game_state *game) {
    sprintf(msg, "***************\r\n"
           "Word to guess: %s\r\nGuesses remaining: %d\r\n"
           "Letters guessed: \r\n", game->guess, game->guesses_left);
    for(int i = 0; i < 26; i++){
        if(game->letters_guessed[i]) {
            int len = strlen(msg);
            msg[len] = (char)('a' + i);
            msg[len + 1] = ' ';
            msg[len + 2] = '\0';
        }
    }
    strncat(msg, "\r\n***************\r\n", MAX_MSG);
    return msg;
}


/* Initialize the gameboard: 
 *    - initialize dictionary
 *    - select a random word to guess from the dictionary file
 *    - set guess to all dashes ('-')
 *    - initialize the other fields
 * We can't initialize head and has_next_turn because these will have
 * different values when we use init_game to create a new game after one
 * has already been played
 */
void init_game(struct game_state *game, char *dict_name) {
    char buf[MAX_WORD];
    if(game->dict.fp != NULL) {
        rewind(game->dict.fp);
    } else {
        game->dict.fp = fopen(dict_name, "r");
        if(game->dict.fp == NULL) {
            perror("Opening dictionary");
            exit(1);
        }
    } 

    int index = random() % game->dict.size;
    printf("Looking for word at index %d\n", index);
    for(int i = 0; i <= index; i++) {
        if(!fgets(buf, MAX_WORD, game->dict.fp)){
            fprintf(stderr,"File ended before we found the entry index %d",index);
            exit(1);
        }
    } 

    // Found word
    if(buf[strlen(buf) - 1] == '\n') {  // from a unix file
        buf[strlen(buf) - 1] = '\0';
    } else {
        fprintf(stderr, "The dictionary file does not appear to have Unix line endings\n");
    }
    strncpy(game->word, buf, MAX_WORD);
    game->word[MAX_WORD-1] = '\0';
    for(int j = 0; j < strlen(game->word); j++) {
        game->guess[j] = '-';
    }
    game->guess[strlen(game->word)] = '\0';

    for(int i = 0; i < NUM_LETTERS; i++) {
        game->letters_guessed[i] = 0;
    }
    game->guesses_left = MAX_GUESSES;

}


/* Return the number of lines in the file
 */
int get_file_length(char *filename) {
    char buf[MAX_MSG];
    int count = 0;
    FILE *fp;
    if((fp = fopen(filename, "r")) == NULL) {
        perror("open");
        exit(1);
    }
    
    while(fgets(buf, MAX_MSG, fp) != NULL) {
        count++;
    }
    
    fclose(fp);
    return count;
}
