#pragma once

#include <functional>
#include <unordered_map>
#include <utility>

template <typename T> class Event {
  private:
	std::unordered_map<size_t, std::function<void(const void*, T&)>> callbacks;
	size_t next_id = 0;

  public:
	// Returns handle for unsubscription
	size_t subscribe(std::function<void(const void*, T&)> callback)
	{
		size_t id = next_id++;
		callbacks[id] = std::move(callback);
		return id;
	}

	void unsubscribe(size_t handle) { callbacks.erase(handle); }

	void fire(const void* sender, T& arg)
	{
		for (auto& [id, callback] : callbacks) {
			callback(sender, arg);
		}
	}

	void clear() { callbacks.clear(); }
};

class Source {
  public:
	Event<int> theEvent;

	void fireEvent(int n) { theEvent.fire(this, n); }
};