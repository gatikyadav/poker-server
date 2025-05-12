#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>  /* For struct timeval */
#include <errno.h>     /* Add this line for errno */
#include <time.h>      /* For time() */

#include "poker_client.h"
#include "client_action_handler.h"
#include "game_logic.h"
#include "logs.h"

#define BASE_PORT 2201
#define NUM_PORTS 6
#define BUFFER_SIZE 1024

// External global variables from game_logic.c
extern player_id_t g_dealer;
extern player_id_t g_player_turn;
extern int g_bet_size;
extern int g_player_bets[MAX_PLAYERS];

game_state_t game; //global variable to store our game state info

// Helper function to handle player disconnections consistently
void handle_player_disconnect(game_state_t *game, player_id_t pid) {
    log_info("Player %d disconnected", pid);
    game->player_status[pid] = PLAYER_LEFT;
    if (game->sockets[pid] > 0) {
        close(game->sockets[pid]);
        game->sockets[pid] = -1;
    }
}

int main(int argc, char **argv) {
    int server_fds[NUM_PORTS];
    int opt = 1;
    struct sockaddr_in server_address, client_address;
    socklen_t addrlen = sizeof(client_address);
    int rand_seed = argc == 2 ? atoi(argv[1]) : (int)time(NULL); // Store random seed for reuse

    // Initialize logging - critical for tests
    log_init("server");
    log_info("Server starting with seed %d", rand_seed);

    // Initialize socket descriptors with invalid values
    for (int i = 0; i < MAX_PLAYERS; i++) {
        game.sockets[i] = -1;
    }

    //Setup the server infrastructure and accept the 6 players on ports 2201, 2202, 2203, 2204, 2205, 2206
    for (int i = 0; i < NUM_PORTS; i++) {
        server_fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fds[i] < 0) {
            log_err("Socket creation failed for port %d", BASE_PORT + i);
            exit(EXIT_FAILURE);
        }

        // Set socket options to reuse address and port
        if (setsockopt(server_fds[i], SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            log_err("setsockopt failed for port %d", BASE_PORT + i);
            exit(EXIT_FAILURE);
        }

        memset(&server_address, 0, sizeof(server_address));
        server_address.sin_family = AF_INET;
        server_address.sin_addr.s_addr = INADDR_ANY;
        server_address.sin_port = htons(BASE_PORT + i);

        if (bind(server_fds[i], (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
            log_err("Bind failed for port %d", BASE_PORT + i);
            exit(EXIT_FAILURE);
        }

        if (listen(server_fds[i], 1) < 0) {
            log_err("Listen failed for port %d", BASE_PORT + i);
            exit(EXIT_FAILURE);
        }

        log_info("[Server] Listening on port %d", BASE_PORT + i);
    }

    // Accept connections from all 6 players
    for (int i = 0; i < NUM_PORTS; i++) {
        log_info("Waiting for player %d to connect...", i);
        int client_socket = accept(server_fds[i], (struct sockaddr *)&client_address, &addrlen);
        if (client_socket < 0) {
            log_err("Accept failed for player %d", i);
            exit(EXIT_FAILURE);
        }
        
        // Store the socket descriptor
        game.sockets[i] = client_socket;
        
        // Verify socket is valid
        if (game.sockets[i] <= 0) {
            log_err("Invalid socket descriptor %d for player %d", game.sockets[i], i);
            exit(EXIT_FAILURE);
        }
        
        log_info("[Server] Player %d connected on socket %d", i, game.sockets[i]);
    }

    // Store the socket descriptors before initializing game state
    int temp_sockets[MAX_PLAYERS];
    for (int i = 0; i < MAX_PLAYERS; i++) {
        temp_sockets[i] = game.sockets[i];
    }

    // Initialize game state with the random seed
    init_game_state(&game, 100, rand_seed);

    // Restore socket descriptors after game state initialization
    for (int i = 0; i < MAX_PLAYERS; i++) {
        game.sockets[i] = temp_sockets[i];
        log_info("Restored socket %d for player %d", game.sockets[i], i);
    }

    // Initialize the deck once at program start
    init_deck(game.deck, rand_seed);
    
    //Join state
    server_join(&game);

    // Game counter for testing
    int game_count = 0;

    while (1) {
        // Debug log - print all sockets
        for (int i = 0; i < MAX_PLAYERS; i++) {
            log_info("Player %d socket: %d", i, game.sockets[i]);
        }
        
        // READY state
        int active_players = server_ready(&game);
        log_info("Active players: %d", active_players);
        
        if (active_players < 2) {
            // Send HALT to remaining player if only one player is ready
            server_packet_t packet;
            packet.packet_type = HALT;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (game.player_status[i] == PLAYER_ACTIVE && game.sockets[i] > 0) {
                    log_info("Sending HALT to player %d on socket %d", i, game.sockets[i]);
                    send(game.sockets[i], &packet, sizeof(server_packet_t), 0);
                }
            }
            break;
        }
        
        // Reset game state for a new hand
        // Store socket descriptors before reset
        for (int i = 0; i < MAX_PLAYERS; i++) {
            temp_sockets[i] = game.sockets[i];
        }
        
        // Reset game state
        reset_game_state(&game);
        
        // For testing - log the game count
        game_count++;
        log_info("Game %d: Using random seed: %d", game_count, rand_seed);

        // Only shuffle the deck (no reinitializing with the same seed)
        shuffle_deck(game.deck);
        game.next_card = 0;
        
        // Restore socket descriptors after reset
        for (int i = 0; i < MAX_PLAYERS; i++) {
            game.sockets[i] = temp_sockets[i];
        }
        
        log_info("Starting new hand, dealer: %d, player_turn: %d", g_dealer, g_player_turn);
        
        // Reset pot size at the beginning of each hand
        game.pot_size = 0;
        
        // Reset all player bets
        for (int i = 0; i < MAX_PLAYERS; i++) {
            g_player_bets[i] = 0;
        }
        
        // DEAL TO PLAYERS
        server_deal(&game);
        log_info("Cards dealt to players");
        
        // PREFLOP BETTING
        log_info("Starting preflop betting, player turn: %d", g_player_turn);
        int betting_result = server_bet(&game);
        
        // If only one player remains, go to showdown
        if (betting_result == 1) {
            log_info("Only one player remains active, going to showdown");
            goto showdown;
        }
        
        // PLACE FLOP CARDS - Use server_community
        log_info("Dealing flop");
        server_community(&game);
        
        // FLOP BETTING
        log_info("Starting flop betting, player turn: %d", g_player_turn);
        betting_result = server_bet(&game);
        
        // If only one player remains, go to showdown
        if (betting_result == 1) {
            log_info("Only one player remains active, going to showdown");
            goto showdown;
        }
        
        // PLACE TURN CARD - Use server_community
        log_info("Dealing turn");
        server_community(&game);
        
        // TURN BETTING
        log_info("Starting turn betting, player turn: %d", g_player_turn);
        betting_result = server_bet(&game);
        
        // If only one player remains, go to showdown
        if (betting_result == 1) {
            log_info("Only one player remains active, going to showdown");
            goto showdown;
        }
        
        // PLACE RIVER CARD - Use server_community
        log_info("Dealing river");
        server_community(&game);
        
        // RIVER BETTING
        log_info("Starting river betting, player turn: %d", g_player_turn);
        betting_result = server_bet(&game);
        
showdown: {
        // ROUND_SHOWDOWN
        player_id_t winner = find_winner(&game);
        log_info("Showdown, winner: %d, pot: %d", winner, game.pot_size);
        
        // Update winner's stack with pot
        game.player_stacks[winner] += game.pot_size;
        
        // Send END packet to all players
        server_packet_t end_packet;
        build_end_packet(&game, winner, &end_packet);
        
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (game.player_status[i] != PLAYER_LEFT && game.sockets[i] > 0) {
                int send_result = send(game.sockets[i], &end_packet, sizeof(server_packet_t), 0);
                log_info("Sending END packet to player %d on socket %d", i, game.sockets[i]);
                
                if (send_result < 0) {
                    log_err("Error sending END packet to player %d: %s", i, strerror(errno));
                    handle_player_disconnect(&game, i);
                }
            }
        }
    }
    }

    log_info("[Server] Shutting down.");

    // Close all fds
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i < NUM_PORTS && server_fds[i] > 0) {
            close(server_fds[i]);
        }
        if (game.sockets[i] > 0) {
            close(game.sockets[i]);
        }
    }

    // Finalize logging
    log_fini();
    
    return 0;
}