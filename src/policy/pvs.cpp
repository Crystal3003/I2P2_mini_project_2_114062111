#include <utility>
#include "state.hpp"
#include "pvs.hpp"
#include <algorithm>
/*============================================================
* Ordering function for moves. Sorts by piece value of captured piece (descending).
==============================================================*/
static const int piece_values[7] = {0, 10, 30, 35, 40, 100, 10000};

static int score_move(State *state, const Move& move){
    int self = state->player;
    int oppn = 1 - self;

    Point from = move.first;
    Point to = move.second;

    int attacker = state->board.board[self][from.first][from.second];
    int victim = state->board.board[oppn][to.first][to.second];

    // 1. First priority (Capture)
    if (victim > 0){
        // Formula: value of victim * 10 - value of attacker
        if (attacker == 6){
            // If the attacker is a king, we reduce the attacker's value to 100
            return 20000 + (piece_values[victim]*10 - 100);
        }
        return 20000 + (piece_values[victim]*10 - piece_values[attacker]);
    }
    
    // 2. Second priority (Pawn promotion)
    if (attacker == 1 && (to.first == 5 || to.first == 0)){
        return 19000;
    }

    // 3. Third priority (PST value bonus)
    if (attacker >= 1 && attacker <= 6){
        int piece_idx = attacker - 1;

        int from_value = pst[piece_idx][from.first][from.second];
        int to_value = pst[piece_idx][to.first][to.second];

        return 1000 + (to_value - from_value);
    }

    return 0;
}

static void order_moves(State* state){
    if (state->legal_actions.empty()){
        return;
    }

    std::vector<std::pair<int, Move>> scored_moves;
    scored_moves.reserve(state->legal_actions.size());

    for (const auto& move : state->legal_actions){
        int score = score_move(state, move);
        scored_moves.push_back({score, move});
    }
    // Sort moves in descending order based on score
    std::sort(scored_moves.begin(), scored_moves.end(),
        [](const std::pair<int, Move>& a, const std::pair<int, Move>& b){
            return a.first > b.first; // Sort in descending order
        }
    );
    // Replace the original legal_actions with the ordered moves
    state->legal_actions.clear();
    for (const auto& scored_move : scored_moves){
        state->legal_actions.push_back(scored_move.second);
    }
}
/*============================================================
 * PVS — eval_ctx
 *
 * Negamax with alpha-beta pruning. Caller manages memory.
 *============================================================*/
int PVS::eval_ctx(
    State *state,
    int depth,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p
){
    ctx.nodes++;
    if(ply > ctx.seldepth){
        ctx.seldepth = ply;
    }
    if(ctx.stop){
        return 0;
    }

    /* === Lazy move generation (sets game_state) === */
    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    /* === Terminal / leaf checks === */

    if (state->game_state == WIN){
        return P_MAX - ply;
    }
    if(state->game_state == DRAW){
        return 0;
    }

    /* === Repetition check (game-specific) === */
    int rep_score;
    if(state->check_repetition(history, rep_score)){
        return rep_score;
    }
    history.push(state->hash());

    /* === Leaf node evaluation === */
    if(depth <= 0){
        int score = state->evaluate(
            p.use_kp_eval, p.use_eval_mobility, &history
        ); 
        history.pop(state->hash());
        return score;
    }

    /* === Order Moves === */
    order_moves(state);

    /* === Negamax loop === */
    int best_score = M_MAX;
    bool first = true;

    for(auto& action : state->legal_actions){
        State *next = state->next_state(action);
        // 1. First priority: if the move is a winning move, we can immediately select it without searching further
        if (next->game_state == WIN) {} 
        else {
            // 2. Second priority: if the move is not a winning move, but it allows the opponent to win on their next turn, we should skip this move (prune it) since a rational opponent would never allow us to make such a blunder. This is a form of shallow pruning that can significantly reduce the search space.
            next->get_legal_actions();
            if (next->game_state == WIN) {
                delete next;
                continue;
            }
        }
        bool same = next->same_player_as_parent();
        int score;
        if (first){
            // PV node: search with full window
            int raw = eval_ctx(
                next,
                depth - 1,
                -beta,
                -alpha,
                history,
                ply + 1,
                ctx,
                p
            );
            score = same ? raw : -raw;
            first = false;
        }
        else{
            // Non-PV node: search with a null window first
            int raw = eval_ctx(
                next,
                depth - 1,
                -(alpha + 1),
                -alpha,
                history,
                ply + 1,
                ctx,
                p
            );
            score = same ? raw : -raw;
            // If this move looks interesting, research with full window
            if (score > alpha && score < beta){
                raw = eval_ctx(
                    next,
                    depth - 1,
                    -beta,
                    -alpha,
                    history,
                    ply + 1,
                    ctx,
                    p
                );
                score = same ? raw : -raw;
            }
            
        }

        delete next;

        if (score > best_score)
            best_score = score;
        if (best_score > alpha)
            alpha = best_score;
        if (alpha >= beta)
            break; // beta cut-off
        
    }

    history.pop(state->hash());
    return best_score;
}


/*============================================================
 * PVS — search
 *
 * Iterate legal moves, call eval_ctx, return SearchResult.
 *============================================================*/
SearchResult PVS::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    ctx.reset();
    MMParams p = MMParams::from_map(ctx.params);
    SearchResult result;
    result.depth = depth;

    if(!state->legal_actions.size()){
        state->get_legal_actions();
    }
    /* === Order Moves === */
    order_moves(state);

    // terminal check: no legal moves
    if (state->legal_actions.empty()){
        result.score = (state->game_state == DRAW) ? 0 : M_MAX - depth;
        return result;
    }
    int alpha = M_MAX;
    int beta = P_MAX;
    int best_score = M_MAX;
    int move_index = 0;
    int total_moves = (int)state->legal_actions.size();
    bool first = true;

    for(auto& action : state->legal_actions){
        State *next = state->next_state(action);
        // 1. First priority: if the move is a winning move, we can immediately select it without searching further
        if (next->game_state == WIN) {} 
        else {
            // 2. Second priority: if the move is not a winning move, but it allows the opponent to win on their next turn, we should skip this move (prune it) since a rational opponent would never allow us to make such a blunder. This is a form of shallow pruning that can significantly reduce the search space.
            next->get_legal_actions();
            if (next->game_state == WIN) {
                delete next;
                continue;
            }
        }
        bool same = next->same_player_as_parent();
        int score;
        if (first){
            // PV node: search with full window
            int raw = eval_ctx(
                next,
                depth - 1,
                -beta,
                -alpha,
                history,
                1,
                ctx,
                p
            );
            score = same ? raw : -raw;
            first = false;
        }
        else{
            // Non-PV node: search with a null window first
            int raw = eval_ctx(
                next,
                depth - 1,
                -(alpha + 1),
                -alpha,
                history,
                1,
                ctx,
                p
            );
            score = same ? raw : -raw;
            if (score > alpha && score < beta){
                raw = eval_ctx(
                    next,
                    depth - 1,
                    -beta,
                    -alpha,
                    history,
                    1,
                    ctx,
                    p
                );
                score = same ? raw : -raw;
            }
        }

        delete next;

        if(score > best_score){
            best_score = score;
            result.best_move = action;
            if(p.report_partial && ctx.on_root_update){
                ctx.on_root_update({result.best_move, best_score, depth, move_index + 1, total_moves});
            }
        }  
        if (best_score > alpha)
            alpha = best_score;
        move_index++;
    }
    result.score = best_score;
    return result;
} 


/*============================================================
 * PVS — default_params / param_defs
 *============================================================*/
ParamMap PVS::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
    };
}

std::vector<ParamDef> PVS::param_defs(){
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
    };
}
