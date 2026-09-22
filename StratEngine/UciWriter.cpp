// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "UciWriter.h"

UciWriter::UciWriter()
    : sink_([](std::string_view line) {
	      std::cout << line << '\n';
	      std::cout.flush();
      })
{}

UciWriter::UciWriter(LineSink sink) : sink_(std::move(sink)) {}

void UciWriter::send(std::string_view line)
{
	const std::lock_guard<std::mutex> lock(mutex_);
	sink_(line);
}
