#pragma once

#include "Utils/Subscriber.h"
#include "Move.h"
#include "Eval.h"

class IPlayer
{
public:
	IPlayer() noexcept = default;
	// Virtual functions
	virtual Move GetMove(_Inout_ GameInfo& info ) = 0;
	//;virtual unsigned GetMaxDepth() = 0;
	virtual const char* GetType()const = 0;
	virtual std::string getDescription() const = 0;

	virtual int GetBestScore() const = 0;
	virtual bool IsHuman() const = 0;
	virtual void SetEvalEngine(EvalManager::EvalTypes) {	/* Overridden where needed (AI's) */	}

	virtual ~IPlayer() = default;

	/*
	*	Events
	*/
	Event<const PVLine> ENewPVLineMove; // A new Move is added to the Principal variation line
	Event<const GameStates> EGameStateChanged; // The Game State has changed

	IPlayer(const IPlayer&) = delete;
	IPlayer& operator=(const IPlayer&) = delete;
	IPlayer(IPlayer&&) = delete;
	IPlayer& operator=(IPlayer&&) = delete;
};
