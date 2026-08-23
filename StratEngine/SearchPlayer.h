#pragma once

#include "AIPerplex.h"
#include "IPlayer.h"
#include "PlayerFactory.h"

#include <string>

class SearchPlayer final : public IPlayer {
  public:
	SearchPlayer(Board& board, AIPerplexConfig config, std::string description);
	SearchResult GetMove(const SearchLimits& limits) override;
	const char* GetType() const noexcept override;
	std::string getDescription() const override;
	bool IsHuman() const noexcept override { return false; }

  private:
	Board& board_;
	AIPerplex search_;
	const std::string description_;

	friend std::unique_ptr<IPlayer> CreatePlayer(const Config::PlayerConfig&, Board&, PlayerCreationOptions);
#ifdef STRAT_ENABLE_TEST_ACCESS
	friend class SearchPlayerTestFixture;
#endif
};
