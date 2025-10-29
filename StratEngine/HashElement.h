#pragma once

#include "Move.h"

enum class eHashFlags 
{ 
	hashfNoValue=0, 
	hashfALPHA=1, 
	hashfBETA=2, 
	hashfEXACT=3
};

struct HashElement
{
	UINT64 key{ 0 };		// TODO: This key is unused
	
	eHashFlags hashflag{ eHashFlags::hashfNoValue };
	int iValue{ GameValues::Unknown_Hash };
	unsigned int iDepth{ 0 };
	Move BestMove;				// TODO: This BestMove is unused - and doesn't really work for PVL algorithms

	HashElement() noexcept = default; //, BestMove(Move())
	
	void Clear() noexcept
	{
		key = 0;
		iDepth = 0;
		iValue = GameValues::Unknown_Hash;
		hashflag = eHashFlags::hashfNoValue;
		BestMove.Clear();
	}
};
