#include <utility>
#include "state.hpp"
#include "alpha_beta.hpp"


/*============================================================
 * AlphaBeta — eval_ctx
 *
 * Negamax with alpha-beta pruning. Caller manages memory.
 *============================================================*/
int AlphaBeta::eval_ctx(
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

    if(depth <= 0){
        int score = state->evaluate(
            p.use_kp_eval, p.use_eval_mobility, &history
        ); 
        history.pop(state->hash());
        return score;
    }

    /* === Negamax loop === */
    int best_score = M_MAX;

    for(auto& action : state->legal_actions){
        State *next = state->next_state(action);
        bool same = next->same_player_as_parent();

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
        int score;
        if (same)
            score = raw;
        else
            score = -raw;

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
 * AlphaBeta — search
 *
 * Iterate legal moves, call eval_ctx, return SearchResult.
 *============================================================*/
SearchResult AlphaBeta::search(
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


    int best_score = M_MAX - 10;
    int move_index = 0;
    int total_moves = (int)state->legal_actions.size();

    for(auto& action : state->legal_actions){
        State *next = state->next_state(action);
        int raw = eval_ctx(
            next,
            depth - 1,
            M_MAX,
            P_MAX,
            history,
            1,
            ctx,
            p
        );
        int score;
        if (next->same_player_as_parent())
            score = raw;
        else
            score = -raw;
        delete next;
        if(score > best_score){
            best_score = score;
            result.best_move = action;
            if(p.report_partial && ctx.on_root_update){
                ctx.on_root_update({result.best_move, best_score, depth, move_index + 1, total_moves});
            }
        }  
        move_index++;
    }
    result.score = best_score;
    return result;
} 


/*============================================================
 * AlphaBeta — default_params / param_defs
 *============================================================*/
ParamMap AlphaBeta::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
    };
}

std::vector<ParamDef> AlphaBeta::param_defs(){
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
    };
}
