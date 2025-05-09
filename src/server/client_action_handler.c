#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "client_action_handler.h"
#include "game_logic.h"
#include "logs.h"

// External global variables from game_logic.c
extern player_id_t g_dealer;
extern player_id_t g_player_turn;
extern int g_bet_size;
extern int g_player_bets[MAX_PLAYERS];

int handle_client_action(game_state_t *game, player_id_t pid, const client_packet_t *in, server_packet_t *out) {
    // Check if it's the player's turn
    if (pid != g_player_turn) {
        out->packet_type = NACK;
        log_info("NACK: Not player %d's turn", pid);
        return -1;
    }
    
    // Check player status
    if (game->player_status[pid] != PLAYER_ACTIVE) {
        out->packet_type = NACK;
        log_info("NACK: Player %d not active", pid);
        return -1;
    }
    
    // Handle different action types
    switch (in->packet_type) {
        case CHECK: {
            // Check is only valid if the current bet is 0
            if (g_bet_size > 0) {
                out->packet_type = NACK;
                log_info("NACK: Player %d cannot check, bet size is %d", pid, g_bet_size);
                return -1;
            }
            log_info("Player %d checks", pid);
            break;
        }
            
        case CALL: {
            // Call is valid only if there's a bet to match
            if (g_bet_size == 0) {
                out->packet_type = NACK;
                log_info("NACK: Player %d cannot call, no bet to match", pid);
                return -1;
            }
            
            // Player matches the current bet
            int to_call = g_bet_size - g_player_bets[pid];
            
            // Check if player has enough money
            if (to_call > game->player_stacks[pid]) {
                // Player goes all-in
                to_call = game->player_stacks[pid];
            }
            
            game->player_stacks[pid] -= to_call;
            g_player_bets[pid] += to_call;
            log_info("Player %d calls %d", pid, to_call);
            break;
        }
            
        case RAISE: {
            // Get the raise amount from parameters
            int raise_amount = in->params[0];
            
            // Check if raise amount is valid (greater than current bet)
            if (raise_amount <= g_bet_size) {
                out->packet_type = NACK;
                log_info("NACK: Player %d's raise of %d not greater than current bet %d", 
                       pid, raise_amount, g_bet_size);
                return -1;
            }
            
            // Check if player has enough money
            int to_raise = raise_amount - g_player_bets[pid];
            if (to_raise > game->player_stacks[pid]) {
                out->packet_type = NACK;
                log_info("NACK: Player %d doesn't have enough chips for raise", pid);
                return -1;
            }
            
            // Update player stack and bet
            game->player_stacks[pid] -= to_raise;
            g_player_bets[pid] += to_raise;
            g_bet_size = raise_amount;
            log_info("Player %d raises to %d", pid, raise_amount);
            break;
        }
            
        case FOLD: {
            // Player folds, mark as inactive
            game->player_status[pid] = PLAYER_FOLDED;
            log_info("Player %d folds", pid);
            break;
        }
            
        default: {
            // Invalid action type
            out->packet_type = NACK;
            log_info("NACK: Invalid action type from player %d", pid);
            return -1;
        }
    }
    
    // Action was valid
    out->packet_type = ACK;
    return 0;
}

void build_info_packet(game_state_t *game, player_id_t pid, server_packet_t *out) {
    // Set the packet type
    out->packet_type = INFO;
    
    // Copy player's hole cards
    out->info.player_cards[0] = game->player_hands[pid][0];
    out->info.player_cards[1] = game->player_hands[pid][1];
    
    // Copy community cards
    for (int i = 0; i < 5; i++) {
        out->info.community_cards[i] = game->community_cards[i];
    }
    
    // Copy player stacks
    for (int i = 0; i < MAX_PLAYERS; i++) {
        out->info.player_stacks[i] = game->player_stacks[i];
    }
    
    // Set pot size
    out->info.pot_size = game->pot_size;
    
    // Set dealer and player turn
    out->info.dealer = g_dealer;
    out->info.player_turn = g_player_turn;
    
    // Set bet size
    out->info.bet_size = g_bet_size;
    
    // Copy player bets
    for (int i = 0; i < MAX_PLAYERS; i++) {
        out->info.player_bets[i] = g_player_bets[i];
    }
    
    // Copy player statuses - Fixed mapping between internal status and packet status
    for (int i = 0; i < MAX_PLAYERS; i++) {
        // Convert internal status to info packet status
        if (game->player_status[i] == PLAYER_ACTIVE) {
            out->info.player_status[i] = 1;  // Active
        } else if (game->player_status[i] == PLAYER_FOLDED) {
            out->info.player_status[i] = 0;  // Folded
        } else {
            out->info.player_status[i] = 2;  // Left
        }
    }
}

void build_end_packet(game_state_t *game, player_id_t winner, server_packet_t *out) {
    // Set the packet type
    out->packet_type = END;
    
    // Copy all player hands
    for (int i = 0; i < MAX_PLAYERS; i++) {
        out->end.player_cards[i][0] = game->player_hands[i][0];
        out->end.player_cards[i][1] = game->player_hands[i][1];
    }
    
    // Copy community cards
    for (int i = 0; i < 5; i++) {
        out->end.community_cards[i] = game->community_cards[i];
    }
    
    // Copy player stacks
    for (int i = 0; i < MAX_PLAYERS; i++) {
        out->end.player_stacks[i] = game->player_stacks[i];
    }
    
    // Set pot size
    out->end.pot_size = game->pot_size;
    
    // Set dealer and winner
    out->end.dealer = g_dealer;
    out->end.winner = winner;
    
    // Copy player statuses - Fixed mapping between internal status and packet status
    for (int i = 0; i < MAX_PLAYERS; i++) {
        // Convert internal status to end packet status
        if (game->player_status[i] == PLAYER_ACTIVE) {
            out->end.player_status[i] = 1;  // Active
        } else if (game->player_status[i] == PLAYER_FOLDED) {
            out->end.player_status[i] = 0;  // Folded
        } else {
            out->end.player_status[i] = 2;  // Left
        }
    }
}