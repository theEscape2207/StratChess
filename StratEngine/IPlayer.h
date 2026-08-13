#pragma once

#include "Utils/Subscriber.h"
#include "Move.h"
#include "Eval.h"
#include "GameState.h"
#include "SearchLimits.h"

class IPlayer {
  public:
	// NOLINTNEXTLINE(bugprone-exception-escape) - defaulted; the Event<T> members' unordered_map
	// default-constructs empty with std::allocator, which the standard requires to be noexcept.
	IPlayer() noexcept = default;
	// Virtual functions
	virtual Move GetMove(GameInfo& info, const SearchLimits& limits) = 0;
	//;virtual unsigned GetMaxDepth() = 0;
	virtual const char* GetType() const = 0;
	virtual std::string getDescription() const = 0;

	virtual int GetBestScore() const = 0;
	virtual bool IsHuman() const = 0;
	virtual void SetEvalEngine(EvalManager::EvalTypes) { /* Overridden where needed (AI's) */ }

	virtual ~IPlayer() = default;

	/*
	*	Events
	*/
	Event<const PVLine> ENewPVLineMove;        // A new Move is added to the Principal variation line
	Event<const GameStates> EGameStateChanged; // The Game State has changed

	IPlayer(const IPlayer&) = delete;
	IPlayer& operator=(const IPlayer&) = delete;
	IPlayer(IPlayer&&) = delete;
	IPlayer& operator=(IPlayer&&) = delete;
};
