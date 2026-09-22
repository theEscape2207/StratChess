// UciWriter.h — the single channel every UCI protocol line leaves the engine through.
#pragma once

#include <functional>
#include <mutex>
#include <string_view>

// Owns UCI protocol output: a line sink plus the mutex that serialises writes to it. Constructed
// once per UciHandler and shared by shared_ptr with anything that must outlive the handler.
class UciWriter {
  public:
	// One complete protocol line, WITHOUT a trailing newline -- the sink owns framing.
	using LineSink = std::function<void(std::string_view)>;

	// Writes std::cout, flushing after every line.
	UciWriter();
	explicit UciWriter(LineSink sink);

	UciWriter(const UciWriter&) = delete;
	UciWriter& operator=(const UciWriter&) = delete;

	// One line per UCI protocol message is a hard requirement: a client reads stdout line by
	// line, and a line torn between two threads' partial writes is a protocol violation a match
	// runner resolves by forfeiting the game (issue #237 stage 0 finding). Every call is
	// serialised against every other send() on this writer via a member mutex.
	void send(std::string_view line);

  private:
	LineSink sink_;
	std::mutex mutex_;
};
