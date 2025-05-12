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
    // Log the current game state for debugging
    log_info("handle_client_action: Current dealer=%d, player_turn=%d, bet_size=%d", 
             g_dealer, g_player_turn, g_bet_size);
    
    // Check if it's the player's turn
    if (pid != g_player_turn) {
        out->packet_type = NACK;
        log_info("NACK: Not player %d's turn, current turn is player %d", pid, g_player_turn);
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
            // Check is only valid if the current bet is 0 or player has already matched it
            if (g_bet_size > g_player_bets[pid]) {
                out->packet_type = NACK;
                log_info("NACK: Player %d cannot check, bet size is %d, player bet is %d", 
                       pid, g_bet_size, g_player_bets[pid]);
                return -1;
            }
            log_info("Player %d checks", pid);
            break;
        }
            
        case CALL: {
            // Call is valid only if there's a bet to match
            if (g_bet_size == 0 || g_bet_size <= g_player_bets[pid]) {
                out->packet_type = NACK;
                log_info("NACK: Player %d cannot call, no bet to match or already matched", pid);
                return -1;
            }
            
            // Player matches the current bet
            int to_call = g_bet_size - g_player_bets[pid];
            
            // Check if player has enough money
            if (to_call > game->player_stacks[pid]) {
                // Player goes all-in
                to_call = game->player_stacks[pid];
                log_info("Player %d going all-in with %d", pid, to_call);
            }
            
            game->player_stacks[pid] -= to_call;
            g_player_bets[pid] += to_call;
            log_info("Player %d calls %d, total bet now %d, stack now %d", 
                   pid, to_call, g_player_bets[pid], game->player_stacks[pid]);
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
            
            // Calculate how much more the player needs to put in
            int current_bet = g_player_bets[pid];
            int to_raise = raise_amount - current_bet;
            
            // Check if player has enough money for the raise
            if (to_raise > game->player_stacks[pid]) {
                out->packet_type = NACK;
                log_info("NACK: Player %d doesn't have enough chips for raise. Needed: %d, Has: %d", 
                       pid, to_raise, game->player_stacks[pid]);
                return -1;
            }
            
            // Update player stack and bet
            game->player_stacks[pid] -= to_raise;
            g_player_bets[pid] += to_raise;
            g_bet_size = raise_amount;
            log_info("Player %d raises to %d, total bet now %d, stack now %d", 
                   pid, raise_amount, g_player_bets[pid], game->player_stacks[pid]);
            break;
        }
            
        case FOLD: {
            // Player folds, mark as inactive
            game->player_status[pid] = PLAYER_FOLDED;
            log_info("Player %d folds", pid);
            break;
        }
            
        case READY:
        case LEAVE: {
            // These actions should not be handled here during betting rounds
            out->packet_type = NACK;
            log_info("NACK: Invalid action type %d from player %d during betting round", 
                   in->packet_type, pid);
            return -1;
        }
            
        default: {
            // Invalid action type
            out->packet_type = NACK;
            log_info("NACK: Unknown action type %d from player %d", in->packet_type, pid);
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
    
    // Calculate current pot size including all bets
    int current_pot = game->pot_size;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        current_pot += g_player_bets[i];
    }
    
    // Set pot size in the packet
    out->info.pot_size = current_pot;
    
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
    
    // Calculate total from current bets
    int current_bets_total = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        current_bets_total += g_player_bets[i];
    }
    
    // Calculate if we need to add current bets to the winner's stack
    // If current_bets_total > 0, it means the bets haven't been collected into the pot yet
    int need_to_add_bets = (current_bets_total > 0);
    
    // Copy stacks from game state
    for (int i = 0; i < MAX_PLAYERS; i++) {
        out->end.player_stacks[i] = game->player_stacks[i];
    }
    
    // For the winner, add current bets if they haven't been added to the pot yet
    if (need_to_add_bets) {
        out->end.player_stacks[winner] += current_bets_total;
    }
    
    // Calculate final pot size for display (includes current bets)
    int total_pot = game->pot_size + current_bets_total;
    out->end.pot_size = total_pot;
    
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