#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>  /* For struct timeval */
#include <errno.h>
#include <fcntl.h>     /* For fcntl */

#include "poker_client.h"
#include "client_action_handler.h"
#include "game_logic.h"
#include "logs.h"

// Global variables to track game state
player_id_t g_dealer = 0;  // Initial dealer, will be updated based on active players
player_id_t g_player_turn = 1;  // Initial player turn, will be updated based on active players
int g_bet_size = 0;
int g_player_bets[MAX_PLAYERS] = {0};
int g_first_hand = 1;  // Flag to track if this is the first hand

// Set socket to blocking mode with no timeout
void set_socket_blocking(int socket_fd) {
    if (socket_fd <= 0) return;
    
    // Get the current flags
    int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags == -1) {
        log_err("Failed to get socket flags");
        return;
    }
    
    // Clear the O_NONBLOCK flag
    flags = flags & ~O_NONBLOCK;
    if (fcntl(socket_fd, F_SETFL, flags) == -1) {
        log_err("Failed to set socket to blocking mode");
    }
    
    // Remove any timeout
    struct timeval no_timeout;
    no_timeout.tv_sec = 0;
    no_timeout.tv_usec = 0;
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &no_timeout, sizeof(no_timeout));
}

// Set socket to non-blocking mode with a timeout
void set_socket_timeout(int socket_fd, int seconds) {
    if (socket_fd <= 0) return;
    
    struct timeval timeout;
    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;
    
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        log_err("Failed to set socket timeout");
    }
}

void print_game_state(game_state_t *game) {
    log_info("Dealer: %d, Player Turn: %d, Pot Size: %d", 
             g_dealer, g_player_turn, game->pot_size);
    
    log_info("Community Cards: ");
    for (int i = 0; i < 5; i++) {
        if (game->community_cards[i] != NOCARD) {
            log_info("%s ", card_name(game->community_cards[i]));
        }
    }
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (game->player_status[i] != PLAYER_LEFT) {
            log_info("Player %d: Status=%d, Stack=%d, Bet=%d, Cards=%s %s, Socket=%d", 
                   i, game->player_status[i], game->player_stacks[i], g_player_bets[i],
                   card_name(game->player_hands[i][0]), card_name(game->player_hands[i][1]),
                   game->sockets[i]);
        }
    }
}

void init_deck(card_t deck[DECK_SIZE], int seed) { //DO NOT TOUCH THIS FUNCTION
    srand(seed);
    int i = 0;
    for(int r = 0; r<13; r++){
        for(int s = 0; s<4; s++){
            deck[i++] = (r << SUITE_BITS) | s;
        }
    }
}

void shuffle_deck(card_t deck[DECK_SIZE]) { //DO NOT TOUCH THIS FUNCTION
    for(int i = 0; i<DECK_SIZE; i++){
        int j = rand() % DECK_SIZE;
        card_t temp = deck[i];
        deck[i] = deck[j];
        deck[j] = temp;
    }
}

void init_game_state(game_state_t *game, int starting_stack, int random_seed) {
    // Save socket descriptors
    int temp_sockets[MAX_PLAYERS];
    for (int i = 0; i < MAX_PLAYERS; i++) {
        temp_sockets[i] = game->sockets[i];
    }
    
    // Zero out game state but leave socket descriptors intact
    int next_card = game->next_card;
    memset(game, 0, sizeof(game_state_t));
    game->next_card = next_card;
    
    // Restore socket descriptors
    for (int i = 0; i < MAX_PLAYERS; i++) {
        game->sockets[i] = temp_sockets[i];
        
        // Ensure sockets are in blocking mode
        set_socket_blocking(game->sockets[i]);
    }
    
    // Initialize the deck - Note: actual initialization happens in main once at start,
    // separate from game state init
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        game->player_stacks[i] = starting_stack;
        game->player_status[i] = PLAYER_LEFT;
        game->player_hands[i][0] = NOCARD;
        game->player_hands[i][1] = NOCARD;
    }
    
    // Initialize community cards
    for (int i = 0; i < 5; i++) {
        game->community_cards[i] = NOCARD;
    }
    
    // Initialize global variables as per assignment requirements
    // Find the lowest-numbered player for the dealer
    g_dealer = 0;  // Default to 0, will be updated after JOIN/READY
    g_first_hand = 1;  // Mark this as the first hand
    
    // Every time new cards are added to the community, player 1 should be the first player to take a turn
    g_player_turn = 1;  // Default, will be updated after seeing active players
    g_bet_size = 0;
    memset(g_player_bets, 0, sizeof(g_player_bets));
    
    game->pot_size = 0;
    game->next_card = 0;
    
    log_info("Game state initialized, starting dealer: %d, starting stack: %d", g_dealer, starting_stack);
}

void reset_game_state(game_state_t *game) {
    // Save socket descriptors and player status
    int temp_sockets[MAX_PLAYERS];
    player_status_t temp_status[MAX_PLAYERS];
    int temp_stacks[MAX_PLAYERS];
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        temp_sockets[i] = game->sockets[i];
        temp_status[i] = game->player_status[i];
        temp_stacks[i] = game->player_stacks[i];
    }
    
    // Reset the game state
    game->next_card = 0;  // Reset to 0 to start from beginning of deck
    game->pot_size = 0;
    
    // Reset all bets
    for (int i = 0; i < MAX_PLAYERS; i++) {
        g_player_bets[i] = 0;
    }
    
    // Reset community cards
    for (int i = 0; i < 5; i++) {
        game->community_cards[i] = NOCARD;
    }
    
    // Restore socket descriptors and player status
    for (int i = 0; i < MAX_PLAYERS; i++) {
        game->sockets[i] = temp_sockets[i];
        game->player_status[i] = temp_status[i];
        game->player_stacks[i] = temp_stacks[i];
    }
    
    // Only rotate dealer if this is not the first hand
    if (!g_first_hand) {
        // Count active players first
        int active_players = 0;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (game->player_status[i] == PLAYER_ACTIVE) {
                active_players++;
            }
        }
        
        // Start from the current dealer and find the next active player
        player_id_t new_dealer = g_dealer;
        int count = 0;
        
        do {
            // Move to the next player
            new_dealer = (new_dealer + 1) % MAX_PLAYERS;
            count++;
            
            // If we've gone through all players and found no active one, break to avoid infinite loop
            if (count >= MAX_PLAYERS) {
                log_err("No active players found for dealer rotation");
                break;
            }
        } while (game->player_status[new_dealer] != PLAYER_ACTIVE);
        
        g_dealer = new_dealer;
        log_info("New dealer after rotation: %d", g_dealer);
    } else {
        // First hand is over, mark it for future hands
        g_first_hand = 0;
        
        // For the first hand, ensure the dealer is the lowest-numbered active player
        player_id_t new_dealer = 0;
        while (new_dealer < MAX_PLAYERS) {
            if (game->player_status[new_dealer] == PLAYER_ACTIVE) {
                break;
            }
            new_dealer++;
        }
        
        if (new_dealer < MAX_PLAYERS) {
            g_dealer = new_dealer;
            log_info("First hand complete, dealer set to lowest active player: %d", g_dealer);
        } else {
            log_err("No active players found for initial dealer");
        }
    }
    
    // Set player turn to player after dealer
    g_player_turn = g_dealer;
    int count = 0;
    
    // Find the next active player to take the first turn
    do {
        g_player_turn = (g_player_turn + 1) % MAX_PLAYERS;
        count++;
        
        // If we've gone through all players and found no active one, break to avoid infinite loop
        if (count >= MAX_PLAYERS) {
            log_err("No active players found for turn");
            break;
        }
    } while (game->player_status[g_player_turn] != PLAYER_ACTIVE);
    
    g_bet_size = 0;
    
    log_info("Game state reset, dealer: %d, player turn: %d", g_dealer, g_player_turn);
}

void server_join(game_state_t *game) {
    client_packet_t packet;
    
    log_info("Expected packet size: %ld bytes", sizeof(client_packet_t));
    
    // Process JOIN packets from all connected players
    for (int i = 0; i < MAX_PLAYERS; i++) {
        // Make sure socket is valid
        if (game->sockets[i] <= 0) {
            log_err("Invalid socket descriptor %d for player %d", game->sockets[i], i);
            game->player_status[i] = PLAYER_LEFT;
            continue;
        }
        
        log_info("Waiting for JOIN from player %d on socket %d", i, game->sockets[i]);
        
        // Try to receive the JOIN packet with a 10-second timeout
        set_socket_timeout(game->sockets[i], 10);
        int bytes_received = recv(game->sockets[i], &packet, sizeof(client_packet_t), 0);
        
        if (bytes_received <= 0) {
            log_err("Error receiving JOIN packet from player %d: %s", i, strerror(errno));
            // Consider player joined anyway
            game->player_status[i] = PLAYER_FOLDED;
        } else if (packet.packet_type == JOIN) {
            game->player_status[i] = PLAYER_FOLDED;
            log_info("[Server] Player %d joined with JOIN packet", i);
        } else {
            log_err("Unexpected packet type from player %d: %d", i, packet.packet_type);
            game->player_status[i] = PLAYER_FOLDED;
        }
        
        // Reset to blocking mode
        set_socket_blocking(game->sockets[i]);
    }
}

int server_ready(game_state_t *game) {
    client_packet_t packet;
    int active_players = 0;
    
    // Wait for READY or LEAVE packets from all players
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (game->player_status[i] == PLAYER_LEFT || game->sockets[i] <= 0) {
            continue;
        }
        
        log_info("Waiting for READY/LEAVE from player %d on socket %d", i, game->sockets[i]);
        
        // Try to receive with a 30-second timeout
        set_socket_timeout(game->sockets[i], 30);
        int bytes_received = recv(game->sockets[i], &packet, sizeof(client_packet_t), 0);
        
        if (bytes_received <= 0) {
            log_err("Error receiving READY/LEAVE packet from player %d: %s", i, strerror(errno));
            game->player_status[i] = PLAYER_LEFT;
            close(game->sockets[i]);
            game->sockets[i] = -1;
            continue;
        }
        
        log_info("Received packet type %d from player %d", packet.packet_type, i);
        
        // Reset to blocking mode
        set_socket_blocking(game->sockets[i]);
        
        if (packet.packet_type == READY) {
            game->player_status[i] = PLAYER_ACTIVE;
            active_players++;
            log_info("[Server] Player %d is ready", i);
        } else if (packet.packet_type == LEAVE) {
            game->player_status[i] = PLAYER_LEFT;
            close(game->sockets[i]);
            game->sockets[i] = -1;
            log_info("[Server] Player %d left", i);
        } else if (packet.packet_type == CHECK || packet.packet_type == CALL || 
                  packet.packet_type == RAISE || packet.packet_type == FOLD) {
            // These are game action packets that might arrive if client is out of sync
            // We'll treat these as READY for robustness
            game->player_status[i] = PLAYER_ACTIVE;
            active_players++;
            log_info("[Server] Received game action packet from player %d, treating as READY", i);
        } else {
            log_err("Unexpected packet type from player %d: %d", i, packet.packet_type);
            // Try to handle it gracefully - assume READY if not LEAVE
            if (packet.packet_type != LEAVE) {
                game->player_status[i] = PLAYER_ACTIVE;
                active_players++;
                log_info("[Server] Assuming player %d is ready", i);
            }
        }
    }
    
    // If this is the first hand, set the dealer to the lowest active player ID
    if (g_first_hand) {
        // Find the lowest-numbered active player
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (game->player_status[i] == PLAYER_ACTIVE) {
                g_dealer = i;
                log_info("Setting initial dealer to lowest active player: %d", g_dealer);
                break;
            }
        }
        
        // Set player turn to the next active player after the dealer
        g_player_turn = g_dealer;
        int count = 0;
        
        do {
            g_player_turn = (g_player_turn + 1) % MAX_PLAYERS;
            count++;
            
            if (count >= MAX_PLAYERS) {
                log_err("No next active player found for initial turn");
                break;
            }
        } while (game->player_status[g_player_turn] != PLAYER_ACTIVE);
        
        log_info("Initial settings - dealer: %d, player_turn: %d", g_dealer, g_player_turn);
    }
    
    log_info("Total active players: %d", active_players);
    return active_players;
}

void server_deal(game_state_t *game) {
    // Deal cards in player order as specified in the assignment:
    // "To standardize the behavior of our servers when you are dealing cards deal 2 cards to the
    // lowest numbered player who is "ready" first. Then move around the table to the next lowest
    // numbered player and give them 2 cards."
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (game->player_status[i] == PLAYER_ACTIVE) {
            game->player_hands[i][0] = game->deck[game->next_card++];
            game->player_hands[i][1] = game->deck[game->next_card++];
            log_info("Dealt cards to player %d: %s %s", i, 
                    card_name(game->player_hands[i][0]), 
                    card_name(game->player_hands[i][1]));
        }
    }
}

int server_bet(game_state_t *game) {
    client_packet_t client_packet;
    server_packet_t server_packet;
    int active_players = 0;
    int betting_complete = 0;
    player_id_t current_player = g_player_turn;
    player_id_t start_player = g_player_turn; // Remember where we started
    int all_players_acted = 0;
    player_id_t last_info_sent_player = -1; // Track the last player for whom INFO was sent
    
    // Add a tracking array to ensure all players have had a chance to act since the last raise
    int player_acted_since_last_raise[MAX_PLAYERS] = {0};
    
    log_info("Starting betting round with dealer: %d, first player: %d", g_dealer, current_player);
    
    // Count active players
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (game->player_status[i] == PLAYER_ACTIVE) {
            active_players++;
        }
    }
    
    // Set the last raiser to be invalid initially (no raises yet)
    player_id_t last_raiser = -1;
    
    // If only one player is active, they win
    if (active_players <= 1) {
        log_info("Only one active player remains, ending betting round");
        return 1;
    }
    
    // Start betting round
    while (!betting_complete) {
        // Skip inactive players or players with invalid sockets
        while (game->player_status[current_player] != PLAYER_ACTIVE || game->sockets[current_player] <= 0) {
            current_player = (current_player + 1) % MAX_PLAYERS;
            if (current_player == start_player) {
                // If we've gone all the way around and found no active players, there's a problem
                log_err("No active players found for betting round");
                return 1;
            }
        }
        
        // Set current player
        g_player_turn = current_player;
        
        // Only send INFO packets if we're moving to a new player or starting the betting round
        if (current_player != last_info_sent_player) {
            last_info_sent_player = current_player;
            
            // Send INFO packet to all players
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (game->player_status[i] != PLAYER_LEFT && game->sockets[i] > 0) {
                    build_info_packet(game, i, &server_packet);
                    int send_result = send(game->sockets[i], &server_packet, sizeof(server_packet_t), 0);
                    log_info("Sent INFO packet to player %d on socket %d (result: %d)", i, game->sockets[i], send_result);
                    
                    if (send_result < 0) {
                        log_err("Error sending INFO packet to player %d: %s", i, strerror(errno));
                        game->player_status[i] = PLAYER_LEFT;
                        close(game->sockets[i]);
                        game->sockets[i] = -1;
                        
                        // If this was the current player's turn, find the next active player
                        if (i == current_player) {
                            current_player = (current_player + 1) % MAX_PLAYERS;
                            last_info_sent_player = -1; // Reset this so INFO will be sent for next player
                            // Reset round tracking if we had to skip the current player
                            continue;
                        }
                    }
                }
            }
        }
        
        // Wait for current player's action
        log_info("Waiting for action from player %d on socket %d", current_player, game->sockets[current_player]);
        
        // Set a 30-second timeout for action
        set_socket_timeout(game->sockets[current_player], 30);
        int bytes_received = recv(game->sockets[current_player], &client_packet, sizeof(client_packet_t), 0);
        
        // Reset to blocking mode
        set_socket_blocking(game->sockets[current_player]);
        
        if (bytes_received <= 0) {
            log_err("Error receiving action from player %d: %s", current_player, strerror(errno));
            game->player_status[current_player] = PLAYER_LEFT;
            close(game->sockets[current_player]);
            game->sockets[current_player] = -1;
            
            // Recount active players
            active_players = 0;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (game->player_status[i] == PLAYER_ACTIVE) {
                    active_players++;
                }
            }
            
            // If only one player left active, end betting
            if (active_players <= 1) {
                log_info("Only one active player remains after disconnect, ending betting round");
                return 1;
            }
            
            // Find the next active player
            player_id_t prev_player = current_player;
            current_player = (current_player + 1) % MAX_PLAYERS;
            last_info_sent_player = -1; // Reset this so INFO will be sent for next player
            int count = 0;
            
            while ((game->player_status[current_player] != PLAYER_ACTIVE || game->sockets[current_player] <= 0) && count < MAX_PLAYERS) {
                current_player = (current_player + 1) % MAX_PLAYERS;
                count++;
                
                if (count >= MAX_PLAYERS) {
                    log_err("No active players found after trying all players");
                    return 1;
                }
            }
            continue;
        }
        
        log_info("Received %d bytes from player %d", bytes_received, current_player);
        
        // Process the player's action
        int result = handle_client_action(game, current_player, &client_packet, &server_packet);
        
        // Send ACK/NACK
        server_packet.packet_type = (result == 0) ? ACK : NACK;
        int send_result = send(game->sockets[current_player], &server_packet, sizeof(server_packet_t), 0);
        log_info("Sent %s to player %d (result: %d)", (result == 0) ? "ACK" : "NACK", current_player, send_result);
        
        if (send_result < 0) {
            log_err("Error sending response to player %d: %s", current_player, strerror(errno));
            game->player_status[current_player] = PLAYER_LEFT;
            close(game->sockets[current_player]);
            game->sockets[current_player] = -1;
            
            // Recount active players
            active_players = 0;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (game->player_status[i] == PLAYER_ACTIVE) {
                    active_players++;
                }
            }
            
            // If only one player left active, end betting
            if (active_players <= 1) {
                log_info("Only one active player remains after failed ACK/NACK, ending betting round");
                return 1;
            }
            
            // Find the next active player
            player_id_t prev_player = current_player;
            current_player = (current_player + 1) % MAX_PLAYERS;
            last_info_sent_player = -1; // Reset this so INFO will be sent for next player
            int count = 0;
            
            while ((game->player_status[current_player] != PLAYER_ACTIVE || game->sockets[current_player] <= 0) && count < MAX_PLAYERS) {
                current_player = (current_player + 1) % MAX_PLAYERS;
                count++;
                
                if (count >= MAX_PLAYERS) {
                    log_err("No active players found after trying all players");
                    return 1;
                }
            }
            continue;
        }
        
        if (result == 0) {
            // Valid action, mark this player as having acted
            player_acted_since_last_raise[current_player] = 1;
            
            // If this was a raise, update the last raiser and reset the acted flags for other players
            if (client_packet.packet_type == RAISE) {
                last_raiser = current_player;
                for (int i = 0; i < MAX_PLAYERS; i++) {
                    if (i != current_player && game->player_status[i] == PLAYER_ACTIVE) {
                        player_acted_since_last_raise[i] = 0;
                    }
                }
                log_info("Player %d raised, resetting acted flags for other players", current_player);
            }
            
            // Valid action, move to next player
            player_id_t prev_player = current_player;
            current_player = (current_player + 1) % MAX_PLAYERS;
            last_info_sent_player = -1; // Reset this so INFO will be sent for next player
            
            // Skip inactive players
            int count = 0;
            while ((game->player_status[current_player] != PLAYER_ACTIVE || game->sockets[current_player] <= 0) && count < MAX_PLAYERS) {
                current_player = (current_player + 1) % MAX_PLAYERS;
                count++;
                
                if (count >= MAX_PLAYERS) {
                    log_err("No next active player found");
                    return 1;
                }
            }
            
            // Check if we've completed one full round
            if (current_player == start_player) {
                all_players_acted = 1;
            }
            
            // Recount active players
            active_players = 0;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (game->player_status[i] == PLAYER_ACTIVE) {
                    active_players++;
                    log_info("Player %d is active", i);
                }
            }
            
            // If only one player left active, end betting
            if (active_players <= 1) {
                log_info("Only one active player remains, ending betting round");
                return 1;
            }
            
            // Check if all players have acted since the last raise
            int all_acted_since_last_raise = 1;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (game->player_status[i] == PLAYER_ACTIVE && !player_acted_since_last_raise[i]) {
                    all_acted_since_last_raise = 0;
                    log_info("Player %d has not acted since the last raise", i);
                    break;
                }
            }
            
            // Check if betting round is complete
            if (all_acted_since_last_raise && check_betting_end(game)) {
                log_info("Betting round complete, all players have matched bets");
                betting_complete = 1;
            }
        } else {
            // Invalid action, send a new INFO packet just to this player
            build_info_packet(game, current_player, &server_packet);
            send_result = send(game->sockets[current_player], &server_packet, sizeof(server_packet_t), 0);
            log_info("Resending INFO packet to player %d after invalid action (result: %d)", 
                     current_player, send_result);
            
            if (send_result < 0) {
                log_err("Error sending INFO packet to player %d after invalid action: %s", 
                        current_player, strerror(errno));
                game->player_status[current_player] = PLAYER_LEFT;
                close(game->sockets[current_player]);
                game->sockets[current_player] = -1;
                
                // Find next active player
                current_player = (current_player + 1) % MAX_PLAYERS;
                last_info_sent_player = -1;
                
                // Recount active players
                active_players = 0;
                for (int i = 0; i < MAX_PLAYERS; i++) {
                    if (game->player_status[i] == PLAYER_ACTIVE) {
                        active_players++;
                    }
                }
                
                if (active_players <= 1) {
                    log_info("Only one active player remains after INFO packet error, ending betting round");
                    return 1;
                }
            }
            
            // Keep the same player's turn
            continue;
        }
    }
    
    // Add all bets to pot
    log_info("Adding bets to pot: current pot=%d", game->pot_size);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        log_info("Player %d bet: %d", i, g_player_bets[i]);
        game->pot_size += g_player_bets[i];
        g_player_bets[i] = 0;
    }
    log_info("New pot size: %d", game->pot_size);
    g_bet_size = 0;
    
    // Reset player turn to player after dealer for next betting round
    g_player_turn = g_dealer;
    int count = 0;
    do {
        g_player_turn = (g_player_turn + 1) % MAX_PLAYERS;
        count++;
        if (count >= MAX_PLAYERS) {
            log_err("No active players found to set as next player turn");
            break;
        }
    } while (game->player_status[g_player_turn] != PLAYER_ACTIVE);
    
    log_info("Ending betting round, pot size: %d, next player turn: %d", game->pot_size, g_player_turn);
    
    return 0;
}

int check_betting_end(game_state_t *game) {
    int first_active_player = -1;
    int first_active_bet = -1;
    int all_equal = 1;
    
    // Find the first active player and their bet
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (game->player_status[i] == PLAYER_ACTIVE) {
            first_active_player = i;
            first_active_bet = g_player_bets[i];
            log_info("First active player %d has bet %d", i, first_active_bet);
            break;
        }
    }
    
    // If no active players found (shouldn't happen)
    if (first_active_player == -1) {
        log_err("No active players found in check_betting_end");
        return 1;  // Return true to avoid getting stuck
    }
    
    // Check if all active players have equal bets
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (game->player_status[i] == PLAYER_ACTIVE) {
            log_info("Player %d is active with bet: %d, comparing to first_active_bet: %d", 
                     i, g_player_bets[i], first_active_bet);
            if (g_player_bets[i] != first_active_bet) {
                all_equal = 0;
                log_info("Bets not equal, betting continues");
                break;
            }
        }
    }
    
    log_info("check_betting_end: all bets equal? %s", all_equal ? "yes" : "no");
    return all_equal;
}

void server_community(game_state_t *game) {
    // Count how many community cards are already dealt
    int card_count = 0;
    for (int i = 0; i < 5; i++) {
        if (game->community_cards[i] != NOCARD) {
            card_count++;
        }
    }
    
    // Deal appropriate cards based on current state
    // As per assignment: "When dealing the 5 community cards, they should be
    // the next 5 cards in order (from the deck) after all "ready" players have been given 2 cards."
    if (card_count == 0) {
        // Deal the flop (3 cards)
        game->community_cards[0] = game->deck[game->next_card++];
        game->community_cards[1] = game->deck[game->next_card++];
        game->community_cards[2] = game->deck[game->next_card++];
        log_info("Dealt flop: %s %s %s", 
                card_name(game->community_cards[0]),
                card_name(game->community_cards[1]),
                card_name(game->community_cards[2]));
    } else if (card_count == 3) {
        // Deal the turn (1 card)
        game->community_cards[3] = game->deck[game->next_card++];
        log_info("Dealt turn: %s", card_name(game->community_cards[3]));
    } else if (card_count == 4) {
        // Deal the river (1 card)
        game->community_cards[4] = game->deck[game->next_card++];
        log_info("Dealt river: %s", card_name(game->community_cards[4]));
    }
    
    // Reset bet size for the new betting round
    g_bet_size = 0;
    
    // Reset all player bets for this new betting round
    for (int i = 0; i < MAX_PLAYERS; i++) {
        g_player_bets[i] = 0;
    }
    
    // Reset player turn to player after dealer for this new betting round
    g_player_turn = g_dealer;
    int count = 0;
    
    // Find the first active player after the dealer to start the betting
    do {
        g_player_turn = (g_player_turn + 1) % MAX_PLAYERS;
        count++;
        
        if (count >= MAX_PLAYERS) {
            log_err("No active players found after dealer to start betting");
            break;
        }
    } while (game->player_status[g_player_turn] != PLAYER_ACTIVE);
    
    log_info("After dealing community cards, player turn: %d, bet size reset to 0", g_player_turn);
    
    // Send updated INFO packets to all players to inform them of the new community cards
    server_packet_t info_packet;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (game->player_status[i] != PLAYER_LEFT && game->sockets[i] > 0) {
            build_info_packet(game, i, &info_packet);
            int send_result = send(game->sockets[i], &info_packet, sizeof(server_packet_t), 0);
            log_info("Sending INFO packet with community cards to player %d", i);
            
            if (send_result < 0) {
                log_err("Error sending INFO packet with community cards to player %d: %s", 
                        i, strerror(errno));
                game->player_status[i] = PLAYER_LEFT;
                close(game->sockets[i]);
                game->sockets[i] = -1;
            }
        }
    }
}

void server_end(game_state_t *game) {
    player_id_t winner = find_winner(game);
    server_packet_t end_packet;
    
    // Update winner's stack
    game->player_stacks[winner] += game->pot_size;
    log_info("Player %d wins pot of %d", winner, game->pot_size);
    
    // Build and send END packet
    build_end_packet(game, winner, &end_packet);
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (game->player_status[i] != PLAYER_LEFT && game->sockets[i] > 0) {
            int send_result = send(game->sockets[i], &end_packet, sizeof(server_packet_t), 0);
            log_info("Sent END packet to player %d (result: %d)", i, send_result);
            if (send_result < 0) {
                log_err("Error sending END packet to player %d: %s", i, strerror(errno));
                game->player_status[i] = PLAYER_LEFT;
                close(game->sockets[i]);
                game->sockets[i] = -1;
            }
        }
    }
}

int evaluate_hand(game_state_t *game, player_id_t pid) {
    // Combine player's hole cards with community cards
    card_t cards[7];
    int num_cards = 0;
    
    // Add player's hole cards
    if (game->player_hands[pid][0] != NOCARD) {
        cards[num_cards++] = game->player_hands[pid][0];
    }
    if (game->player_hands[pid][1] != NOCARD) {
        cards[num_cards++] = game->player_hands[pid][1];
    }
    
    // Add community cards
    for (int i = 0; i < 5; i++) {
        if (game->community_cards[i] != NOCARD) {
            cards[num_cards++] = game->community_cards[i];
        }
    }
    
    // Count occurrences of each rank (A-K)
    int rank_counts[13] = {0};
    for (int i = 0; i < num_cards; i++) {
        rank_counts[(cards[i] >> SUITE_BITS)]++; // Extract rank bits
    }
    
    // Count occurrences of each suit
    int suit_counts[4] = {0};
    for (int i = 0; i < num_cards; i++) {
        suit_counts[(cards[i] & ((1 << SUITE_BITS) - 1))]++; // Extract suit bits
    }
    
    // Check for flush (5+ cards of same suit)
    int flush_suit = -1;
    for (int s = 0; s < 4; s++) {
        if (suit_counts[s] >= 5) {
            flush_suit = s;
            break;
        }
    }
    
    // Check for straight (5+ consecutive ranks)
    int straight_high = -1;
    int consecutive = 0;
    int prev_rank = -1;
    
    // Sort cards by rank
    for (int i = 0; i < num_cards - 1; i++) {
        for (int j = i + 1; j < num_cards; j++) {
            if ((cards[i] >> SUITE_BITS) > (cards[j] >> SUITE_BITS)) {
                card_t temp = cards[i];
                cards[i] = cards[j];
                cards[j] = temp;
            }
        }
    }
    
    // Check for consecutive ranks
    for (int i = 0; i < num_cards; i++) {
        int rank = (cards[i] >> SUITE_BITS);
        
        if (prev_rank == -1 || rank == prev_rank + 1) {
            consecutive++;
            if (consecutive >= 5) {
                straight_high = rank;
            }
        } else if (rank != prev_rank) {
            consecutive = 1;
        }
        
        prev_rank = rank;
    }
    
    // Check for pairs, three-of-a-kind, and four-of-a-kind
    int one_pairs = 0, two_pairs = 0, three_kind = 0, four_kind = 0;
    int pair_ranks[2] = {-1, -1};
    int three_rank = -1, four_rank = -1;
    
    for (int r = 12; r >= 0; r--) { // Start from Ace (highest)
        if (rank_counts[r] == 4) {
            four_kind++;
            four_rank = r;
        } else if (rank_counts[r] == 3) {
            three_kind++;
            three_rank = r;
        } else if (rank_counts[r] == 2) {
            if (one_pairs < 2) {
                pair_ranks[one_pairs] = r;
            }
            one_pairs++;
            if (one_pairs == 2) {
                two_pairs = 1;
            }
        }
    }
    
    // Calculate hand score (higher is better)
    int high_card = -1;
    for (int i = 0; i < num_cards; i++) {
        int rank = (cards[i] >> SUITE_BITS);
        if (rank > high_card) {
            high_card = rank;
        }
    }
    
    // Check for straight flush
    if (flush_suit >= 0 && straight_high >= 0) {
        // Verify the straight is all the same suit
        int straight_flush = 1;
        for (int i = 0; i < num_cards - 1; i++) {
            if ((cards[i] >> SUITE_BITS) == straight_high - 4 && 
                (cards[i + 1] >> SUITE_BITS) == straight_high - 3 && 
                (cards[i + 2] >> SUITE_BITS) == straight_high - 2 && 
                (cards[i + 3] >> SUITE_BITS) == straight_high - 1 && 
                (cards[i + 4] >> SUITE_BITS) == straight_high) {
                if ((cards[i] & ((1 << SUITE_BITS) - 1)) != flush_suit || 
                    (cards[i + 1] & ((1 << SUITE_BITS) - 1)) != flush_suit || 
                    (cards[i + 2] & ((1 << SUITE_BITS) - 1)) != flush_suit || 
                    (cards[i + 3] & ((1 << SUITE_BITS) - 1)) != flush_suit || 
                    (cards[i + 4] & ((1 << SUITE_BITS) - 1)) != flush_suit) {
                    straight_flush = 0;
                }
                break;
            }
        }
        
        if (straight_flush) {
            return 8000000 + straight_high;
        }
    }
    
    // Four of a kind
    if (four_kind > 0) {
        return 7000000 + four_rank;
    }
    
    // Full house
    if (three_kind > 0 && one_pairs > 0) {
        return 6000000 + three_rank;
    }
    
    // Flush
    if (flush_suit >= 0) {
        return 5000000 + high_card;
    }
    
    // Straight
    if (straight_high >= 0) {
        return 4000000 + straight_high;
    }
    
    // Three of a kind
    if (three_kind > 0) {
        return 3000000 + three_rank;
    }
    
    // Two pair
    if (two_pairs > 0) {
        return 2000000 + pair_ranks[0];
    }
    
    // One pair
    if (one_pairs > 0) {
        return 1000000 + pair_ranks[0];
    }
    
    // High card
    return high_card;
}

player_id_t find_winner(game_state_t *game) {
    // Find the last active player if everyone else folded
    int active_count = 0;
    player_id_t last_active = -1;
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (game->player_status[i] == PLAYER_ACTIVE) {
            active_count++;
            last_active = i;
        }
    }
    
    if (active_count == 1) {
        log_info("Only one active player (%d) left, they win by default", last_active);
        return last_active; // Only one player left, they win
    }
    
    // Find player with best hand
    player_id_t best_player = -1;
    int best_score = -1;
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (game->player_status[i] == PLAYER_ACTIVE) {
            int score = evaluate_hand(game, i);
            log_info("Player %d hand score: %d", i, score);
            
            if (best_player == -1 || score > best_score) {
                best_player = i;
                best_score = score;
            }
        }
    }
    
    log_info("Player %d wins with best hand (score: %d)", best_player, best_score);
    return best_player;
}