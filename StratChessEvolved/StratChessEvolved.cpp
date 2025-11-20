// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Game.h"
#include "globals.h"

std::ofstream outLegalMoves("legalmoves.txt", std::ios::trunc | std::ios::out);

class Chess 
{
	// This Class does some configuration file handling and command line arguments processing.

public:
	Chess() = default;

protected:
	
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
		// Game is initialized - starting game
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
	Game game;
	game.Run();
	return 0;
}