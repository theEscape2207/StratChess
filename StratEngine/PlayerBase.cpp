// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "PlayerBase.h"
// For factory constructor
#include "AIBasic.h"
#include "AIAgent.h"
#include "ABIterative.h"
#include "AIPerplex.h"
#include "PlayerHuman.h"

#include "MoveHelper.h"

// static Factory constructor
std::unique_ptr<PlayerBase> PlayerBase::Create(ePlayerTypes type, unsigned max_depth)
{
	switch (type) {
	case ePlayerTypes::HUMAN:				return std::make_unique<PlayerHuman>();		// no depth necessary
	case ePlayerTypes::ALPHABETA:			return std::make_unique<AIBasic>(max_depth);
	case ePlayerTypes::ABITERATING:			return std::make_unique<ABIterative>(max_depth);
	case ePlayerTypes::AIAGENT:				return std::make_unique<AIAgent>(max_depth);
	case ePlayerTypes::AITRANS:				throw std::invalid_argument("AITrans is archived (TT bugs). Use AI_PERPLEX instead.");
	case ePlayerTypes::ABITERATIVE_TRANS:	throw std::invalid_argument("ABIterTrans is archived (TT bugs). Use AI_PERPLEX instead.");
	case ePlayerTypes::AI_PERPLEX:			return std::make_unique<AIPerplex>(max_depth);
	default:				throw std::invalid_argument("Unknown Player type");	// Oops... we forgot an algo
	}
}