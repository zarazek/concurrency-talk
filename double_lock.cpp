#include <mutex>
#include <iostream>

template <class Mutex>
void tryDoubleLock(const std::string& name, Mutex& m) {
  std::cout << "locking " << name << " first time" << std::endl;
  m.lock();
  std::cout << "locking " << name << " second time" << std::endl;
  m.lock();
  std::cout << name << " doubly locked" << std::endl;
  m.unlock();
  m.unlock();
}

int main(int, char**) {
  std::recursive_mutex m1;
  tryDoubleLock("recursive_mutex", m1);
  std::mutex m2;
  tryDoubleLock("mutex", m2);
  return 0;
}
