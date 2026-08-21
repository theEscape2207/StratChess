#pragma once

#include "Utils/Subscriber.h"
#include "Move.h"
#include "GameState.h"
#include "SearchResult.h"
#include "SearchLimits.h"
#include <string>

class IPlayer {
  public:
	// Defaulted; Event<T>'s unordered_map default-ctor is noexcept.
	// NOLINTNEXTLINE(bugprone-exception-escape)
	IPlayer() noexcept = default;
	// Virtual functions
	virtual SearchResult GetMove(const SearchLimits& limits) = 0;
	//;virtual unsigned GetMaxDepth() = 0;
	virtual const char* GetType() const = 0;
	virtual std::string getDescription() const = 0;

	virtual bool IsHuman() const = 0;

	virtual ~IPlayer() = default;

	/*
	*	Events
	*/
	Event<const PVLine> ENewPVLineMove; // A new Move is added to the Principal variation line

	IPlayer(const IPlayer&) = delete;
	IPlayer& operator=(const IPlayer&) = delete;
	IPlayer(IPlayer&&) = delete;
	IPlayer& operator=(IPlayer&&) = delete;
};
