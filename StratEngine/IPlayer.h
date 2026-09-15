#pragma once

#include "Move.h"
#include "GameState.h"
#include "SearchResult.h"
#include "SearchLimits.h"
#include <string>

class IPlayer {
  public:
	IPlayer() noexcept = default;
	// Virtual functions
	virtual SearchResult GetMove(const SearchLimits& limits) = 0;
	virtual const char* GetType() const = 0;
	virtual std::string getDescription() const = 0;

	virtual bool IsHuman() const = 0;

	virtual ~IPlayer() = default;

	IPlayer(const IPlayer&) = delete;
	IPlayer& operator=(const IPlayer&) = delete;
	IPlayer(IPlayer&&) = delete;
	IPlayer& operator=(IPlayer&&) = delete;
};
