#include "StdAfx.h"
#include "SearchPlayer.h"

SearchPlayer::SearchPlayer(Board& board, AIPerplexConfig config, std::string description)
    : board_(board), search_(std::move(config)), description_(std::move(description))
{}

SearchResult SearchPlayer::GetMove(const SearchLimits& limits) { return search_.Search(board_, limits); }

const char* SearchPlayer::GetType() const noexcept { return "Perplexity Transpositional AlphaBeta"; }

std::string SearchPlayer::getDescription() const { return description_; }
