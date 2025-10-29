// StratChessEvolved.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <memory>

// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Game.h"
#include "globals.h"


std::ofstream outFile2("SimplePerfStats.txt", std::ios::trunc | std::ios::out);
std::ofstream outLegalMoves("legalmoves.txt", std::ios::trunc | std::ios::out);

static void InitGameData()
{
	// Bare til en testfil
	outFile2 << "No. of nodes  |  Ms used  |  Nodes pr. ms  "
		<< "|  Total nodes  |  Total time  |  Total nodes pr. ms " << std::endl;
	outFile2 << "-------------------------------------------"
		<< "-----------------------------------------------------" << std::endl;



	//unsigned int i;
	//int j = 0;

	//// Foerst hestene
	//for (i = 0; i < ALL_SQUARES; ++i)
	//{
	//	g_bbKnightMoves[i] = 0;
	//	if (Rank(i) > 0)
	//	{
	//		if (Rank(i) > 1)
	//		{
	//			if (File(i) > 0)
	//				g_bbKnightMoves[i] += (UNIT << (i - 17));
	//			if (File(i) < 7)
	//				g_bbKnightMoves[i] += (UNIT << (i - 15));
	//		}
	//		if (File(i) > 1)
	//			g_bbKnightMoves[i] += (UNIT << (i - 10));
	//		if (File(i) < 6)
	//			g_bbKnightMoves[i] += (UNIT << (i - 6));
	//	}
	//	if (Rank(i) < 7)
	//	{
	//		if (Rank(i) < 6)
	//		{
	//			if (File(i) > 0)
	//				g_bbKnightMoves[i] += (UNIT << (i + 15));
	//			if (File(i) < 7)
	//				g_bbKnightMoves[i] += (UNIT << (i + 17));
	//		}
	//		if (File(i) > 1)
	//			g_bbKnightMoves[i] += (UNIT << (i + 6));
	//		if (File(i) < 6)
	//			g_bbKnightMoves[i] += (UNIT << (i + 10));
	//	}
	//}

	//// saa kongerne

	//for (i = 0; i < ALL_SQUARES; ++i)
	//{
	//	g_bbKingMoves[i] = 0;
	//	if (Rank(i) > 0)
	//	{
	//		if (File(i) > 0)
	//			g_bbKingMoves[i] += (UNIT << (i - 9));
	//		if (File(i) < 7)
	//			g_bbKingMoves[i] += (UNIT << (i - 7));
	//		g_bbKingMoves[i] += (UNIT << (i - 8));
	//	}
	//	if (Rank(i) < 7)
	//	{
	//		if (File(i) > 0)
	//			g_bbKingMoves[i] += (UNIT << (i + 7));
	//		if (File(i) < 7)
	//			g_bbKingMoves[i] += (UNIT << (i + 9));
	//		g_bbKingMoves[i] += (UNIT << (i + 8));
	//	}
	//	if (File(i) > 0)
	//		g_bbKingMoves[i] += (UNIT << (i - 1));
	//	if (File(i) < 7)
	//		g_bbKingMoves[i] += (UNIT << (i + 1));
	//}

	//// herefter taarne (og dronning) i den horizontale retning
	//unsigned int iFile = 0;
	//unsigned int iRank = 0;
	//BITBOARD bbMask = 0;
	//int x = 0;

	//for (iFile = 0; iFile < 8; ++iFile)
	//{
	//	for (j = 0; j < 256; ++j)
	//	{
	//		bbMask = 0;
	//		for (x = iFile - 1; x >= 0; --x)
	//		{
	//			bbMask += (UNIT << x);
	//			if (j & (1 << x))
	//				break;
	//		}
	//		for (x = iFile + 1; x < 8; ++x)
	//		{
	//			bbMask += (UNIT << x);
	//			if (j & (1 << x))
	//				break;
	//		}
	//		for (iRank = 0; iRank < 8; ++iRank)
	//			g_bbMovesRank[(iRank << 3) + iFile][j] = bbMask << (iRank << 3);
	//	}
	//}

	//// saa vertikalt

	//for (iRank = 0; iRank < 8; ++iRank)
	//{
	//	for (j = 0; j < 256; ++j)
	//	{
	//		bbMask = 0;
	//		for (x = 6 - iRank; x >= 0; --x)
	//		{
	//			bbMask += (UNIT << ((7 - x) << 3)); //-V629
	//			if (j & (1 << x))
	//				break;
	//		}
	//		for (x = 8 - iRank; x < 8; ++x)
	//		{
	//			bbMask += (UNIT << ((7 - x) << 3));
	//			if (j & (1 << x))
	//				break;
	//		}
	//		for (iFile = 0; iFile < 8; ++iFile)
	//			g_bbMovesFile[(iRank << 3) + iFile][j] = bbMask << iFile;
	//	}
	//}

	//// Calc possible moves for bishops (and queen)
	//// i diagonal-retningen a1-h8

	//unsigned int iDiagonalStart = 0;
	//unsigned int iDiagonalStartFile = 0;
	//unsigned int iDiagonalLength = 0;
	//BITBOARD bbMask2 = 0;

	//for (i = 0; i < ALL_SQUARES; ++i)
	//{
	//	// Find square furthest to the left for this diagonal
	//	iDiagonalStart = 7 * (std::min((File(i)), 7 - (Rank(i)))) + i;
	//	iDiagonalStartFile = File(iDiagonalStart);
	//	iDiagonalLength = g_iDiagonalLength_a1h8[i];
	//	iFile = File(i);

	//	if (1 <= iDiagonalLength && iDiagonalLength <= 8)
	//		return;

	//	// Looper gennem alle mulige placeringer paa denne diagonal
	//	for (j = 0; j < (1 << iDiagonalLength); ++j)
	//	{
	//		bbMask = bbMask2 = 0;
	//		// Beregner mulige maalfelter
	//		for (x = (iFile - iDiagonalStartFile) - 1; x >= 0; --x)
	//		{
	//			bbMask += (UNIT << x);
	//			if (j & (1 << x))
	//				break;
	//		}
	//		for (x = (iFile - iDiagonalStartFile) + 1; x < iDiagonalLength; ++x)
	//		{
	//			bbMask += (UNIT << x);
	//			if (j & (1 << x))
	//				break;
	//		}
	//		// Roterer noget tilbage
	//		for (x = 0; x < iDiagonalLength; ++x)
	//			bbMask2 += (((bbMask >> x) & 1) << (iDiagonalStart - (7 * x)));

	//		g_bbMovesa1h8[i][j] = bbMask2;
	//	}
	//}

	//// Possible moves for bishops (and queen) i diagonal-retningen a8-h1

	//for (i = 0; i < ALL_SQUARES; ++i)
	//{
	//	iDiagonalStart = i - 9 * (std::min((File(i)), (Rank(i))));
	//	iDiagonalStartFile = File(iDiagonalStart);
	//	iDiagonalLength = g_iDiagonalLength_a8h1[i];
	//	iFile = File(i);

	//	if (1 <= iDiagonalLength && iDiagonalLength <= 8)
	//		return;

	//	for (j = 0; j < (1 << iDiagonalLength); ++j)
	//	{
	//		bbMask = bbMask2 = 0;
	//		for (x = (iFile - iDiagonalStartFile) - 1; x >= 0; x--)
	//		{
	//			bbMask += (UNIT << x);
	//			if (j & (1 << x))
	//				break;
	//		}
	//		for (x = (iFile - iDiagonalStartFile) + 1; x < iDiagonalLength; ++x)
	//		{
	//			bbMask += (UNIT << x);
	//			if (j & (1 << x))
	//				break;
	//		}

	//		for (x = 0; x < iDiagonalLength; ++x)
	//			bbMask2 += (((bbMask >> x) & 1) << (iDiagonalStart + (9 * x)));

	//		g_bbMovesa8h1[i][j] = bbMask2;
	//	}
	//}

	///*	Her oprettes en tabel, der indeholder feltet, hvor den foerste
	//brik staar, for alle stillinger i to raekker						*/

	//for (i = 0; i < 65536; ++i)
	//{
	//	for (j = 0; j < 16; ++j)
	//	{
	//		if (i & (1 << j))
	//		{
	//			g_iFirstPiece[i] = j;
	//			break;
	//		}
	//	}
	//}
}

//#include "Poco/Util/Application.h"
//#include "Poco/Util/Option.h"
//#include "Poco/Util/OptionSet.h"
//#include "Poco/Util/HelpFormatter.h"
//#include "Poco/Util/AbstractConfiguration.h"
//#include "Poco/AutoPtr.h"
//
//#include <Poco/PatternFormatter.h>
//#include "Poco/FormattingChannel.h"
//#include "Poco/ConsoleChannel.h"
//#include <Poco/FileChannel.h>
//
//
//using Poco::Util::Application;
//using Poco::Util::Option;
//using Poco::Util::OptionSet;
//using Poco::Util::HelpFormatter;
//using Poco::Util::AbstractConfiguration;
//using Poco::Util::OptionCallback;

class Chess //: public Poco::Util::Application
{
	// This Class does some configuration file handling and command line arguments processing.

public:
	Chess() = default;

protected:
	//void initialize(Application& self) override
	//{
	//	loadConfiguration(); // load default configuration files, if present
	//	Application::initialize(self);


	//	// add your own initialization code here
	//	if (_helpRequested)
	//		return;

	//	InitLogging();
	//	InitGameData();

	//	spdlog::default_logger()->info("Data Tables initialized - starting game...");
	//}

	// FIXME: Never called
	void InitLogging()
	{
		//// set up two channel chains - one to the console and the other one to a log file. - "N: <msg>"
		//auto pFCConsole = new Poco::FormattingChannel(new Poco::PatternFormatter("%q: %t"));
		//pFCConsole->setChannel(new Poco::ConsoleChannel);	// FIXME: cppcoreguidelines-owning-memory
		//pFCConsole->open();

		////"%Y-%m-%d %H:%M:%S.%c :%s: %p: %t" -> 2006-12-31 00:03:52.1 :FileLogger: Information: <msg>
		//auto pFCFile = new Poco::FormattingChannel(new Poco::PatternFormatter("%Y-%m-%d %H:%M:%S.%i %Z :%p: %t"));
		//pFCFile->setChannel(new Poco::FileChannel("StratChess.log"));
		//pFCFile->open();

		//// create two Logger objects - one for each channel chain.
		///*Poco::Logger& consoleLogger =*/ Poco::Logger::create("ConsoleLogger", pFCConsole, Poco::Message::PRIO_INFORMATION);
		//Poco::Logger& fileLogger = Poco::Logger::create("FileLogger", pFCFile, Poco::Message::PRIO_TRACE);

		//// set default logger
		//setLogger(fileLogger);

		spdlog::default_logger()->info("Logging subsystem ready");
	}

	//void defineOptions(OptionSet& options) override
	//{
	//	Application::defineOptions(options);

	//	options.addOption(
	//		Option("help", "h", "display help information on command line arguments")
	//		.required(false)
	//		.repeatable(false)
	//		.callback(OptionCallback<Chess>(this, &Chess::handleHelp)));

	//	options.addOption(
	//		Option("define", "D", "define a configuration property")
	//		.required(false)
	//		.repeatable(true)
	//		.argument("name=value")
	//		.callback(OptionCallback<Chess>(this, &Chess::handleDefine)));

	//	options.addOption(
	//		Option("config-file", "f", "load configuration data from a file")
	//		.required(false)
	//		.repeatable(true)
	//		.argument("file")
	//		.callback(OptionCallback<Chess>(this, &Chess::handleConfig)));

	//	options.addOption(
	//		Option("bind", "b", "bind option value to test.property")
	//		.required(false)
	//		.repeatable(false)
	//		.argument("value")
	//		.binding("test.property"));
	//}

	//void handleHelp(const std::string& /*name*/, const std::string& /*value*/)
	//{
	//	_helpRequested = true;
	//	displayHelp();
	//	stopOptionsProcessing();
	//}

	//void handleDefine(const std::string& /*name*/, const std::string& value)
	//{
	//	defineProperty(value);
	//}

	//void handleConfig(const std::string& /*name*/, const std::string& value)
	//{
	//	loadConfiguration(value);
	//}

	//void displayHelp() const
	//{
	//	HelpFormatter helpFormatter(options());
	//	helpFormatter.setCommand(commandName());
	//	helpFormatter.setUsage("OPTIONS");
	//	helpFormatter.setHeader("StratChess engine");
	//	helpFormatter.setFooter("Developed by Sveinn Madvig Sigfredsson - Copyright 2001-2018");
	//	helpFormatter.setIndent(10);
	//	helpFormatter.format(std::cout);

	//	spdlog::default_logger()->info("Help called");
	//}

	//void defineProperty(const std::string& def) const
	//{
	//	std::string name;
	//	std::string value;
	//	const std::string::size_type pos = def.find('=');
	//	if (pos != std::string::npos)
	//	{
	//		name.assign(def, 0, pos);
	//		value.assign(def, pos + 1, def.length() - pos);
	//	}
	//	else
	//		name = def;
	//	config().setString(name, value);
	//}

	int main(const std::vector<std::string>& /*args*/)
	{
//#ifdef _DEBUG
//		if (!_helpRequested)
//		{
//			spdlog::default_logger()->info("Arguments to main():");
//			for (const auto& arg : args)
//			{
//				spdlog::default_logger()->info(arg);
//			}
//			spdlog::default_logger()->info("Application properties:");
//			printProperties("");
//		}
//#endif // _DEBUG

		// Game is initialized - starting game
		spdlog::default_logger()->info("Starting game...");
		Game theGame;
		theGame.Run();

		return 0;
	}

	void printProperties(const std::string& /*base*/) const
	{
		/*AbstractConfiguration::Keys keys;
		config().keys(base, keys);
		if (keys.empty())
		{
			if (config().hasProperty(base))
			{
				std::string msg;
				msg.append(base);
				msg.append(" = ");
				msg.append(config().getString(base));
				logger().information(msg);
			}
		}
		else
		{
			for (const auto& key : keys)
			{
				std::string fullKey = base;
				if (!fullKey.empty()) fullKey += '.';
				fullKey.append(key);
				printProperties(fullKey);
			}
		}*/
	}
public:
	Chess(const Chess&) = delete;
	Chess& operator=(const Chess&) = delete;

private:
	bool _helpRequested{ false };
};

int main(int /*argc*/, char** /*argv*/)
{
	std::cout << "Hello World!\n";

	spdlog::default_logger()->info("Starting game...");
	InitGameData();
	Game game;
	game.Run();
	return 0;
	/*try
	{
		game.init(argc, argv);
	}
	catch (const Poco::Exception& exc)
	{
		game.logger().log(exc);
		return Application::EXIT_CONFIG;
	}

	return game.run();*/
}